# Changelog

## Unreleased

- Add the experimental Actions Micro `185b:2d1d` live backend with replay-
  derived initialization and heartbeat templates, FFmpeg H.264 encoding, and
  dual-hidraw video delivery.
- Extend backend ABI v1 with an optional idle tick callback.
- Add LVGL 9 and C# examples for both fbdev and DRM/KMS.
- Add the Simplified Chinese README.
- Normalize live H.264 NAL units to the adapter's four-byte Annex-B framing.
- Mark live H.264 as square-pixel video so its SPS matches the stream accepted
  by the Actions Micro firmware.
- Add an opt-in Actions Micro full-replay bootstrap that preserves transport
  sequence and HID alternation when handing off to live H.264 video.
- Continue post-template Actions Micro keepalives on alternating command HID
  endpoints with matching report IDs.
- Visually verify the Actions Micro full-bootstrap handoff with live 1920x1080
  LVGL output on the reference adapter after a physical USB power cycle.

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
