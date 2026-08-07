// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <lvgl.h>

#include "../common/demo_ui.h"

#define DRAW_ROWS 64U

struct fbdev_target {
	int descriptor;
	unsigned char *mapping;
	size_t mapping_size;
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void fbdev_flush(lv_display_t *display, const lv_area_t *area,
			uint8_t *pixels)
{
	struct fbdev_target *target = lv_display_get_user_data(display);
	size_t row_bytes = (size_t)lv_area_get_width(area) * 4U;
	int32_t row;

	for (row = area->y1; row <= area->y2; ++row) {
		memcpy(target->mapping +
		       (size_t)(row + (int32_t)target->variable.yoffset) *
		       target->fixed.line_length +
		       (size_t)(area->x1 + (int32_t)target->variable.xoffset) * 4U,
		       pixels + (size_t)(row - area->y1) * row_bytes,
		       row_bytes);
	}
	if (msync(target->mapping, target->mapping_size, MS_ASYNC) != 0) {
		fprintf(stderr, "lvgl-fbdev: msync: %s\n", strerror(errno));
		stop_requested = 1;
	}
	lv_display_flush_ready(display);
}

static int open_target(const char *path, struct fbdev_target *target)
{
	int result = 0;

	memset(target, 0, sizeof(*target));
	target->descriptor = open(path, O_RDWR | O_CLOEXEC);
	if (target->descriptor < 0) {
		result = -errno;
	} else if (ioctl(target->descriptor, FBIOGET_FSCREENINFO,
			 &target->fixed) != 0 ||
		   ioctl(target->descriptor, FBIOGET_VSCREENINFO,
			 &target->variable) != 0) {
		result = -errno;
	} else if (strncmp(target->fixed.id, "usbdisplay",
			   sizeof(target->fixed.id)) != 0) {
		result = -ENODEV;
	} else if (target->variable.bits_per_pixel != 32 ||
		   target->variable.red.offset != 16 ||
		   target->variable.green.offset != 8 ||
		   target->variable.blue.offset != 0) {
		result = -ENOTSUP;
	} else {
		target->mapping_size = target->fixed.smem_len;
		target->mapping = mmap(NULL, target->mapping_size,
				       PROT_READ | PROT_WRITE, MAP_SHARED,
				       target->descriptor, 0);
		if (target->mapping == MAP_FAILED) {
			target->mapping = NULL;
			result = -errno;
		}
	}
	if (result != 0) {
		fprintf(stderr, "lvgl-fbdev: %s: %s\n", path,
			strerror(-result));
	}

	return result;
}

static void close_target(struct fbdev_target *target)
{
	if (target->mapping != NULL) {
		munmap(target->mapping, target->mapping_size);
	}
	if (target->descriptor >= 0) {
		close(target->descriptor);
	}
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/fb1";
	struct fbdev_target target;
	lv_display_t *display = NULL;
	void *draw_buffer1 = NULL;
	void *draw_buffer2 = NULL;
	size_t draw_bytes;
	uint32_t delay_ms;
	int result;

	target.descriptor = -1;
	if (argc > 2) {
		fprintf(stderr, "Usage: %s [FBDEV]\n", argv[0]);
		return EXIT_FAILURE;
	}
	result = open_target(path, &target);
	if (result == 0) {
		lv_init();
		lv_tick_set_cb(usbdisplay_demo_ticks);
		draw_bytes = (size_t)target.variable.xres * DRAW_ROWS * 4U;
		draw_buffer1 = malloc(draw_bytes);
		draw_buffer2 = malloc(draw_bytes);
		if (draw_buffer1 == NULL || draw_buffer2 == NULL) {
			result = -ENOMEM;
		}
	}
	if (result == 0) {
		display = lv_display_create(target.variable.xres,
					    target.variable.yres);
		if (display == NULL) {
			result = -ENOMEM;
		} else {
			lv_display_set_color_format(display,
						LV_COLOR_FORMAT_XRGB8888);
			lv_display_set_user_data(display, &target);
			lv_display_set_flush_cb(display, fbdev_flush);
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
		fprintf(stderr, "lvgl-fbdev: %s\n", strerror(-result));
	}

	return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
