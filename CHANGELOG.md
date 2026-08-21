# Changelog

## Unreleased

- Recover a failed persistent FFmpeg process inside the Actions Micro backend
  with one bounded same-frame retry, while preserving the live HID transport
  session and its command/video sequence numbers.
- Verify that each encoder handoff contains SPS, PPS, and IDR NAL units; report
  the FFmpeg exit status, bounded HID input samples, and encoder generations.
- Add dual-hidraw input polling, a missed-heartbeat session watchdog, and a
  VID/PID-scoped `USBDEVFS_RESET` before Actions Micro transport reopen.
- Reopen the kernel frame stream after transport recovery, resubmit the newest
  frame every two idle seconds, and publish a monotonic ready-file generation.
- Add a kernel-version-bound offline Debian package, SHA-256 sidecar, and an
  explicit installer that never falls back to a network package source.
- Add `usbdisplay-check` with human-readable and JSON physical-readiness
  reports for installation checks and second-screen supervision.
- Link the daemon with `librt` for older glibc toolchains and reject packaging
  when the kernel module vermagic differs from the declared target kernel.
- Migrate an unowned legacy `/usr/lib` systemd unit during offline activation
  so it cannot shadow the Debian package unit under `/lib`.
- Keep the transport daemon alive while the selected USB backend is absent or
  disconnected; publish a PID-checked readiness marker for framebuffer clients.
- Mark readiness with the backend's physical-transport capability so diagnostic
  null/PPM backends cannot activate the real second-screen consumer.
- Rate-limit missing-adapter diagnostics through the daemon's retry reporting
  instead of emitting backend discovery and empty-close messages every retry.
- Add explicit reconnect and virtual-device path handling to the systemd service
  so a missing UGREEN adapter cannot be mistaken for a ready display.
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
