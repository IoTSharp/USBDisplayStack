// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <lvgl.h>

#include "../common/demo_ui.h"

#define DRAW_ROWS 64U

struct drm_target {
	int descriptor;
	uint32_t connector_id;
	uint32_t crtc_id;
	uint32_t framebuffer_id;
	drmModeModeInfo mode;
	struct drm_mode_create_dumb dumb;
	unsigned char *mapping;
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int verify_driver(int descriptor)
{
	drmVersionPtr version = drmGetVersion(descriptor);
	int result = 0;

	if (version == NULL) {
		result = -errno;
	} else if (version->name_len != strlen("usbdisplay") ||
		   memcmp(version->name, "usbdisplay", version->name_len) != 0) {
		result = -ENODEV;
	}
	if (version != NULL) {
		drmFreeVersion(version);
	}

	return result;
}

static int select_connector(struct drm_target *target)
{
	drmModeRes *resources = drmModeGetResources(target->descriptor);
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	uint32_t possible_crtcs = 0;
	int resource_index;
	int connector_index;
	int encoder_index;
	int mode_index;
	int result = -ENODEV;

	if (resources == NULL) {
		return -errno;
	}
	for (connector_index = 0;
	     connector_index < resources->count_connectors && result != 0;
	     ++connector_index) {
		connector = drmModeGetConnector(target->descriptor,
				resources->connectors[connector_index]);
		if (connector == NULL || connector->connection != DRM_MODE_CONNECTED ||
		    connector->count_modes == 0) {
			drmModeFreeConnector(connector);
			connector = NULL;
			continue;
		}
		target->connector_id = connector->connector_id;
		target->mode = connector->modes[0];
		for (mode_index = 0; mode_index < connector->count_modes;
		     ++mode_index) {
			if ((connector->modes[mode_index].type &
			     DRM_MODE_TYPE_PREFERRED) != 0) {
				target->mode = connector->modes[mode_index];
				break;
			}
		}
		if (connector->encoder_id != 0) {
			encoder = drmModeGetEncoder(target->descriptor,
						    connector->encoder_id);
		}
		if (encoder != NULL && encoder->crtc_id != 0) {
			target->crtc_id = encoder->crtc_id;
			result = 0;
		} else {
			for (encoder_index = 0;
			     encoder_index < connector->count_encoders && result != 0;
			     ++encoder_index) {
				drmModeFreeEncoder(encoder);
				encoder = drmModeGetEncoder(target->descriptor,
					connector->encoders[encoder_index]);
				if (encoder != NULL) {
					possible_crtcs = encoder->possible_crtcs;
					for (resource_index = 0;
					     resource_index < resources->count_crtcs;
					     ++resource_index) {
						if ((possible_crtcs &
						     (1U << resource_index)) != 0) {
							target->crtc_id =
								resources->crtcs[resource_index];
							result = 0;
							break;
						}
					}
				}
			}
		}
	}
	drmModeFreeEncoder(encoder);
	drmModeFreeConnector(connector);
	drmModeFreeResources(resources);

	return result;
}

static int create_framebuffer(struct drm_target *target)
{
	struct drm_mode_map_dumb map;
	uint32_t handles[4] = {0};
	uint32_t pitches[4] = {0};
	uint32_t offsets[4] = {0};
	int result = 0;

	memset(&target->dumb, 0, sizeof(target->dumb));
	target->dumb.width = target->mode.hdisplay;
	target->dumb.height = target->mode.vdisplay;
	target->dumb.bpp = 32;
	if (drmIoctl(target->descriptor, DRM_IOCTL_MODE_CREATE_DUMB,
		     &target->dumb) != 0) {
		result = -errno;
	} else {
		handles[0] = target->dumb.handle;
		pitches[0] = target->dumb.pitch;
		if (drmModeAddFB2(target->descriptor, target->mode.hdisplay,
				  target->mode.vdisplay, DRM_FORMAT_XRGB8888,
				  handles, pitches, offsets,
				  &target->framebuffer_id, 0) != 0) {
			result = -errno;
		}
	}
	if (result == 0) {
		memset(&map, 0, sizeof(map));
		map.handle = target->dumb.handle;
		if (drmIoctl(target->descriptor, DRM_IOCTL_MODE_MAP_DUMB,
			     &map) != 0) {
			result = -errno;
		} else {
			target->mapping = mmap(NULL, target->dumb.size,
					       PROT_READ | PROT_WRITE, MAP_SHARED,
					       target->descriptor, map.offset);
			if (target->mapping == MAP_FAILED) {
				target->mapping = NULL;
				result = -errno;
			}
		}
	}
	if (result == 0 &&
	    drmModeSetCrtc(target->descriptor, target->crtc_id,
			   target->framebuffer_id, 0, 0,
			   &target->connector_id, 1, &target->mode) != 0) {
		result = -errno;
	}

	return result;
}

static int open_target(const char *path, struct drm_target *target)
{
	int result;

	memset(target, 0, sizeof(*target));
	target->descriptor = open(path, O_RDWR | O_CLOEXEC);
	if (target->descriptor < 0) {
		result = -errno;
	} else {
		result = verify_driver(target->descriptor);
	}
	if (result == 0) {
		result = select_connector(target);
	}
	if (result == 0) {
		result = create_framebuffer(target);
	}
	if (result != 0) {
		fprintf(stderr, "lvgl-drm: %s: %s\n", path, strerror(-result));
	}

	return result;
}

static void close_target(struct drm_target *target)
{
	struct drm_mode_destroy_dumb destroy;

	if (target->descriptor >= 0 && target->crtc_id != 0) {
		drmModeSetCrtc(target->descriptor, target->crtc_id, 0, 0, 0,
			       NULL, 0, NULL);
	}
	if (target->mapping != NULL) {
		munmap(target->mapping, target->dumb.size);
	}
	if (target->descriptor >= 0 && target->framebuffer_id != 0) {
		drmModeRmFB(target->descriptor, target->framebuffer_id);
	}
	if (target->descriptor >= 0 && target->dumb.handle != 0) {
		memset(&destroy, 0, sizeof(destroy));
		destroy.handle = target->dumb.handle;
		drmIoctl(target->descriptor, DRM_IOCTL_MODE_DESTROY_DUMB,
			 &destroy);
	}
	if (target->descriptor >= 0) {
		close(target->descriptor);
	}
}

static void drm_flush(lv_display_t *display, const lv_area_t *area,
		      uint8_t *pixels)
{
	struct drm_target *target = lv_display_get_user_data(display);
	drmModeClip clip;
	size_t row_bytes = (size_t)lv_area_get_width(area) * 4U;
	int32_t row;

	for (row = area->y1; row <= area->y2; ++row) {
		memcpy(target->mapping + (size_t)row * target->dumb.pitch +
		       (size_t)area->x1 * 4U,
		       pixels + (size_t)(row - area->y1) * row_bytes,
		       row_bytes);
	}
	clip.x1 = (uint16_t)area->x1;
	clip.y1 = (uint16_t)area->y1;
	clip.x2 = (uint16_t)(area->x2 + 1);
	clip.y2 = (uint16_t)(area->y2 + 1);
	if (drmModeDirtyFB(target->descriptor, target->framebuffer_id,
			   &clip, 1) != 0) {
		fprintf(stderr, "lvgl-drm: dirty framebuffer: %s\n",
			strerror(errno));
		stop_requested = 1;
	}
	lv_display_flush_ready(display);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/card1";
	struct drm_target target;
	lv_display_t *display = NULL;
	void *draw_buffer1 = NULL;
	void *draw_buffer2 = NULL;
	size_t draw_bytes;
	uint32_t delay_ms;
	int result;

	target.descriptor = -1;
	if (argc > 2) {
		fprintf(stderr, "Usage: %s [DRM_CARD]\n", argv[0]);
		return EXIT_FAILURE;
	}
	result = open_target(path, &target);
	if (result == 0) {
		lv_init();
		lv_tick_set_cb(usbdisplay_demo_ticks);
		draw_bytes = (size_t)target.mode.hdisplay * DRAW_ROWS * 4U;
		draw_buffer1 = malloc(draw_bytes);
		draw_buffer2 = malloc(draw_bytes);
		if (draw_buffer1 == NULL || draw_buffer2 == NULL) {
			result = -ENOMEM;
		}
	}
	if (result == 0) {
		display = lv_display_create(target.mode.hdisplay,
					    target.mode.vdisplay);
		if (display == NULL) {
			result = -ENOMEM;
		} else {
			lv_display_set_color_format(display,
						LV_COLOR_FORMAT_XRGB8888);
			lv_display_set_user_data(display, &target);
			lv_display_set_flush_cb(display, drm_flush);
			lv_display_set_buffers(display, draw_buffer1, draw_buffer2,
					       draw_bytes,
					       LV_DISPLAY_RENDER_MODE_PARTIAL);
			usbdisplay_create_demo_ui();
		}
	}
	if (result == 0) {
		signal(SIGINT, handle_signal);
		signal(SIGTERM, handle_signal);
		while (!stop_requested) {
			delay_ms = lv_timer_handler();
			if (delay_ms < 5U) {
				delay_ms = 5U;
			} else if (delay_ms > 50U) {
				delay_ms = 50U;
			}
			usleep(delay_ms * 1000U);
		}
	}
	if (display != NULL) {
		lv_display_delete(display);
	}
	free(draw_buffer2);
	free(draw_buffer1);
	close_target(&target);
	if (result != 0) {
		fprintf(stderr, "lvgl-drm: %s\n", strerror(-result));
	}

	return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
