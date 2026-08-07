/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USBDISPLAY_UAPI_H
#define USBDISPLAY_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define USBDISPLAY_ABI_VERSION 1U
#define USBDISPLAY_SLOT_COUNT 3U

enum usbdisplay_pixel_format {
	USBDISPLAY_FORMAT_INVALID = 0,
	USBDISPLAY_FORMAT_RGB565 = 1,
	USBDISPLAY_FORMAT_XRGB8888 = 2
};

enum usbdisplay_frame_source {
	USBDISPLAY_SOURCE_INITIAL = 0,
	USBDISPLAY_SOURCE_DRM = 1,
	USBDISPLAY_SOURCE_FBDEV = 2
};

struct usbdisplay_device_info {
	__u32 abi_version;
	__u32 width;
	__u32 height;
	__u32 slot_count;
	__u32 slot_bytes;
	__u32 map_bytes;
	__u32 reserved0;
	__u32 reserved1;
	__u64 sequence;
};

struct usbdisplay_update {
	__u64 sequence;
	__u64 timestamp_ns;
	__u32 slot;
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 format;
	__u32 source;
	__u32 damage_x;
	__u32 damage_y;
	__u32 damage_width;
	__u32 damage_height;
	__u32 reserved[4];
};

#define USBDISPLAY_IOCTL_MAGIC 'U'
#define USBDISPLAY_IOCTL_GET_INFO \
	_IOR(USBDISPLAY_IOCTL_MAGIC, 0x00, struct usbdisplay_device_info)

#endif
