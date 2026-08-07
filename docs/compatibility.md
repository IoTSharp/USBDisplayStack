# Compatibility

## Linux kernel

The first alpha is developed and tested against Ubuntu's
`4.15.0-60-generic` x86 kernel. The current module uses the Linux 4.15
TinyDRM API. Newer kernels moved simple display drivers to other DRM helpers,
so forward-port compatibility is planned but not claimed yet.

The userspace daemon and diagnostic backends require only a C11 compiler and
`libdl`. The Actions Micro backend additionally invokes `ffmpeg` with the
`libx264` encoder. The LVGL examples target LVGL 9; the DRM example uses
libdrm. The C# examples target .NET 8 and the DRM example loads
`libdrm.so.2`.

## Application interfaces

Framebuffer applications, including LVGL's Linux fbdev display driver, can
open the framebuffer whose sysfs name is `usbdisplay`. The current fbdev mode
is fixed XRGB8888; mode-setting requests for another size or depth are
rejected.

DRM applications see one connected virtual connector with the module's fixed
mode. DRM and fbdev are independent producers feeding the same backend stream;
they should not be used to render different content at the same time.

## USB display adapters

"USB to HDMI" is a product category, not one protocol. DisplayLink, Trigger,
MacroSilicon, Fresco Logic, and Actions Micro devices are not interchangeable.
A backend must match the adapter's VID/PID, interfaces, endpoints, framing,
codec, and initialization sequence.

Known project status:

| Adapter/protocol | Status |
| --- | --- |
| Null backend | Working; diagnostics only |
| PPM backend | Working; end-to-end frame verification |
| Actions Micro `185b:2d1d` | Experimental live backend; full-bootstrap 1920x1080 LVGL output visually verified on the reference unit; repeated hotplug and long-running tests remain |
| DisplayLink/EVDI | Not used by this project |
| MacroSilicon MS912x/MS9132 | Different protocol; not supported |

The Actions Micro adapter must not be advertised as generally supported until
the visually verified live path has also survived repeated hotplug and
long-running tests.
