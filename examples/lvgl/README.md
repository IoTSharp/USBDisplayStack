# LVGL 9 examples

These examples render the same LVGL dashboard through either the independent
USBDisplayStack framebuffer or its DRM/KMS connector. They require LVGL 9
with `LV_COLOR_DEPTH=32`; the DRM example also requires libdrm development
headers.

```bash
make -C examples/lvgl
sudo build/examples/lvgl-fbdev-example /dev/fb1
sudo build/examples/lvgl-drm-example /dev/dri/card1
```

When LVGL has not been installed system-wide, build directly from an LVGL 9
source tree:

```bash
make -C examples/lvgl LVGL_PATH=/path/to/lvgl
```

Device numbers are examples. Discover the framebuffer whose sysfs `name` is
`usbdisplay` or the DRM card whose driver name is `usbdisplay` before launch.
Both examples verify that identity before rendering and refuse other devices.
Run one producer at a time.
