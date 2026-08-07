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
desktop. A later full-bootstrap test visually confirmed live LVGL output as
described below.

## 2026-08-07 experimental live-backend test

The live backend was built with GCC 5.4 and `-Werror` on the same reference
host. The adapter was discovered as `/dev/hidraw2` (`input0`) and
`/dev/hidraw3` (`input1`). An authorized five-second replay template produced
14 initialization commands and a three-message `_PPA` heartbeat cycle.

Verified over temporary 640x360 module loads:

- backend initialization, automatic hidraw discovery, idle heartbeat, FFmpeg
  2.8/libx264 encoding, and HID delivery remained active without daemon exits;
- an initial frame followed by `fb-test-pattern` completed with 2 encoded
  frames and 24 HID reports;
- the LVGL 9 fbdev and DRM examples ran sequentially for three seconds each;
  the same backend session encoded 142 frames and sent 184 HID reports;
- self-contained .NET 8 fbdev and DRM examples each rendered 50 frames; the
  backend session encoded 121 frames and sent 154 HID reports;
- a replay with an invalid record length was rejected with zero HID reports
  before device initialization;
- DRM still reported one connected preferred 640x360 mode, and no new kernel
  Oops, protection fault, or USBDisplayStack error appeared.

Linux 4.15 logged an x86 PAT cache-policy notice whenever either DRM example
mapped its dumb buffer. The notices did not include an Oops or terminate the
examples.

These results verify software flow through both application interfaces and the
live HID sender.

The reference adapter was then physically unplugged and reconnected before a
1920x1080 `bootstrap=full` run. USB capture verified that all 154 authorized
bootstrap reports matched the replay byte-for-byte. The last replay P frame was
video sequence 92 on endpoint `0x04`; 105 ms later, live sequence 93 sent the
captured-matching SPS/PPS/SEI configuration on `0x03`, followed by a four-slice
IDR at sequence 94. Continued `_PPA` commands used sequences 17 through 23 on
alternating endpoints with matching report IDs, and the capture had zero packet
loss. The user visually confirmed new LVGL widgets and a scroll bar on the HDMI
display while both the producer and daemon remained active.

This closes the live-pixel visual acceptance gap for the reference unit. The
backend remains experimental pending repeated hotplug and long-running tests.
