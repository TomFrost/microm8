# M8 Headless USB descriptor

Captured from an M8 Headless (firmware 6.5.2, Teensy 4.1) enumerating on the ESP32-S3 host.
VID `0x16C0`, PID `0x048A`, bcdDevice 2.80, composite device (IAD), one configuration of
376 bytes, bus-powered at 100 mA max.

The descriptors are the same whether the link runs at high or full speed. The audio timing is
designed for **high speed**, so a full-speed host (such as the ESP32-S3) only gets a fraction
of the audio; see [Audio timing](#audio-timing).

## Interfaces

| Iface | Alt | Class | Purpose | Endpoints |
|---|---|---|---|---|
| 0 | 0 | CDC comm (02/02/01) | Serial control | `0x82` IN interrupt, 16 B, interval 16 |
| 1 | 0 | CDC data (0A) | Display protocol (SLIP) and key input | `0x03` OUT bulk 64 B, `0x83` IN bulk 64 B |
| 2 | 0 | Audio control (01/01/20 = UAC 2.0) | Audio topology | — |
| 3 | 1 | Audio streaming | **M8 → host** audio | `0x85` IN iso, async, data, 48 B, interval 2 |
| 4 | 1 | Audio streaming | **Host → M8** audio (input, for sampling) | `0x05` OUT iso, async, data, 48 B, interval 2; `0x86` IN iso feedback, 4 B, interval 4 |
| 5 | 0 | MIDI streaming (01/03) | USB MIDI | `0x04` OUT bulk 64 B, `0x84` IN bulk 64 B |

Alt 0 of interfaces 3 and 4 has no endpoints (zero bandwidth); select alt 1 with
`SET_INTERFACE` to stream.

## Audio topology (UAC 2.0)

| ID | Unit | Details |
|---|---|---|
| `0x3A` | Clock source | Internal fixed clock, frequency control read-only. Query `GET_CUR` (sampling frequency) to confirm the rate; expected 44.1 kHz. |
| `0x01` | Input terminal | Type `0x0602` (digital audio interface), 2 ch (FL, FR) |
| `0x02` | Output terminal | Type `0x0101` (USB streaming), source `0x01` → interface 3 |
| `0x03` | Input terminal | Type `0x0101` (USB streaming), 2 ch, fed by interface 4 |
| `0x31` | Feature unit | Source `0x03`. Master: mute + volume (read/write); channels 1–2: volume |
| `0x04` | Output terminal | Type `0x0602` (digital audio interface), source `0x31` |

So: **M8 audio → IT 1 → OT 2 → USB IN `0x85`**, and **USB OUT `0x05` → IT 3 → FU `0x31`
(mute/volume) → OT 4 → M8 input**.

## Stream format (both directions)

AS general: PCM, 2 channels, channel config `0x00000003` (front left, front right).
Format type I: 2-byte subslots, 16-bit resolution. The sample rate comes from clock `0x3A`.

## Audio timing

At high speed, `bInterval 2` means one packet every 2 microframes = **250 µs**. At 44.1 kHz
stereo 16-bit that is 11.025 frames = ~44 bytes per packet (max 48 = 12 frames).

The feedback endpoint `0x86` (`bInterval 4` = every 8 microframes = 1 ms) reports the
M8's actual rate in the high-speed feedback format (16.16 fixed point, samples per
microframe; nominally ~5.51 for 44.1 kHz). A host sending to `0x05` must size its packets
from this value to stay locked to the M8's clock.

A full-speed host interprets `bInterval 2` as 2 ms and can move at most 48 bytes per
packet, so it receives about one eighth of the audio. This is why USB audio fails on the
ESP32-S3 and needs a high-speed host such as the ESP32-P4.
