# M8 Headless USB descriptor

Captured from an M8 Headless (firmware 6.5.2, Teensy 4.1) enumerating on the Tab5.
VID `0x16C0`, PID `0x048A`, bcdDevice 2.80, composite device (IAD), one configuration of
376 bytes, bus-powered at 100 mA max.

The M8 sends the same descriptors whether its link runs at high or full speed. Its audio
timing is designed for **high speed**, so a full-speed host connected to the M8 directly only
gets a fraction of the audio; see [Audio timing](#audio-timing). This doesn't apply to USB
passthrough: there the computer talks to microm8, not the M8, and microm8 presents its own
full-speed descriptors (see [Passthrough](#passthrough)).

## Interfaces

| Iface | Alt | Class | Purpose | Endpoints |
|---|---|---|---|---|
| 0 | 0 | CDC comm (02/02/01) | Serial control | `0x82` IN interrupt, 16 B, interval 16 |
| 1 | 0 | CDC data (0A) | Display protocol (SLIP) and key input | `0x03` OUT bulk 64 B, `0x83` IN bulk 64 B |
| 2 | 0 | Audio control (01/01/20 = UAC 2.0) | Audio topology | none |
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
| `0x31` | Feature unit | Source `0x03`. Master: mute + volume (read/write); channels 1 and 2: volume |
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

In practice the M8 reports **16.16 samples per data packet** (two microframes): about
`0x000B0660` = 11.025, not the spec's per-microframe 5.51. Without the feedback, the M8's
clock (about 0.02% off the Tab5's) slowly over- or under-runs its input buffer, which is
audible as occasional crackles.

On the Headless, audio sent to `0x05` arrives as the sample recorder's Line (and MIC) source;
the USB source records nothing from it. The input's feature unit (`0x31`) reports a linear
0..255 volume (default 128) rather than the spec's 1/256 dB, and changing it had no effect on
recordings.

A full-speed host interprets `bInterval 2` as 2 ms and can move at most 48 bytes per
packet, so it receives about one eighth of the audio. That's why microm8 talks to the M8 from
the ESP32-P4's high-speed USB controller (the Tab5's USB-A port).

## Passthrough

On USB-C, microm8 is a full-speed device with its own configuration built to match the M8's
(`src/usb/passthru.cpp`): the same identity, serial interfaces and MIDI, and the M8's audio
output on an isochronous endpoint with `bInterval 1` (one packet per 1 ms frame) carrying 44 or
45 frames per packet (one more or fewer when needed to keep about 10 ms buffered). That's the full 44.1 kHz stream at full speed, so the computer gets all
of the audio. The M8's screen and MIDI are bulk data, which full speed carries easily.

The configuration differs from the M8's in a few places:

- Audio out is endpoint `0x81` rather than `0x85`: the P4's full-speed controller has TX FIFOs
  for IN endpoints 1-4 only.
- There's no audio input interface, so MIDI is interface 4 rather than 5. The input's feedback
  endpoint would need a fifth IN endpoint.
