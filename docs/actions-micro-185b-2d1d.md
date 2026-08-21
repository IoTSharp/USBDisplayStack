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
sequence.

USBDisplayStack now includes an experimental live backend that owns
initialization, `_PPA` keepalives, FFmpeg H.264 encoding, `RRIM/TADV`
encapsulation, and alternating HID report delivery. On 2026-08-07, after a
physical USB power cycle, `bootstrap=full` handed off from an authorized replay
to newly encoded 1920x1080 LVGL output and the HDMI pixels were visually
confirmed. The backend remains experimental pending repeated reconnect and
long-running tests. The host daemon now clears its readiness marker on
disconnect and waits for the same adapter to reappear instead of relying on a
rapid systemd restart loop.

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

## Live backend

Build `usbdisplay-actions-micro.so` with the normal userspace target. The
backend requires an authorized `DPRPL001` replay template. It validates the
whole container. By default it retains only command reports: commands before
the first video report form the initialization sequence, and single-fragment
`_PPA` commands after video starts form the heartbeat cycle. Captured video
bytes are not sent in this default mode.

```bash
build/usb-displayd \
  --device /dev/usbdisplay0 \
  --backend build/usbdisplay-actions-micro.so \
  --backend-option template=/path/to/authorized.replay
```

The backend automatically discovers the two `185b:2d1d` hidraw nodes and
verifies physical `input0` and `input1` before the first write. Device numbers
can be pinned when required:

```text
template=PATH,hid0=/dev/hidraw2,hid1=/dev/hidraw3,encoder=ffmpeg,fps=30,fragment-us=500,encode-timeout-ms=2000,bootstrap=none
```

Some firmware revisions do not leave their waiting page after the command-only
initialization. For those devices, `bootstrap=full` explicitly sends the entire
authorized replay once, preserving its timing, command sequence, video
sequence, and alternating-HID position. The next live configuration and IDR
messages continue in the same transport session, and keepalives resume from the
captured cycle with command endpoints continuing to alternate. This mode sends
the captured screen data contained in the template and is therefore never
enabled implicitly:

```bash
build/usb-displayd \
  --device /dev/usbdisplay0 \
  --backend build/usbdisplay-actions-micro.so \
  --backend-option template=/path/to/authorized.replay,bootstrap=full
```

The virtual mode must have even dimensions because the current encoder output
is baseline H.264 in `yuv420p`. XRGB8888 and RGB565 input are accepted.
Encoded NAL units are normalized to four-byte Annex-B start codes before
transport, matching the captured stream accepted by the adapter.
Initialization preserves captured relative timing. Heartbeats continue through
the backend ABI's idle `tick`, including while the screen is static. Each tick
also polls and reads both HID input interfaces. After at least one heartbeat has
been sent, three heartbeat periods with no device input (with a five-second
minimum) are treated as a lost firmware session.

On a HID error or session-watchdog timeout, the backend closes both stale
hidraw descriptors, locates the unique `185b:2d1d` USB device through sysfs,
and issues `USBDEVFS_RESET` to its `/dev/bus/usb/BBB/DDD` node before the daemon
reopens the backend. The daemon then close/opens `/dev/usbdisplay0`, which makes
the kernel's `last_sequence` fallback deliver the latest frame immediately.
It also resubmits the latest frame every two seconds while the producer is
idle. Each successful transport open atomically publishes `generation=N` in
the ready file so framebuffer consumers can force a complete redraw even when
the marker disappeared and reappeared between their readiness checks.

## Remaining validation work

- validate the meaning of status input reports, backpressure, and frame dropping;
- replace or supplement `USBDEVFS_RESET` with a recovery action that the target
  firmware demonstrably treats like a physical power cycle;
- validate long-running behavior.

Lane 179 field validation on 2026-08-21 proved that HID input reports continue
to increase and that a real unplug/replug recovers through full bootstrap,
generation change, retained/new frame delivery, and a stable LVGL screen.
However, an injected HID write failure followed by a successful
`USBDEVFS_RESET`, full bootstrap, and generation change left the physical
display on its connecting page. Therefore usbfs reset is currently a useful
host-side recovery attempt, but it is not proven equivalent to removing USB
power for this firmware. Host readiness, heartbeat, and frame counters alone
must not be reported as physical display recovery.

## 2026-08-20 physical-link follow-up

The next investigation must start at the USB physical path: connect the adapter
directly to a motherboard USB port, use a short known-good cable, and remove any
passive hub or extension from the path. Capture the daemon log while the adapter
is idle and while frames are moving. The source-specific records are:

- `heartbeat send success|failed`: command endpoint, sequence, template index,
  and the failure `errno` when present;
- `hid write failed`: HID endpoint, descriptor, byte offset, total report length,
  and `errno`;
- `ffmpeg pipe ... failed`: pipe operation, descriptor, and `errno`;
- `usb-displayd: transport lost|transport open generation|reopen_count`: the
  observed loss and the number of completed backend reopens.
- `session watchdog expired|USB reset completed|failed`: the no-input interval
  and whether the matching usbfs device accepted `USBDEVFS_RESET`.

A replay, simulated error, or a clean uninterrupted run is not a physical-link
recovery test. A recovery mechanism passes only when a real stalled session or
equivalent automated fault shows the daemon incrementing `generation`,
returning to a ready state, sending heartbeats, delivering retained and new
HDMI frames, and the physical display visibly returning to stable live output
without a manual unplug/replug.
