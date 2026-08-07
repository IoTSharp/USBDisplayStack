/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USBDISPLAY_BACKEND_H
#define USBDISPLAY_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include <usbdisplay/uapi.h>

#define USBDISPLAY_BACKEND_ABI_VERSION 1U
#define USBDISPLAY_BACKEND_ENTRY "usbdisplay_backend_v1"
#define USBDISPLAY_BACKEND_CAP_NONE 0U

struct usbdisplay_backend_config {
	uint32_t struct_size;
	const char *option;
	uint32_t device_width;
	uint32_t device_height;
};

struct usbdisplay_frame {
	uint32_t struct_size;
	const void *pixels;
	size_t bytes;
	uint64_t sequence;
	uint64_t timestamp_ns;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint32_t source;
	uint32_t damage_x;
	uint32_t damage_y;
	uint32_t damage_width;
	uint32_t damage_height;
};

struct usbdisplay_backend_v1 {
	uint32_t abi_version;
	uint32_t struct_size;
	uint64_t capabilities;
	const char *name;
	int (*open)(const struct usbdisplay_backend_config *config, void **context);
	int (*submit)(void *context, const struct usbdisplay_frame *frame);
	void (*close)(void *context);
};

#define USBDISPLAY_BACKEND_V1_REQUIRED_SIZE \
	(offsetof(struct usbdisplay_backend_v1, close) + \
	 sizeof(((struct usbdisplay_backend_v1 *)0)->close))

typedef const struct usbdisplay_backend_v1 *(*usbdisplay_backend_entry_fn)(void);

#endif
