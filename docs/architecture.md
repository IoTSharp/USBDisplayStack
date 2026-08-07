# Architecture

## Design goals

USBDisplayStack separates the operating-system display interface from each
adapter's transport protocol. An application such as LVGL uses only fbdev or
DRM/KMS and does not link to x264, libusb, hidraw, or a vendor SDK.

The primary display is outside the stack's ownership. The driver creates new
nodes and never redirects output to `/dev/fb0` or the existing DRM card.

## Kernel frontend

The module exposes three related interfaces:

1. A DRM/KMS card with one connected virtual connector and one fixed mode.
2. An independently managed, fixed-mode XRGB8888 framebuffer.
3. `/dev/usbdisplay0`, a read-only frame stream for one userspace consumer.

The Linux 4.15 TinyDRM helpers are used for GEM and the simple DRM display
pipe. USBDisplayStack calls `drm_dev_register()` directly instead of
`devm_tinydrm_register()`, because the latter always creates its own fbdev
emulation on this kernel. The project registers fbdev itself so its memory,
mode, and update notification behavior are deterministic.

Writes through `write(2)`, fbdev software drawing operations, and deferred-I/O
mmap dirties all publish through the same snapshot function. DRM framebuffer
dirty callbacks use that function as well.

## Snapshot stream

The producer owns three page-aligned slots. A new update is copied into a slot
that is not held by the consumer, assigned a monotonically increasing
sequence, and announced through `poll(2)` and `read(2)`.

The daemon maps all slots read-only. A successful metadata read transfers the
hold to the announced slot. Intermediate updates may be replaced when a slow
backend falls behind; this is intentional for a display stream. Frames are
coherent snapshots, not a lossless event log.

The ring is allocated with `vmalloc_user()` so Linux 4.15 accepts the standard
`remap_vmalloc_range()` mapping. The mapping is never writable by userspace.

## Userspace backends

`usb-displayd` loads one shared object with `dlopen(3)`. The backend ABI carries
structure sizes and an ABI version so fields can be appended without forcing
unrelated backends to change.

A backend owns all protocol-specific work:

- pixel conversion and compression;
- USB, HID, or network discovery and transport;
- initialization messages and keepalives;
- hotplug and reconnect policy;
- rate control and frame dropping.

No vendor protocol is added to the kernel module. Adding another adapter
normally requires a new directory under `backends/`, not a new framebuffer
driver.

## Failure boundaries

- Only one daemon may open `/dev/usbdisplay0` at a time.
- A missing or failed backend terminates the daemon; it does not affect fb0.
- A disconnected USB adapter is a backend error; there is no primary-display
  fallback.
- Module unload unregisters fbdev, the misc device, and DRM before releasing
  their memory.
