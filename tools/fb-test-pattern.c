// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t color_for_position(uint32_t x, uint32_t y,
				   uint32_t width, uint32_t height)
{
	static const uint32_t bars[] = {
		0x00ffffffU, 0x00ffff00U, 0x0000ffffU, 0x0000ff00U,
		0x00ff00ffU, 0x00ff0000U, 0x000000ffU, 0x00181818U,
	};
	uint32_t color = bars[((uint64_t)x * 8) / width];
	uint32_t checker;

	if (y > height * 3 / 4) {
		checker = ((x / 32) + (y / 32)) & 1U;
		color = checker != 0 ? 0x00f0f0f0U : 0x00202020U;
	}

	return color;
}

static uint16_t xrgb8888_to_rgb565(uint32_t color)
{
	uint16_t red = (uint16_t)((color >> 19) & 0x1f);
	uint16_t green = (uint16_t)((color >> 10) & 0x3f);
	uint16_t blue = (uint16_t)((color >> 3) & 0x1f);

	return (uint16_t)((red << 11) | (green << 5) | blue);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/fb1";
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	unsigned char *mapping = MAP_FAILED;
	size_t mapping_size = 0;
	uint32_t color;
	uint32_t x;
	uint32_t y;
	int descriptor = -1;
	int result = EXIT_FAILURE;

	descriptor = open(path, O_RDWR | O_CLOEXEC);
	if (descriptor < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
	} else if (ioctl(descriptor, FBIOGET_FSCREENINFO, &fixed) != 0 ||
		   ioctl(descriptor, FBIOGET_VSCREENINFO, &variable) != 0) {
		fprintf(stderr, "%s: framebuffer query failed: %s\n", path,
			strerror(errno));
	} else if (variable.bits_per_pixel != 16 &&
		   variable.bits_per_pixel != 32) {
		fprintf(stderr, "%s: unsupported bpp %u\n", path,
			variable.bits_per_pixel);
	} else {
		mapping_size = fixed.smem_len;
		mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
			       MAP_SHARED, descriptor, 0);
		if (mapping == MAP_FAILED) {
			fprintf(stderr, "%s: mmap failed: %s\n", path,
				strerror(errno));
		} else {
			for (y = 0; y < variable.yres; ++y) {
				for (x = 0; x < variable.xres; ++x) {
					color = color_for_position(x, y, variable.xres,
							   variable.yres);
					if (variable.bits_per_pixel == 32) {
						memcpy(mapping + (size_t)y * fixed.line_length +
						       (size_t)x * 4, &color, sizeof(color));
					} else {
						uint16_t color16 = xrgb8888_to_rgb565(color);
						memcpy(mapping + (size_t)y * fixed.line_length +
						       (size_t)x * 2, &color16, sizeof(color16));
					}
				}
			}
			if (msync(mapping, mapping_size, MS_SYNC) != 0) {
				fprintf(stderr, "%s: msync failed: %s\n", path,
					strerror(errno));
			} else {
				printf("wrote %ux%u %ubpp test pattern to %s\n",
				       variable.xres, variable.yres,
				       variable.bits_per_pixel, path);
				result = EXIT_SUCCESS;
			}
		}
	}

	if (mapping != MAP_FAILED) {
		munmap(mapping, mapping_size);
	}
	if (descriptor >= 0) {
		close(descriptor);
	}

	return result;
}
