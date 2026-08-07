# Changelog

## v0.1.0-alpha.1 - 2026-08-07

- Add a Linux 4.15 virtual DRM/KMS card with one fixed virtual connector.
- Add an independent fixed-mode XRGB8888 fbdev for LVGL and similar clients.
- Add a read-only triple-buffer kernel-to-userspace ABI.
- Add the backend host daemon and versioned, extensible plugin ABI.
- Add null and PPM diagnostic backends.
- Add fbdev test-pattern, DRM probe, and experimental Actions Micro replay
  diagnostics.
- Add source and prebuilt installation support, udev/systemd packaging, and
  CI definitions.

The Actions Micro `185b:2d1d` live hardware path remains experimental and is
not included in the successful framebuffer/DRM support claim.
