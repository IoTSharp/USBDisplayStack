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

Before live session negotiation was implemented, some sessions did not leave
the waiting page after command-only initialization. `bootstrap=full` remains
an explicit diagnostic mode that sends the authorized captured video once,
preserving its timing, video sequence, and alternating-HID position. Captured
SYNC is replaced by negotiation, session IDs are updated, and command sequences
continue after the handshake. The next live configuration and IDR
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
An encoder restart is handled inside the existing backend session with one
bounded same-frame retry. Its first access unit must contain SPS, PPS, and IDR
before it is submitted; the HID command and video sequence numbers remain
monotonic, so an encoder-only failure does not trigger full bootstrap or a
ready-file generation change.
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

- use the bounded HID input samples to validate the meaning of status reports,
  backpressure, and frame dropping;
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

## 2026-08-21 encoder and USB topology findings

Lane 179 exposes the adapter directly as xHCI root-hub port `1-6`. The root
hub descriptor reports `No power switching`; the device is not behind the
separate Terminus hub, whose descriptor reports ganged rather than per-port
power switching. Therefore `USBDEVFS_RESET`, sysfs authorization changes, and
driver unbind/bind can reset or reprobe the host-side USB session but cannot
remove VBUS from this physical port. They must not be described as equivalent
to the unplug/replug that visibly recovered this firmware.

The periodic failures observed after the 0.2.4 deployment were FFmpeg stdout
EOF events. Immediately before each EOF, command heartbeats and HID input
reports were still advancing, and the kernel journal had no matching USB
disconnect or reset. The daemon previously promoted every encoder EOF into a
full backend reopen; the backend now records the FFmpeg exit code or signal and
performs one local encoder restart before allowing that escalation.

At 18:37 on 2026-08-21, a controlled sysfs `deauthorize -> authorize` cycle
rebound the HID interfaces and restarted the transport, but USB device number
17 did not change. The daemon completed `bootstrap=full`, published a new ready
marker, and continued receiving input reports, while the operator confirmed
that the physical display still showed its connecting page. This is a failed
physical recovery attempt, not a re-enumeration or firmware recovery.

A device-only runtime-power experiment at 18:46 also failed to suspend the
adapter. With the daemon stopped, the unmounted virtual optical drive and USB
device were temporarily changed to `power/control=auto` with zero autosuspend
delay. Port `1-6` remained `active` for all 20 polls,
`runtime_suspended_time` stayed zero, and device number 17 did not change. The
original power controls and daemon were restored. No suspend/resume recovery
claim can be made from this result.

A bounded raw sample taken while the display remained on that page contained
eight 511-byte input0 reports. Their outer headers used report ID 2, message
type 1, one fragment, and a 24-byte payload. The sequence at bytes 4 through 7
increased for every report. The payload began with a repeated length word and
the little-endian tag value `0x50494e47` (`PING`), for example:

```text
02 01 00 00 3e 02 00 00 01 00 00 00 18 00 18 00
00 00 47 4e 49 50 69 98 29 00 03 00 00 00 00 00
```

This identifies the sampled input as a control/keepalive response. In
particular, the values previously seen at bytes 12 and 14 are lengths rather
than a video-ready state. Advancing input-report counts therefore prove that
the firmware control path is alive, but do not acknowledge H.264 acceptance or
physical presentation. A reproducible bounded sample can be collected by
opening the verified input0 hidraw node read-only and reading complete reports:

```bash
timeout 8 dd if=/dev/hidraw0 of=/tmp/actions-input.bin \
  bs=511 count=8 status=none
od -An -v -tx1 -N32 /tmp/actions-input.bin
```

The captured full bootstrap ends with command sequence 16 and video sequence
92. Replaying those fixed values after a successful usbfs reset makes both
streams regress even though that reset does not remove firmware power. The
backend now retains the next command and video sequence high-water marks only
when `USBDEVFS_RESET` succeeds in the same daemon process, and shifts the next
full bootstrap to continue from them only while the usbfs path, including USB
device number, remains unchanged. A failed reset/device disappearance does not
retain the marks, and a changed device number discards them, so a real
re-enumeration continues to use the captured template sequence. This addresses
a concrete non-power-reset session mismatch; physical display recovery still
requires field validation.

Metadata-only inspection of the authorized replay found an initial
configuration message with NAL types `7,8,6`, followed by an IDR message with
NAL type `5`; the captured firmware-compatible transport did not contain AUD.
The live encoder still requests AUD from x264 so logs can prove access-unit
boundaries, but transport continues to omit AUD to match the accepted capture.
After a local encoder restart, the backend requires SPS, PPS, and IDR and logs
the source AUD observation before completing the handoff.

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
- `ffmpeg process ... exit|signal` and `encoder local recovery`: child-process
  cause and the bounded in-session recovery attempt;
- `encoder handoff complete`: SPS/PPS/IDR and source-AUD evidence for the first
  frame after encoder creation;
- `HID input sample`: rate-limited endpoint, report length, and 16-byte prefix
  used to investigate firmware acknowledgements without dumping full reports;
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

## 2026-09-07 lane 179 startup-splash validation

The operator authorized diagnosis, deployment, restart, and physical USB tests
on `192.168.137.179`. Its primary framebuffer is `fb0=inteldrmfb`; the USB
second screen is `fb1=usbdisplay`. The second-screen application uses
`--framebuffer /dev/fb1`.

The kernel recorded a real USB disconnect at 14:57:05 CST and new device
numbers at 14:57:18 and 14:57:20. After the startup-splash fix was deployed,
the daemon logged a successful backend submission at 15:09:26. The operator
still saw the UGREEN connecting page. Neither the real re-enumeration nor this
submission proves physical display recovery in this incident.

The installed `/var/lib/usbdisplay/actions-micro.replay` is 633,264 bytes,
with SHA-256
`f4769abebc7a34b65ec53270ef188f85e0b0da0572da7e47b445e705d113fe1e`.
It contains 17 command reports and 137 video reports over 4.565229 seconds,
ending at command sequence 16 and video sequence 92. Its 154-record size
matches the later successful live-backend test in `testing.md`; the earlier
1,251-record capture is a different sample. The record-count difference alone
does not establish a damaged template.

Later in the same daemon session, a bounded read of input0 returned 511-byte reports
with a 39-byte payload. Only the outer sequence changed in the two samples:

```text
02 01 00 00 c2 03 00 00 01 00 00 00 27 00 27 00
00 00 43 4e 59 53 c6 23 ff ff 03 00 00 00 ...
```

| Direction / Payload Tag (Little Endian) | Observation | Unconfirmed Meaning |
| --- | --- | --- |
| Device to host / `0x50494e47` (`PING`) | Earlier 24-byte control payloads | Video acceptance or presentation |
| Device to host / `0x53594e43` (`SYNC`) | Repeated 39-byte control payloads in this incident | Required response, session fields, or video state |
| Host to device / `0x53594e43` (`SYNC`) | Two discovery requests, then acknowledgement of captured device session `0x4567` | Whether that old session still belongs to the attached device |

The template's first two host `SYNC` payloads advertise local session `0x0029`
and unknown peer `0xffff`. The third acknowledges the captured peer `0x4567`:

```text
18 00 00 00 43 4e 59 53 29 00 ff ff 03 00 00 00
01 00 01 80 00 00 00 00

18 00 00 00 43 4e 59 53 29 00 67 45 03 00 00 00
01 00 01 80 00 00 00 00
```

At the time of this failed test, the backend counted device inputs without
interpreting their payloads. The bounded sample can be
reproduced after verifying the current input0 node (the output is truncated to
exclude the serial-number field):

```bash
timeout 8s dd if=/dev/hidraw0 bs=511 count=2 status=none |
  od -An -v -tx1 -w511 | cut -c1-90
```

### Vendor implementation and session mismatch

The adapter's read-only virtual CD-ROM `/dev/sr0` contains `/UDISPLAY.EXE;1`.
The 7,755,672-byte PE32 executable has SHA-256
`0ebe9b36555e8676f6b6bce31bd310fc6486f579d47f0e57ac49b0e6e6e4e8d2` and
PE build timestamp 2024-07-23 11:40:30. It was inspected statically, not run.
Import-table evidence includes HID descriptor APIs, `ReadFile`, `WriteFile`,
`DeviceIoControl`, registry and process APIs, and Winsock/WinHTTP APIs.

With radare2 6.2.0, `rabin2 -ij UDisplay.exe` reproduces the import inventory.
`radare2 -q -e scr.color=0 -c 'pd 80 @ 0x54fe30' UDisplay.exe` shows the
SGUP v3 SYNC sender. It writes two 16-bit object fields into message offsets
8 and 10 and logs `send sync. localSsid:0x%04x remoteSsid:0x%04x`.
Related protocol strings include `remote session change, resync` and
`ssid diff, ignore`. These establish session IDs rather than constant magic.

All commands after discovery in the template carry `29 00 67 45`. The Linux
live-video sender also hardcoded these same four bytes as `protocol_id`.
The attached device instead advertised session `0x23c6`, with peer `0xffff`.
Consequently, successful HID writes were transmitting messages for an old
session without completing the attached device's synchronization.

The backend now negotiates a fresh host session before replaying initialization,
requires the device to echo the host ID, and acknowledges the device ID. The
confirmation may be a matching `RRIM` video request rather than a separate
SYNC echo, as observed on lane 179.
Negotiation is bounded by five seconds, 100 polls, and 12 sends. Captured SYNC
reports are replaced by this exchange; command sequence numbers continue after
it. Initialization, heartbeats, bootstrap video, and live video use the negotiated
IDs. Only the first fragment contains a session header; continuation bytes are
preserved. A later SYNC announcing changed IDs returns `ESTALE`, causing the
daemon to reopen, negotiate, and deliver the retained framebuffer again.

At 15:47:21 CST, lane 179 confirmed a live session using an `RRIM` response
with host ID `0x14a0` and device ID `0x5cff`. After live video began, the
operator confirmed that the physical screen displayed LaneApp. A later
`bootstrap=none` restart negotiated different IDs and sent fresh configuration
and IDR messages without replaying captured video. At 15:51:51, a controlled
fb1 reload submitted the restored daemon splash; LaneApp resumed five seconds
later. The primary `fb0=inteldrmfb` remained present throughout both tests.
