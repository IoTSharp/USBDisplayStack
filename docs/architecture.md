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

Backends may request a periodic `tick` callback. This keeps control traffic
alive even when fbdev or DRM has not published a new frame. Physical backends
never replace the current framebuffer image with a daemon splash during
reconnect; the latest fbdev snapshot is replayed when the stream is reopened.

Before the first application frame, an `INITIAL` update displays the existing
USBDisplayStack `CONNECTED / WAITING FOR APPLICATION` startup splash, including
on physical backends. The next fbdev or DRM update takes over immediately.
Closing an application retains its last frame; restarting the transport does
not replace that retained frame with the startup splash. This splash is drawn
by the daemon's pixel renderer, independently of LVGL. Sending it still requires
a working physical video session; it cannot replace the adapter's firmware page
when the firmware has not accepted video.

The Actions Micro backend negotiates SGUP v3 local and remote session IDs
before initialization. Those IDs are included in every command, heartbeat,
and first video fragment; a captured session ID is never treated as fixed
protocol magic. Matching device replies confirm synchronization, and a changed
device session causes a backend reopen and latest-frame delivery. The default
command-only bootstrap avoids displaying video captured in the replay template.

The daemon publishes a readiness file after the backend opens. The file carries
the live daemon PID, a positive transport `generation`, and `physical=1` only
for a backend that owns a physical display transport; diagnostic backends
publish `physical=0`. On a backend open or submit failure it removes the file,
closes the backend, and retries in the same process. A completed reopen
increments the generation. Real display consumers must require a live daemon
PID, a positive generation, and `physical=1`; the virtual framebuffer can exist
while no USB adapter is present.

A backend owns all protocol-specific work:

- pixel conversion and compression;
- USB, HID, or network discovery and transport;
- initialization messages and keepalives;
- hotplug and reconnect policy;
- rate control and frame dropping.

No vendor protocol is added to the kernel module. Adding another adapter
normally requires a new directory under `backends/`, not a new framebuffer
driver.

The Actions Micro backend follows this boundary: it extracts only command
reports from an authorized replay template, verifies both HID interfaces,
sends initialization, schedules `_PPA` keepalives, runs a persistent FFmpeg
H.264 encoder, and fragments `RRIM/TADV` video messages. None of those details
are present in the kernel module or application examples.

## Failure boundaries

- Only one daemon may open `/dev/usbdisplay0` at a time.
- A missing backend library is fatal, but a physical backend open or submit
  failure clears readiness and enters the bounded reconnect wait.
- A disconnected USB adapter never falls back to the primary display.
- Module unload unregisters fbdev, the misc device, and DRM before releasing
  their memory.
