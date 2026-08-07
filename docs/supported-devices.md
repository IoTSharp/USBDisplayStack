# Supported Device List

This list records physical USB display adapters that have a matching
USBDisplayStack backend and a hardware validation result. A retail product name
alone is not sufficient for protocol support; the USB identity and interface
layout must match.

## Verified Hardware

| Device | USB identity | Status | Backend |
| --- | --- | --- | --- |
| UGREEN USB-to-HDMI adapter (black USB-A model) | VID `185b`, PID `2d1d`; descriptors report `Compro` / `Mirroring Suit` | Experimental; live 1920x1080 LVGL output visually verified on the reference unit | Actions Micro HID H.264 |

![UGREEN USB-to-HDMI adapter cutout](assets/devices/ugreen-usb-hdmi-185b-2d1d.png)

The product image is a clean cutout derived from the user-provided retail
listing image. It identifies the physical enclosure only; it is not evidence
that every visually similar UGREEN revision uses the same chipset.

### UGREEN USB-to-HDMI adapter

- Matching USB ID: `185b:2d1d`.
- Observed HID interfaces: physical `input0` and `input1`, normally exposed as
  two `/dev/hidraw*` nodes.
- Transport: vendor-specific 4096-byte HID reports carrying `RRIM/TADV`
  Annex-B H.264 and `_PPA` keepalives.
- Activation: an authorized `DPRPL001` template is required. The reference
  firmware uses `bootstrap=full` so the captured stream can hand off to live
  H.264 in the same transport session.
- Validation: after a physical USB power cycle, the reference unit displayed
  new LVGL widgets and a scroll bar while the live backend and producer stayed
  active. Repeated hotplug and long-running validation remain outstanding.

To check a connected unit before selecting this backend:

```bash
lsusb -d 185b:2d1d
```

Other USB-to-HDMI products, including DisplayLink, Trigger, MacroSilicon, and
Fresco Logic devices, are different protocols and are not covered by this
entry.
