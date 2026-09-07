/* SPDX-License-Identifier: GPL-2.0-only */

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t test_now_ns;
static unsigned int test_polls;

static int test_clock_gettime(clockid_t clock_id, struct timespec *value)
{
	(void)clock_id;
	value->tv_sec = (time_t)(test_now_ns / 1000000000ULL);
	value->tv_nsec = (long)(test_now_ns % 1000000000ULL);
	return 0;
}

/* Preserve pipe readiness while making idle waits deterministic and bounded. */
static int test_poll(struct pollfd *descriptors, nfds_t count, int timeout_ms)
{
	int result;

	if (++test_polls > 1000U || test_now_ns > 20000000000ULL) {
		errno = ETIMEDOUT;
		return -1;
	}
	result = poll(descriptors, count, 0);
	if (result == 0 && timeout_ms > 0) {
		test_now_ns += (uint64_t)timeout_ms * 1000000ULL;
	}
	return result;
}

#define clock_gettime test_clock_gettime
#define poll test_poll
#define main usbdisplay_daemon_main
#include "../userspace/usb-displayd.c"
#undef main
#undef poll
#undef clock_gettime

#define TEST_FRAME_BYTES (32U * 16U * 4U)
#define TEST_MAX_FRAMES 128U

struct capture_context {
	unsigned int expected_frames;
	unsigned int expected_application_frames;
	unsigned int frames;
	unsigned int application_frames;
	unsigned int ticks_during_hold;
	unsigned int first_application_index;
	uint64_t first_submit_delay_ns;
	uint64_t startup_ready_ns;
	bool inject_updates;
	bool hold_updates_injected;
	bool application_update_injected;
	int update_descriptor;
	struct usbdisplay_frame captured[TEST_MAX_FRAMES];
	unsigned char pixels[TEST_MAX_FRAMES][TEST_FRAME_BYTES];
	uint64_t submitted_ns[TEST_MAX_FRAMES];
};

static void fill_update(struct usbdisplay_update *update, uint32_t source,
			uint64_t sequence, uint32_t slot)
{
	memset(update, 0, sizeof(*update));
	update->sequence = sequence;
	update->timestamp_ns = sequence;
	update->slot = slot;
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
	int result = count > 3U ? -EOVERFLOW : 0;

	for (index = 0; index < count && result == 0; ++index) {
		written = write(descriptor, &updates[index], sizeof(updates[index]));
		if (written != (ssize_t)sizeof(updates[index])) {
			result = -EIO;
		}
	}
	return result;
}

static int capture_submit(void *opaque, const struct usbdisplay_frame *frame)
{
	struct capture_context *context = opaque;
	unsigned int index = context->frames;

	if (index >= TEST_MAX_FRAMES || frame->bytes > sizeof(context->pixels[index])) {
		return -EOVERFLOW;
	}
	if (index == 0U) {
		test_now_ns += context->first_submit_delay_ns;
		context->startup_ready_ns = test_now_ns;
	}
	context->captured[index] = *frame;
	memcpy(context->pixels[index], frame->pixels, frame->bytes);
	context->captured[index].pixels = context->pixels[index];
	context->submitted_ns[index] = test_now_ns;
	context->frames = index + 1U;
	if (frame->source != USBDISPLAY_SOURCE_INITIAL) {
		if (context->application_frames == 0U) {
			context->first_application_index = index;
		}
		++context->application_frames;
	}
	if ((context->expected_frames != 0U &&
	     context->frames >= context->expected_frames) ||
	    (context->expected_application_frames != 0U &&
	     context->application_frames >= context->expected_application_frames)) {
		handle_signal(SIGTERM);
	}
	return 0;
}

static int capture_tick(void *opaque, uint64_t now_ns)
{
	struct capture_context *context = opaque;
	struct usbdisplay_update updates[2];
	int result = 0;

	if (context->application_frames == 0U) {
		++context->ticks_during_hold;
	}
	if (context->inject_updates && !context->hold_updates_injected &&
	    now_ns - context->startup_ready_ns >= 1000000000ULL) {
		fill_update(&updates[0], USBDISPLAY_SOURCE_DRM, 21U, 1U);
		fill_update(&updates[1], USBDISPLAY_SOURCE_FBDEV, 22U, 2U);
		updates[1].damage_x = 2U;
		updates[1].damage_y = 3U;
		updates[1].damage_width = 4U;
		updates[1].damage_height = 5U;
		result = write_updates(context->update_descriptor, updates, 2U);
		context->hold_updates_injected = true;
	}
	if (result == 0 && context->inject_updates &&
	    context->application_frames == 1U && !context->application_update_injected) {
		fill_update(&updates[0], USBDISPLAY_SOURCE_DRM, 23U, 1U);
		result = write_updates(context->update_descriptor, updates, 1U);
		context->application_update_injected = true;
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
	.capabilities = USBDISPLAY_BACKEND_CAP_PHYSICAL | USBDISPLAY_BACKEND_CAP_TICK,
	.name = "test-capture",
	.submit = capture_submit,
	.close = capture_close,
	.tick = capture_tick,
};

static const struct usbdisplay_backend_v1 diagnostic_backend = {
	.abi_version = USBDISPLAY_BACKEND_ABI_VERSION,
	.struct_size = sizeof(struct usbdisplay_backend_v1),
	.capabilities = USBDISPLAY_BACKEND_CAP_NONE,
	.name = "test-diagnostic",
	.submit = capture_submit,
	.close = capture_close,
};

static int run_case(const struct usbdisplay_backend_v1 *backend,
		    const struct usbdisplay_update *updates, unsigned int update_count,
		    const unsigned char *mapping, struct capture_context *capture)
{
	struct usbdisplay_device_info info;
	int descriptors[2] = {-1, -1};
	int result = 0;

	memset(&info, 0, sizeof(info));
	info.abi_version = USBDISPLAY_ABI_VERSION;
	info.width = 32U;
	info.height = 16U;
	info.slot_count = 3U;
	info.slot_bytes = TEST_FRAME_BYTES;
	info.map_bytes = info.slot_count * info.slot_bytes;
	if (pipe(descriptors) != 0) {
		result = -EIO;
	} else {
		stop_requested = 0;
		test_now_ns = 1000000000ULL;
		test_polls = 0U;
		capture->update_descriptor = descriptors[1];
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
			  const unsigned char *expected)
{
	return frame->bytes == TEST_FRAME_BYTES &&
	       memcmp(frame->pixels, expected, TEST_FRAME_BYTES) == 0;
}

static int test_cases(void)
{
	unsigned char mapping[3U * TEST_FRAME_BYTES];
	uint32_t splash[32U * 16U];
	struct usbdisplay_update updates[2];
	struct capture_context capture;
	unsigned int index;
	unsigned int app_index;
	int result = 0;

	for (index = 0; index < sizeof(mapping); ++index) {
		mapping[index] = (unsigned char)((index * 37U + index / TEST_FRAME_BYTES + 11U) & 0xffU);
	}
	if (usbdisplay_splash_bytes(32U, 16U) != sizeof(splash) ||
	    usbdisplay_splash_render(splash, 32U, 16U, 32U * 4U) != 0) {
		return -EINVAL;
	}

	/* A signal cancels the hold promptly, even with an application frame pending. */
	memset(&capture, 0, sizeof(capture));
	capture.expected_frames = 3U;
	fill_update(&updates[0], USBDISPLAY_SOURCE_INITIAL, 1U, 0U);
	fill_update(&updates[1], USBDISPLAY_SOURCE_FBDEV, 2U, 0U);
	result = run_case(&capture_backend, updates, 2U, mapping, &capture);
	if (result == 0 && (capture.frames != 3U || capture.application_frames != 0U ||
	    test_now_ns - capture.startup_ready_ns >= 2000000000ULL ||
	    !frame_matches(&capture.captured[2], (const unsigned char *)splash))) {
		result = -EINVAL;
	}

	/* INITIAL releases an earlier pending slot, so it must never reach the backend. */
	if (result == 0) {
		memset(&capture, 0, sizeof(capture));
		capture.expected_frames = 70U;
		fill_update(&updates[0], USBDISPLAY_SOURCE_FBDEV, 10U, 0U);
		fill_update(&updates[1], USBDISPLAY_SOURCE_INITIAL, 11U, 0U);
		result = run_case(&capture_backend, updates, 2U, mapping, &capture);
		if (result == 0 && (capture.application_frames != 0U ||
		    test_now_ns - capture.startup_ready_ns < 2000000000ULL ||
		    !frame_matches(&capture.captured[69], (const unsigned char *)splash))) {
			result = -EINVAL;
		}
	}

	/* A slow initial submit must finish before the two-second display interval. */
	if (result == 0) {
		memset(&capture, 0, sizeof(capture));
		capture.expected_application_frames = 2U;
		capture.first_submit_delay_ns = 3000000000ULL;
		capture.inject_updates = true;
		fill_update(&updates[0], USBDISPLAY_SOURCE_FBDEV, 20U, 0U);
		result = run_case(&capture_backend, updates, 1U, mapping, &capture);
		app_index = capture.first_application_index;
		if (result == 0 && (capture.application_frames != 2U || app_index < 30U ||
		    capture.ticks_during_hold < 30U || !capture.hold_updates_injected ||
		    capture.submitted_ns[app_index] - capture.startup_ready_ns < 2000000000ULL ||
		    capture.submitted_ns[app_index] - capture.startup_ready_ns > 2100000000ULL ||
		    capture.captured[app_index].sequence != 22U ||
		    capture.captured[app_index].source != USBDISPLAY_SOURCE_FBDEV ||
		    capture.captured[app_index].damage_x != 0U ||
		    capture.captured[app_index].damage_y != 0U ||
		    capture.captured[app_index].damage_width != 32U ||
		    capture.captured[app_index].damage_height != 16U ||
		    !frame_matches(&capture.captured[app_index], mapping + 2U * TEST_FRAME_BYTES) ||
		    capture.captured[app_index + 1U].sequence != 23U ||
		    capture.captured[app_index + 1U].source != USBDISPLAY_SOURCE_DRM ||
		    !frame_matches(&capture.captured[app_index + 1U], mapping + TEST_FRAME_BYTES) ||
		    capture.submitted_ns[app_index + 1U] - capture.submitted_ns[app_index] > 100000000ULL)) {
			result = -EINVAL;
		}
		for (index = 0; index < app_index && result == 0; ++index) {
			if (capture.captured[index].source != USBDISPLAY_SOURCE_INITIAL ||
			    !frame_matches(&capture.captured[index], (const unsigned char *)splash) ||
			    (index > 0U && capture.submitted_ns[index] -
			     capture.submitted_ns[index - 1U] > 34000000ULL)) {
				result = -EINVAL;
			}
		}
	}

	/* A retained DRM frame also receives the generation-start splash. */
	if (result == 0) {
		memset(&capture, 0, sizeof(capture));
		capture.expected_application_frames = 1U;
		fill_update(&updates[0], USBDISPLAY_SOURCE_DRM, 30U, 1U);
		result = run_case(&capture_backend, updates, 1U, mapping, &capture);
		app_index = capture.first_application_index;
		if (result == 0 && (capture.captured[app_index].source != USBDISPLAY_SOURCE_DRM ||
		    !frame_matches(&capture.captured[app_index], mapping + TEST_FRAME_BYTES) ||
		    capture.submitted_ns[app_index] - capture.startup_ready_ns < 2000000000ULL)) {
			result = -EINVAL;
		}
	}

	/* Diagnostic backends preserve the splash but never delay application updates. */
	if (result == 0) {
		memset(&capture, 0, sizeof(capture));
		capture.expected_application_frames = 1U;
		fill_update(&updates[0], USBDISPLAY_SOURCE_INITIAL, 40U, 0U);
		fill_update(&updates[1], USBDISPLAY_SOURCE_FBDEV, 41U, 0U);
		result = run_case(&diagnostic_backend, updates, 2U, mapping, &capture);
		if (result == 0 && (capture.frames != 2U ||
		    !frame_matches(&capture.captured[0], (const unsigned char *)splash) ||
		    !frame_matches(&capture.captured[1], mapping) ||
		    capture.submitted_ns[1] != capture.startup_ready_ns)) {
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
