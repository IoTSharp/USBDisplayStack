# Actions Micro 185b:2d1d

## Status

The adapter exposes two HID output interfaces and carries H.264 in a
vendor-specific message stream. It is not DisplayLink, Trigger 5, MS912x, or
MS9132. No matching open-source Linux host driver was found as of 2026-08-07.

Replaying the first five seconds of an original Windows capture on the
reference host displayed the captured Windows desktop. On 2026-08-07 this
software replay woke the HDMI output without pressing the adapter's physical
button. The replay contained 17 command reports and 1,234 video reports over
4.968 seconds. This verifies the HID paths and captured initialization
sequence. Newly encoded H.264 has not yet been visually accepted by the
adapter, so USBDisplayStack does not claim live hardware support and does not
install a default backend for this device.

## Observed framing

Each HID output report is 4096 bytes. The 14-byte little-endian outer header
contains:

```text
report_id:u8
message_type:u8
flags:u16
sequence:u32
fragment_info:u32
payload_length:u16
```

The low 16 bits of `fragment_info` are the fragment count and the high 16 bits
are the fragment index. Reassembled video messages contain a 32-byte
`RRIM/TADV` header followed by Annex-B H.264. Observed data flags are:

| Flag | Meaning |
| --- | --- |
| `0x800` | SPS/PPS/SEI configuration |
| `0x200` | IDR frame |
| `0x400` | P frame |

These values describe one observed firmware and are not a public vendor
specification.

## Replay diagnostic

`actions-micro-replay` consumes the `DPRPL001` capture-derived replay format.
Dry-run mode performs full container, endpoint, report ID, message type, and
length validation without opening a USB device:

```bash
build/actions-micro-replay --dry-run capture.replay
```

Actual replay requires explicit nodes for HID physical `input0` and `input1`:

```bash
sudo build/actions-micro-replay --max-seconds 5 capture.replay \
  /dev/hidraw2 /dev/hidraw3
```

Before the first write, the tool checks both nodes for VID/PID `185b:2d1d` and
the expected physical input suffix. It refuses other devices. hidraw numbering
is not stable and must be discovered through sysfs for each connection.

Replay data is not distributed because it contains captured vendor messages
and captured screen video. Users must capture data from hardware they are
authorized to analyze.

## Work required for a live backend

- reproduce the exact decoder configuration accepted by the firmware;
- send the captured initialization sequence when the backend opens, before
  submitting generated video;
- encode framebuffer input with bounded latency and correct color conversion;
- schedule initialization, keepalive, and video reports across both HID paths;
- handle hotplug, status input, backpressure, and frame dropping;
- validate cold boot, reconnect, and long-running behavior;
- confirm output visually on hardware before changing the support status.
