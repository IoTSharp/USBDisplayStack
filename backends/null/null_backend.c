// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <usbdisplay/backend.h>

struct null_context {
	uint64_t frames;
};

static int null_open(const struct usbdisplay_backend_config *config,
		     void **context)
{
	struct null_context *state;

	(void)config;
	state = calloc(1, sizeof(*state));
	if (state == NULL) {
		return -ENOMEM;
	}
	*context = state;

	return 0;
}

static int null_submit(void *context, const struct usbdisplay_frame *frame)
{
	struct null_context *state = context;
	const uint8_t *bytes = frame->pixels;
	uint32_t checksum = 2166136261U;
	size_t index;

	for (index = 0; index < frame->bytes; index += 4096) {
		checksum = (checksum ^ bytes[index]) * 16777619U;
	}
	++state->frames;
	if (state->frames == 1 || state->frames % 30 == 0) {
		fprintf(stderr,
			"null: frame=%" PRIu64 " sequence=%" PRIu64
			" format=%u checksum=%08x\n",
			state->frames, frame->sequence, frame->format, checksum);
	}

	return 0;
}

static void null_close(void *context)
{
	free(context);
}

static const struct usbdisplay_backend_v1 null_backend = {
	.abi_version = USBDISPLAY_BACKEND_ABI_VERSION,
	.struct_size = sizeof(struct usbdisplay_backend_v1),
	.capabilities = USBDISPLAY_BACKEND_CAP_NONE,
	.name = "null",
	.open = null_open,
	.submit = null_submit,
	.close = null_close,
};

const struct usbdisplay_backend_v1 *usbdisplay_backend_v1(void)
{
	return &null_backend;
}
