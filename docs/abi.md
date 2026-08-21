# ABI Reference

## Kernel UAPI

The public header is `include/usbdisplay/uapi.h`. ABI version 1 exposes:

- `USBDISPLAY_IOCTL_GET_INFO`: dimensions, slot geometry, and latest sequence.
- `mmap`: one read-only mapping whose length equals `map_bytes`.
- `poll`: readable when a sequence newer than the consumer's last read exists.
- `read`: returns one complete `struct usbdisplay_update`.

The consumer calculates a frame address as:

```c
frame = mapping + update.slot * info.slot_bytes;
```

The valid byte count is `update.stride * update.height`. Pixel formats are
`USBDISPLAY_FORMAT_RGB565` and `USBDISPLAY_FORMAT_XRGB8888`. Frame sources are
initial state, DRM, and fbdev.

There is one consumer. A second `open(2)` returns `EBUSY`. The mapping rejects
write permission, non-zero offsets, and lengths other than `map_bytes`.

Reserved fields must be zero when produced and ignored when consumed. Any
incompatible layout change requires a new `USBDISPLAY_ABI_VERSION` and ioctl.

## Backend ABI

The public header is `include/usbdisplay/backend.h`. A shared object exports:

```c
const struct usbdisplay_backend_v1 *usbdisplay_backend_v1(void);
```

The returned table must provide matching `abi_version`, a compatible
`struct_size`, a name, and `open`, `submit`, and `close` callbacks. New fields
may only be appended. A semantic or layout break requires a new entry point
and ABI version.

Version 1 optionally exposes `tick(context, monotonic_now_ns)` at the end of
the table. A backend that sets `USBDISPLAY_BACKEND_CAP_TICK` must include the
field in `struct_size` and provide the callback. The daemon calls it at least
every 100 ms while idle and after frame processing, allowing keepalives and
transport health checks to continue when the displayed image is static. Old
backends whose table ends at `close` remain compatible.

`USBDISPLAY_BACKEND_CAP_PHYSICAL` declares that a backend owns a real display
transport rather than a diagnostic sink. The daemon records this capability as
`physical=1` in its readiness file after `open` succeeds. The same marker
contains a positive `generation` that increments after each recovered backend
open. Consumers that drive real displays must require both values; the null and
PPM backends intentionally leave the capability clear and publish `physical=0`.

`submit` runs synchronously. A backend should either complete promptly or
implement its own bounded queue. Holding the call indefinitely prevents the
daemon from advancing to newer snapshots.

The `pixels` pointer remains valid only for the duration of `submit`. Backends
must copy data they retain after the callback returns.
