// SPDX-License-Identifier: GPL-2.0-only

#include "splash.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SPLASH_LOGICAL_WIDTH 960U
#define SPLASH_LOGICAL_HEIGHT 540U

#define COLOR_BACKGROUND 0x000b1115U
#define COLOR_PANEL 0x00121c22U
#define COLOR_LINE 0x00213b47U
#define COLOR_CYAN 0x003ac7f2U
#define COLOR_GREEN 0x006bd146U
#define COLOR_WHITE 0x00f4f7f8U
#define COLOR_MUTED 0x0087a1aeU

struct splash_surface {
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
};

struct splash_glyph {
	char character;
	uint8_t rows[7];
};

static const struct splash_glyph splash_glyphs[] = {
	{' ', {0, 0, 0, 0, 0, 0, 0}},
	{'-', {0, 0, 0, 31, 0, 0, 0}},
	{'.', {0, 0, 0, 0, 0, 12, 12}},
	{'/', {1, 2, 4, 8, 16, 0, 0}},
	{':', {0, 12, 12, 0, 12, 12, 0}},
	{'0', {14, 17, 19, 21, 25, 17, 14}},
	{'1', {4, 12, 4, 4, 4, 4, 14}},
	{'2', {14, 17, 1, 2, 4, 8, 31}},
	{'3', {30, 1, 1, 14, 1, 1, 30}},
	{'4', {2, 6, 10, 18, 31, 2, 2}},
	{'5', {31, 16, 16, 30, 1, 1, 30}},
	{'6', {14, 16, 16, 30, 17, 17, 14}},
	{'7', {31, 1, 2, 4, 8, 8, 8}},
	{'8', {14, 17, 17, 14, 17, 17, 14}},
	{'9', {14, 17, 17, 15, 1, 1, 14}},
	{'A', {14, 17, 17, 31, 17, 17, 17}},
	{'B', {30, 17, 17, 30, 17, 17, 30}},
	{'C', {14, 17, 16, 16, 16, 17, 14}},
	{'D', {30, 17, 17, 17, 17, 17, 30}},
	{'E', {31, 16, 16, 30, 16, 16, 31}},
	{'F', {31, 16, 16, 30, 16, 16, 16}},
	{'G', {14, 17, 16, 23, 17, 17, 15}},
	{'H', {17, 17, 17, 31, 17, 17, 17}},
	{'I', {14, 4, 4, 4, 4, 4, 14}},
	{'J', {7, 2, 2, 2, 18, 18, 12}},
	{'K', {17, 18, 20, 24, 20, 18, 17}},
	{'L', {16, 16, 16, 16, 16, 16, 31}},
	{'M', {17, 27, 21, 21, 17, 17, 17}},
	{'N', {17, 25, 21, 19, 17, 17, 17}},
	{'O', {14, 17, 17, 17, 17, 17, 14}},
	{'P', {30, 17, 17, 30, 16, 16, 16}},
	{'Q', {14, 17, 17, 17, 21, 18, 13}},
	{'R', {30, 17, 17, 30, 20, 18, 17}},
	{'S', {15, 16, 16, 14, 1, 1, 30}},
	{'T', {31, 4, 4, 4, 4, 4, 4}},
	{'U', {17, 17, 17, 17, 17, 17, 14}},
	{'V', {17, 17, 17, 17, 17, 10, 4}},
	{'W', {17, 17, 17, 21, 21, 21, 10}},
	{'X', {17, 17, 10, 4, 10, 17, 17}},
	{'Y', {17, 17, 10, 4, 4, 4, 4}},
	{'Z', {31, 1, 2, 4, 8, 16, 31}},
};

static uint32_t splash_scale_x(const struct splash_surface *surface,
			       unsigned int value)
{
	uint32_t result;

	result = (uint32_t)(((uint64_t)value * surface->width) /
			    SPLASH_LOGICAL_WIDTH);

	return result;
}

static uint32_t splash_scale_y(const struct splash_surface *surface,
			       unsigned int value)
{
	uint32_t result;

	result = (uint32_t)(((uint64_t)value * surface->height) /
			    SPLASH_LOGICAL_HEIGHT);

	return result;
}

static void splash_fill(struct splash_surface *surface, uint32_t color)
{
	uint32_t x;
	uint32_t y;

	for (y = 0; y < surface->height; ++y) {
		for (x = 0; x < surface->width; ++x) {
			surface->pixels[(size_t)y * surface->stride + x] = color;
		}
	}
}

static void splash_fill_rect(struct splash_surface *surface, unsigned int x,
			     unsigned int y, unsigned int width,
			     unsigned int height, uint32_t color)
{
	uint32_t left = splash_scale_x(surface, x);
	uint32_t top = splash_scale_y(surface, y);
	uint32_t right = splash_scale_x(surface, x + width);
	uint32_t bottom = splash_scale_y(surface, y + height);
	uint32_t column;
	uint32_t row;

	if (width > 0 && right <= left && left < surface->width) {
		right = left + 1U;
	}
	if (height > 0 && bottom <= top && top < surface->height) {
		bottom = top + 1U;
	}
	if (right > surface->width) {
		right = surface->width;
	}
	if (bottom > surface->height) {
		bottom = surface->height;
	}
	for (row = top; row < bottom; ++row) {
		for (column = left; column < right; ++column) {
			surface->pixels[(size_t)row * surface->stride + column] = color;
		}
	}
}

static void splash_outline_rect(struct splash_surface *surface, unsigned int x,
				unsigned int y, unsigned int width,
				unsigned int height, unsigned int thickness,
				uint32_t color)
{
	splash_fill_rect(surface, x, y, width, thickness, color);
	splash_fill_rect(surface, x, y + height - thickness, width, thickness,
			 color);
	splash_fill_rect(surface, x, y, thickness, height, color);
	splash_fill_rect(surface, x + width - thickness, y, thickness, height,
			 color);
}

static void splash_line(struct splash_surface *surface, int x0, int y0, int x1,
			int y1, unsigned int thickness, uint32_t color)
{
	int delta_x = x1 > x0 ? x1 - x0 : x0 - x1;
	int step_x = x0 < x1 ? 1 : -1;
	int delta_y = y1 > y0 ? y0 - y1 : y1 - y0;
	int step_y = y0 < y1 ? 1 : -1;
	int error = delta_x + delta_y;
	int doubled;
	bool complete = false;

	while (!complete) {
		splash_fill_rect(surface,
				 (unsigned int)(x0 - (int)thickness / 2),
				 (unsigned int)(y0 - (int)thickness / 2),
				 thickness, thickness, color);
		if (x0 == x1 && y0 == y1) {
			complete = true;
		} else {
			doubled = 2 * error;
			if (doubled >= delta_y) {
				error += delta_y;
				x0 += step_x;
			}
			if (doubled <= delta_x) {
				error += delta_x;
				y0 += step_y;
			}
		}
	}
}

static void splash_circle(struct splash_surface *surface, int center_x,
			  int center_y, int radius, int thickness,
			  uint32_t color)
{
	int x;
	int y;
	int delta_x;
	int delta_y;
	int distance;
	int outer = radius * radius;
	int inner = (radius - thickness) * (radius - thickness);

	for (y = center_y - radius; y <= center_y + radius; ++y) {
		for (x = center_x - radius; x <= center_x + radius; ++x) {
			delta_x = x - center_x;
			delta_y = y - center_y;
			distance = delta_x * delta_x + delta_y * delta_y;
			if (distance <= outer && distance >= inner) {
				splash_fill_rect(surface, (unsigned int)x,
						 (unsigned int)y, 1, 1, color);
			}
		}
	}
}

static const struct splash_glyph *splash_find_glyph(char character)
{
	const struct splash_glyph *result = &splash_glyphs[0];
	size_t index;

	if (character >= 'a' && character <= 'z') {
		character = (char)(character - 'a' + 'A');
	}
	for (index = 0; index < sizeof(splash_glyphs) / sizeof(splash_glyphs[0]);
	     ++index) {
		if (splash_glyphs[index].character == character) {
			result = &splash_glyphs[index];
		}
	}

	return result;
}

static unsigned int splash_text_width(const char *text, unsigned int scale)
{
	size_t length = strlen(text);
	unsigned int result = 0;

	if (length > 0) {
		result = (unsigned int)((length * 6U - 1U) * scale);
	}

	return result;
}

static void splash_text(struct splash_surface *surface, const char *text,
			unsigned int x, unsigned int y, unsigned int scale,
			uint32_t color)
{
	const struct splash_glyph *glyph;
	unsigned int column;
	unsigned int row;
	unsigned int cursor = x;

	while (*text != '\0') {
		glyph = splash_find_glyph(*text);
		for (row = 0; row < 7U; ++row) {
			for (column = 0; column < 5U; ++column) {
				if ((glyph->rows[row] & (1U << (4U - column))) != 0) {
					splash_fill_rect(surface, cursor + column * scale,
							 y + row * scale, scale, scale,
							 color);
				}
			}
		}
		cursor += 6U * scale;
		++text;
	}
}

static void splash_centered_text(struct splash_surface *surface,
				 const char *text, unsigned int y,
				 unsigned int scale, uint32_t color)
{
	unsigned int width = splash_text_width(text, scale);
	unsigned int x = width < SPLASH_LOGICAL_WIDTH ?
			 (SPLASH_LOGICAL_WIDTH - width) / 2U : 0U;

	splash_text(surface, text, x, y, scale, color);
}

/* 版本字符串由打包参数注入，过长时缩小字号，避免现场自定义版本越过徽标边界。 */
static unsigned int splash_text_fit_scale(const char *text,
					  unsigned int maximum_width,
					  unsigned int preferred_scale)
{
	unsigned int result = preferred_scale;

	while (result > 1U && splash_text_width(text, result) > maximum_width) {
		--result;
	}

	return result;
}

static void splash_check(struct splash_surface *surface, int center_x,
			 int center_y, int radius)
{
	splash_circle(surface, center_x, center_y, radius, 3, COLOR_GREEN);
	splash_line(surface, center_x - radius / 2, center_y,
		    center_x - radius / 8, center_y + radius / 3, 4, COLOR_GREEN);
	splash_line(surface, center_x - radius / 8, center_y + radius / 3,
		    center_x + radius / 2, center_y - radius / 3, 4, COLOR_GREEN);
}

static void splash_terminal_icon(struct splash_surface *surface, int center_x,
				 int center_y)
{
	splash_outline_rect(surface, (unsigned int)(center_x - 34),
			    (unsigned int)(center_y - 24), 68, 48, 3,
			    COLOR_CYAN);
	splash_line(surface, center_x - 22, center_y - 4, center_x - 10,
		    center_y + 6, 3, COLOR_CYAN);
	splash_line(surface, center_x - 10, center_y + 6, center_x - 22,
		    center_y + 16, 3, COLOR_CYAN);
	splash_line(surface, center_x - 4, center_y + 16, center_x + 18,
		    center_y + 16, 3, COLOR_CYAN);
}

static void splash_driver_icon(struct splash_surface *surface, int center_x,
			       int center_y)
{
	splash_circle(surface, center_x, center_y, 24, 4, COLOR_CYAN);
	splash_circle(surface, center_x, center_y, 8, 3, COLOR_CYAN);
	splash_line(surface, center_x, center_y - 35, center_x, center_y - 24,
		    4, COLOR_CYAN);
	splash_line(surface, center_x, center_y + 24, center_x, center_y + 35,
		    4, COLOR_CYAN);
	splash_line(surface, center_x - 35, center_y, center_x - 24, center_y,
		    4, COLOR_CYAN);
	splash_line(surface, center_x + 24, center_y, center_x + 35, center_y,
		    4, COLOR_CYAN);
}

static void splash_display_icon(struct splash_surface *surface, int center_x,
				int center_y)
{
	splash_outline_rect(surface, (unsigned int)(center_x - 36),
			    (unsigned int)(center_y - 25), 72, 45, 3,
			    COLOR_CYAN);
	splash_line(surface, center_x, center_y + 20, center_x, center_y + 31,
		    3, COLOR_CYAN);
	splash_line(surface, center_x - 16, center_y + 31, center_x + 16,
		    center_y + 31, 3, COLOR_CYAN);
	splash_text(surface, "USB", (unsigned int)(center_x - 18),
		    (unsigned int)(center_y - 8), 2, COLOR_CYAN);
}

size_t usbdisplay_splash_bytes(uint32_t width, uint32_t height)
{
	size_t result = 0;

	if (width > 0 && height > 0 && width <= SIZE_MAX / 4U &&
	    (size_t)width * 4U <= SIZE_MAX / height) {
		result = (size_t)width * height * 4U;
	}

	return result;
}

/* 状态页只陈述可证明的驱动与 USB 传输就绪，不显示虚构的网络或温度信息。 */
int usbdisplay_splash_render(uint32_t *pixels, uint32_t width,
			     uint32_t height, uint32_t stride_bytes)
{
	struct splash_surface surface;
	const char *version_text = "VERSION " USBDISPLAY_VERSION;
	unsigned int version_scale;
	int result = 0;

	if (pixels == NULL || width == 0 || height == 0 ||
	    stride_bytes < width * 4U || stride_bytes % 4U != 0) {
		result = -EINVAL;
	} else {
		surface.pixels = pixels;
		surface.width = width;
		surface.height = height;
		surface.stride = stride_bytes / 4U;
		splash_fill(&surface, COLOR_BACKGROUND);
		splash_fill_rect(&surface, 0, 0, 960, 5, COLOR_CYAN);
		splash_text(&surface, "USB DISPLAY SERVICE", 45, 24, 2,
			    COLOR_MUTED);
		splash_check(&surface, 754, 34, 18);
		splash_text(&surface, "CONNECTED", 785, 24, 3, COLOR_GREEN);

		splash_centered_text(&surface, "USBDISPLAYSTACK", 67, 6,
				     COLOR_WHITE);
		splash_centered_text(&surface, "USB DISPLAY TRANSPORT", 118, 2,
				     COLOR_MUTED);

		splash_text(&surface, "SOFTWARE", 123, 157, 2, COLOR_CYAN);
		splash_text(&surface, "DRIVER", 445, 157, 2, COLOR_CYAN);
		splash_text(&surface, "USB DISPLAY", 725, 157, 2, COLOR_CYAN);
		splash_line(&surface, 222, 226, 428, 226, 3, COLOR_CYAN);
		splash_line(&surface, 532, 226, 738, 226, 3, COLOR_CYAN);
		splash_circle(&surface, 170, 226, 52, 3, COLOR_CYAN);
		splash_circle(&surface, 480, 226, 52, 3, COLOR_CYAN);
		splash_circle(&surface, 790, 226, 52, 3, COLOR_CYAN);
		splash_terminal_icon(&surface, 170, 226);
		splash_driver_icon(&surface, 480, 226);
		splash_display_icon(&surface, 790, 226);

		/* 主状态面板将结论和运行语义分栏，避免多行居中信息互相争夺视觉焦点。 */
		splash_fill_rect(&surface, 45, 312, 870, 126, COLOR_PANEL);
		splash_fill_rect(&surface, 45, 312, 6, 126, COLOR_GREEN);
		splash_check(&surface, 111, 375, 30);
		splash_text(&surface, "TRANSPORT STATUS", 164, 337, 2,
			    COLOR_MUTED);
		splash_text(&surface, "CONNECTED", 164, 366, 5, COLOR_GREEN);
		splash_fill_rect(&surface, 520, 334, 2, 82, COLOR_LINE);
		splash_text(&surface, "DRIVER AND USB TRANSPORT READY", 551, 350,
			    2, COLOR_WHITE);
		splash_text(&surface, "WAITING FOR APPLICATION", 593, 391, 2,
			    COLOR_MUTED);

		/* 页脚保持单行扫描顺序：项目地址、QQ、由 PCCT 注入的构建版本。 */
		splash_fill_rect(&surface, 45, 463, 870, 2, COLOR_LINE);
		splash_text(&surface, USBDISPLAY_SPLASH_PROJECT_URL, 45, 489, 2,
			    COLOR_WHITE);
		splash_text(&surface, "QQ:100860505", 575, 489, 2, COLOR_CYAN);
		splash_outline_rect(&surface, 741, 478, 174, 36, 2, COLOR_LINE);
		version_scale = splash_text_fit_scale(version_text, 154U, 2U);
		splash_text(&surface, version_text,
			    741U + (174U - splash_text_width(version_text,
							 version_scale)) / 2U,
			    489, version_scale, COLOR_MUTED);
	}

	return result;
}
