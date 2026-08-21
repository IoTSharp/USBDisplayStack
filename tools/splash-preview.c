// SPDX-License-Identifier: GPL-2.0-only

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../userspace/splash.h"

#define PREVIEW_WIDTH 960U
#define PREVIEW_HEIGHT 540U

/* 预览工具输出无压缩 PPM，便于在不依赖 LVGL 的构建机上审查实际像素。 */
int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "usbdisplay-splash.ppm";
	size_t bytes = usbdisplay_splash_bytes(PREVIEW_WIDTH, PREVIEW_HEIGHT);
	uint32_t *pixels = NULL;
	FILE *output = NULL;
	uint32_t color;
	unsigned int x;
	unsigned int y;
	int result = EXIT_SUCCESS;

	if (argc > 2 || bytes == 0) {
		fprintf(stderr, "Usage: %s [OUTPUT.ppm]\n", argv[0]);
		result = EXIT_FAILURE;
	} else {
		pixels = malloc(bytes);
		if (pixels == NULL ||
		    usbdisplay_splash_render(pixels, PREVIEW_WIDTH, PREVIEW_HEIGHT,
					     PREVIEW_WIDTH * 4U) != 0) {
			result = EXIT_FAILURE;
		}
	}
	if (result == EXIT_SUCCESS) {
		output = fopen(path, "wb");
		if (output == NULL ||
		    fprintf(output, "P6\n%u %u\n255\n", PREVIEW_WIDTH,
			    PREVIEW_HEIGHT) < 0) {
			result = EXIT_FAILURE;
		}
	}
	for (y = 0; y < PREVIEW_HEIGHT && result == EXIT_SUCCESS; ++y) {
		for (x = 0; x < PREVIEW_WIDTH && result == EXIT_SUCCESS; ++x) {
			color = pixels[(size_t)y * PREVIEW_WIDTH + x];
			if (fputc((int)((color >> 16) & 0xffU), output) == EOF ||
			    fputc((int)((color >> 8) & 0xffU), output) == EOF ||
			    fputc((int)(color & 0xffU), output) == EOF) {
				result = EXIT_FAILURE;
			}
		}
	}
	if (output != NULL && fclose(output) != 0) {
		result = EXIT_FAILURE;
	}
	free(pixels);

	return result;
}
