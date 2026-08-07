// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <usbdisplay/backend.h>

struct ppm_context {
	char *path;
	char *temporary_path;
};

static int ppm_open(const struct usbdisplay_backend_config *config,
		    void **context)
{
	const char *path = config->option != NULL ? config->option :
			   "/tmp/usbdisplay.ppm";
	struct ppm_context *state = NULL;
	size_t path_length;
	int result = 0;

	state = calloc(1, sizeof(*state));
	if (state == NULL) {
		result = -ENOMEM;
	} else {
		path_length = strlen(path);
		state->path = strdup(path);
		state->temporary_path = malloc(path_length + 5);
		if (state->path == NULL || state->temporary_path == NULL) {
			result = -ENOMEM;
		} else {
			snprintf(state->temporary_path, path_length + 5, "%s.tmp", path);
			*context = state;
		}
	}
	if (result != 0 && state != NULL) {
		free(state->temporary_path);
		free(state->path);
		free(state);
	}

	return result;
}

static void ppm_rgb565(uint16_t pixel, unsigned char rgb[3])
{
	rgb[0] = (unsigned char)(((pixel >> 11) & 0x1f) * 255 / 31);
	rgb[1] = (unsigned char)(((pixel >> 5) & 0x3f) * 255 / 63);
	rgb[2] = (unsigned char)((pixel & 0x1f) * 255 / 31);
}

static int ppm_write_pixels(FILE *stream, const struct usbdisplay_frame *frame)
{
	const unsigned char *row;
	unsigned char rgb[3];
	uint16_t pixel16;
	uint32_t pixel32;
	uint32_t x;
	uint32_t y;
	int result = 0;

	for (y = 0; y < frame->height && result == 0; ++y) {
		row = (const unsigned char *)frame->pixels + (size_t)y * frame->stride;
		for (x = 0; x < frame->width && result == 0; ++x) {
			if (frame->format == USBDISPLAY_FORMAT_XRGB8888) {
				memcpy(&pixel32, row + (size_t)x * 4, sizeof(pixel32));
				rgb[0] = (unsigned char)(pixel32 >> 16);
				rgb[1] = (unsigned char)(pixel32 >> 8);
				rgb[2] = (unsigned char)pixel32;
			} else if (frame->format == USBDISPLAY_FORMAT_RGB565) {
				memcpy(&pixel16, row + (size_t)x * 2, sizeof(pixel16));
				ppm_rgb565(pixel16, rgb);
			} else {
				result = -ENOTSUP;
			}
			if (result == 0 && fwrite(rgb, sizeof(rgb), 1, stream) != 1) {
				result = -EIO;
			}
		}
	}

	return result;
}

static int ppm_submit(void *context, const struct usbdisplay_frame *frame)
{
	struct ppm_context *state = context;
	FILE *stream;
	int saved_errno = 0;
	int result = 0;

	stream = fopen(state->temporary_path, "wb");
	if (stream == NULL) {
		result = -errno;
	} else {
		if (fprintf(stream, "P6\n%u %u\n255\n", frame->width,
			    frame->height) < 0) {
			result = -EIO;
		}
		if (result == 0) {
			result = ppm_write_pixels(stream, frame);
		}
		if (result == 0 && fflush(stream) != 0) {
			result = -errno;
		}
		if (result == 0 && fsync(fileno(stream)) != 0) {
			result = -errno;
		}
		if (fclose(stream) != 0 && result == 0) {
			result = -errno;
		}
		if (result == 0 && rename(state->temporary_path, state->path) != 0) {
			saved_errno = errno;
			result = -saved_errno;
		}
	}

	return result;
}

static void ppm_close(void *context)
{
	struct ppm_context *state = context;

	if (state != NULL) {
		free(state->temporary_path);
		free(state->path);
		free(state);
	}
}

static const struct usbdisplay_backend_v1 ppm_backend = {
	.abi_version = USBDISPLAY_BACKEND_ABI_VERSION,
	.struct_size = sizeof(struct usbdisplay_backend_v1),
	.capabilities = USBDISPLAY_BACKEND_CAP_NONE,
	.name = "ppm",
	.open = ppm_open,
	.submit = ppm_submit,
	.close = ppm_close,
};

const struct usbdisplay_backend_v1 *usbdisplay_backend_v1(void)
{
	return &ppm_backend;
}
