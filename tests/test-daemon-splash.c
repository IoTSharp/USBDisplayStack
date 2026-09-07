/* SPDX-License-Identifier: GPL-2.0-only */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define main usbdisplay_daemon_main
#include "../userspace/usb-displayd.c"
#undef main

struct capture_context {
	unsigned int expected_frames;
	unsigned int frames;
	struct usbdisplay_frame captured[3];
	unsigned char pixels[3][4096];
};

static int capture_submit(void *opaque, const struct usbdisplay_frame *frame)
{
	struct capture_context *context = opaque;
	unsigned int index = context->frames;
	int result = 0;

	if (index >= 3U || frame->bytes > sizeof(context->pixels[index])) {
		result = -EOVERFLOW;
	} else {
		context->captured[index] = *frame;
		memcpy(context->pixels[index], frame->pixels, frame->bytes);
		context->captured[index].pixels = context->pixels[index];
		context->frames = index + 1U;
		if (context->frames >= context->expected_frames) {
			stop_requested = 1;
		}
	}

	return result;
}

static void capture_close(void *opaque)
{
	(void)opaque;
}

static const struct usbdisplay_backend_v1 capture_backend = {
	.abi_version = USBDISPLAY_BACKEND_ABI_VERSION,
	.struct_size = sizeof(struct usbdisplay_backend_v1),
	.capabilities = USBDISPLAY_BACKEND_CAP_PHYSICAL,
	.name = "test-capture",
	.submit = capture_submit,
	.close = capture_close,
};

static const struct usbdisplay_backend_v1 diagnostic_backend = {
	.abi_version = USBDISPLAY_BACKEND_ABI_VERSION,
	.struct_size = sizeof(struct usbdisplay_backend_v1),
	.capabilities = USBDISPLAY_BACKEND_CAP_NONE,
	.name = "test-diagnostic",
	.submit = capture_submit,
	.close = capture_close,
};

static void fill_update(struct usbdisplay_update *update, uint32_t source,
			uint64_t sequence)
{
	memset(update, 0, sizeof(*update));
	update->sequence = sequence;
	update->timestamp_ns = sequence;
	update->slot = 0;
	update->width = 32U;
	update->height = 16U;
	update->stride = 32U * 4U;
	update->format = USBDISPLAY_FORMAT_XRGB8888;
	update->source = source;
	update->damage_width = update->width;
	update->damage_height = update->height;
}

static int write_updates(int descriptor, const struct usbdisplay_update *updates,
			 unsigned int count)
{
	unsigned int index;
	ssize_t written;
	int result = 0;

	for (index = 0; index < count && result == 0; ++index) {
		written = write(descriptor, &updates[index], sizeof(updates[index]));
		if (written != (ssize_t)sizeof(updates[index])) {
			result = -EIO;
		}
	}

	return result;
}

static int run_case(const struct usbdisplay_backend_v1 *backend,
		    const struct usbdisplay_update *updates, unsigned int update_count,
		    const unsigned char *mapping, size_t mapping_bytes,
		    unsigned int expected_frames, struct capture_context *capture)
{
	struct usbdisplay_device_info info;
	int descriptors[2] = {-1, -1};
	int result = 0;

	memset(capture, 0, sizeof(*capture));
	capture->expected_frames = expected_frames;
	memset(&info, 0, sizeof(info));
	info.abi_version = USBDISPLAY_ABI_VERSION;
	info.width = 32U;
	info.height = 16U;
	info.slot_count = 3U;
	info.slot_bytes = 32U * 16U * 4U;
	info.map_bytes = info.slot_count * info.slot_bytes;
	if (mapping_bytes < info.map_bytes || pipe(descriptors) != 0) {
		result = -EIO;
	} else {
		stop_requested = 0;
		result = write_updates(descriptors[1], updates, update_count);
		if (result == 0) {
			result = run_loop(descriptors[0], &info, mapping, backend, capture);
		}
	}
	if (descriptors[0] >= 0) {
		close(descriptors[0]);
	}
	if (descriptors[1] >= 0) {
		close(descriptors[1]);
	}

	return result;
}

static bool frame_matches(const struct usbdisplay_frame *frame,
			  const unsigned char *expected, size_t bytes)
{
	return frame->bytes == bytes &&
	       memcmp((const unsigned char *)frame->pixels, expected, bytes) == 0;
}

static int test_cases(void)
{
	unsigned char mapping[3U * 32U * 16U * 4U];
	uint32_t splash[32U * 16U];
	struct usbdisplay_update updates[2];
	struct capture_context capture;
	const struct usbdisplay_backend_v1 *backend;
	size_t splash_bytes;
	uint64_t started_ns;
	int result = 0;
	unsigned int index;

	for (index = 0; index < sizeof(mapping); ++index) {
		mapping[index] = (unsigned char)((index * 37U + 11U) & 0xffU);
	}
	splash_bytes = usbdisplay_splash_bytes(32U, 16U);
	if (splash_bytes != sizeof(splash) ||
	    usbdisplay_splash_render((uint32_t *)splash, 32U, 16U, 32U * 4U) != 0) {
		result = -EINVAL;
	}

	if (result == 0) {
		fill_update(&updates[0], USBDISPLAY_SOURCE_INITIAL, 1U);
		backend = &capture_backend;
		result = run_case(backend, updates, 1U, mapping, sizeof(mapping), 1U,
				  &capture);
		if (result == 0 && (capture.frames != 1U ||
			capture.captured[0].source != USBDISPLAY_SOURCE_INITIAL ||
			!frame_matches(&capture.captured[0], (const unsigned char *)splash,
				       splash_bytes))) {
			result = -EINVAL;
		}
	}

	if (result == 0) {
		fill_update(&updates[0], USBDISPLAY_SOURCE_INITIAL, 1U);
		started_ns = monotonic_nanoseconds();
		result = run_case(&capture_backend, updates, 1U, mapping,
				 sizeof(mapping), 3U, &capture);
		if (result == 0 && (capture.frames != 3U ||
			monotonic_nanoseconds() - started_ns > 1000000000ULL ||
			capture.captured[2].source != USBDISPLAY_SOURCE_INITIAL ||
			!frame_matches(&capture.captured[2], (const unsigned char *)splash,
				       splash_bytes))) {
			result = -EINVAL;
		}
	}

	if (result == 0) {
		fill_update(&updates[0], USBDISPLAY_SOURCE_INITIAL, 2U);
		fill_update(&updates[1], USBDISPLAY_SOURCE_FBDEV, 3U);
		result = run_case(&capture_backend, updates, 2U, mapping,
				sizeof(mapping), 2U, &capture);
		if (result == 0 && (capture.frames != 2U ||
			capture.captured[1].source != USBDISPLAY_SOURCE_FBDEV ||
			!frame_matches(&capture.captured[1], mapping,
				       32U * 16U * 4U))) {
			result = -EINVAL;
		}
	}

	if (result == 0) {
		fill_update(&updates[0], USBDISPLAY_SOURCE_DRM, 4U);
		result = run_case(&capture_backend, updates, 1U, mapping,
				 sizeof(mapping), 2U, &capture);
		if (result == 0 && (capture.frames != 2U ||
			capture.captured[1].source != USBDISPLAY_SOURCE_DRM ||
			!frame_matches(&capture.captured[1], mapping,
				       32U * 16U * 4U))) {
			result = -EINVAL;
		}
	}

	if (result == 0) {
		fill_update(&updates[0], USBDISPLAY_SOURCE_FBDEV, 5U);
		result = run_case(&capture_backend, updates, 1U, mapping,
				 sizeof(mapping), 2U, &capture);
		if (result == 0 && (capture.frames != 2U ||
			capture.captured[1].source != USBDISPLAY_SOURCE_FBDEV ||
			!frame_matches(&capture.captured[1], mapping,
				       32U * 16U * 4U))) {
			result = -EINVAL;
		}
	}

	if (result == 0) {
		fill_update(&updates[0], USBDISPLAY_SOURCE_INITIAL, 6U);
		result = run_case(&diagnostic_backend, updates, 1U, mapping,
				sizeof(mapping), 1U, &capture);
		if (result == 0 && (capture.frames != 1U ||
			!frame_matches(&capture.captured[0], (const unsigned char *)splash,
				       splash_bytes))) {
			result = -EINVAL;
		}
	}

	return result;
}

int main(void)
{
	int result = test_cases();

	if (result != 0) {
		fprintf(stderr, "daemon splash runtime test failed: %d\n", result);
	} else {
		printf("daemon splash runtime tests passed\n");
	}

	return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
