# Test Record

## 2026-08-07 reference test

Environment:

- Ubuntu 16.04 userspace
- Linux `4.15.0-60-generic` x86_64
- GCC 5.4
- existing Intel DRM card and framebuffer left active

Verified:

- userspace builds with `-Wall -Wextra -Werror`;
- `usbdisplay.ko` builds against the running kernel headers;
- loading `width=640 height=360` creates a second DRM card, an fbdev named
  `usbdisplay`, and `/dev/usbdisplay0`;
- fbdev reports exactly `640x360`, 32 bpp, and a 2560-byte stride;
- DRM reports one connected virtual connector with one preferred 640x360 mode;
- `fb-test-pattern -> /dev/fb1 -> usb-displayd -> PPM` produces a 691,215-byte
  P6 image with the expected non-black pixels;
- repeated load/unload leaves the original DRM card, framebuffer, and existing
  application process unchanged;
- no new `BUG`, `Bad page state`, `Oops`, or protection fault is logged.

Hardware output through Actions Micro `185b:2d1d` is outside this successful
framebuffer test result and remains experimental. Separately, a five-second
captured replay containing 17 command reports and 1,234 video reports woke the
HDMI output without a physical button press and displayed the captured Windows
desktop. Live frames encoded from the new framebuffer are not yet confirmed.
