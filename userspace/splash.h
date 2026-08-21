/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USBDISPLAY_SPLASH_H
#define USBDISPLAY_SPLASH_H

#include <stddef.h>
#include <stdint.h>

/* PCCT 打包时覆盖该宏，使状态页展示与 DEB 元数据一致的版本号。 */
#ifndef USBDISPLAY_VERSION
#define USBDISPLAY_VERSION "development"
#endif

#define USBDISPLAY_SPLASH_PROJECT_URL \
	"https://gitee.com/IoTSharp/USBDisplayStack"

/* Splash 使用纯 XRGB8888 绘制，避免为状态页引入额外图形动态库。 */
size_t usbdisplay_splash_bytes(uint32_t width, uint32_t height);
int usbdisplay_splash_render(uint32_t *pixels, uint32_t width,
			     uint32_t height, uint32_t stride_bytes);

#endif
