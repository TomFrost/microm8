# microm8

Standalone firmware that turns an **M5Stack Tab5** (ESP32-P4) into a battery-powered handheld
for the **Dirtywave M8 Headless** running on a Teensy 4.1.

The Teensy plugs into the Tab5's USB-A port. The Tab5 powers it, renders the M8's screen,
plays its audio through the speaker or headphone jack, and sends key presses from an
on-screen multi-touch keypad. It works in portrait, with the M8's own key layout, and in
landscape, with a bigger M8 screen and keys at the sides.

Plugged into a computer while the M8 is connected, the Tab5's USB-C port passes the M8 through,
so m8c or [m8.run](https://m8.run) can use it too (see [Using a computer](#using-a-computer)).

The protocol handling comes from [m8c](https://github.com/laamaa/m8c), reimplemented for the
ESP32.

<p>
<img src="docs/screenshot-landscape-compact.png" width="560" alt="Landscape with compact keys: the M8's song view at 3x in the middle; status icons, the arrow pad, and OPTION and SHIFT in the bottom corner on the left; the power, key layout and HOLD icons over a 2x2 grid of OPTION, EDIT, SHIFT and PLAY on the right">
<img src="docs/screenshot-portrait-compact.png" width="177" alt="Portrait with compact keys: the M8's song view across the top, the status bar under it, then the arrow pad beside a 2x2 grid of OPTION, EDIT, SHIFT and PLAY">
</p>

*Landscape and portrait, with compact keys. Screenshots from the device (see
[Screenshots](#screenshots)).*

## Layout

The screen follows the Tab5's accelerometer through all four orientations. The rotation icon
locks it; tap it again to unlock. The lock and the last orientation are saved.

The M8's 320×240 screen is scaled with pixel-area resampling. Each display pixel averages the
M8 pixels it covers, so text stays sharp at non-integer scales.

**Portrait** (720×1280):

- **M8 screen at 2.25×** (720×540) across the full width at the top.
- **Status bar:** connection and firmware version on the left; mic, key layout, HOLD, volume,
  brightness, rotation and power icons on the right.
- **The M8's own key layout** along the bottom: the cursor keys as an inverted T, SHIFT/PLAY
  directly under DOWN/RIGHT, and OPTION/EDIT up and to the right of UP, as on the hardware.
  Keys are 144 px (about 12.5 mm).

**Landscape** (1280×720):

- **M8 screen at 3×** (960×720), centred.
- **Left margin:** the volume, brightness, rotation and mic icons; under them a panel with
  the arrow pad (or an open slider, the power details or a message); OPTION over SHIFT in the
  bottom-left corner.
- **Right margin:** the power, key layout and HOLD icons; under them EDIT, OPTION, PLAY and
  SHIFT, stacked up from the bottom-right corner. Landscape keys are low (88 px), so they're
  easy to reach from the bottom corners.
- No connection text, to keep the margins clear.

**Compact keys** (the key layout icon toggles them, in either orientation): the arrow pad for
the left thumb, and the M8's own right-hand block for the right, OPTION and EDIT over SHIFT and
PLAY, drawn as symbols (⌥, a pencil, ⇧, ▶).

- **Portrait:** the arrow pad and the block, full size, centred between the status bar and
  the bottom of the screen.
- **Landscape:** the block is a square grid level with the arrow pad. OPTION and SHIFT appear
  again in the bottom-left corner, so every chord has a key under each thumb: a touchscreen
  can't press two keys under one thumb, as you would OPTION+EDIT on the hardware.

Keys are drawn in the M8's cyan (dimmed); while held, they turn white and their touch area is
outlined.

<p>
<img src="docs/screenshot-landscape-normal.png" width="560" alt="Landscape with the normal keys: the arrow pad, then OPTION over SHIFT on the left; EDIT, OPTION, PLAY and SHIFT stacked up from the bottom on the right">
<img src="docs/screenshot-portrait-normal.png" width="177" alt="Portrait with the M8 key layout: OPTION and EDIT over the cursor keys, with SHIFT and PLAY under DOWN and RIGHT">
</p>

*The normal keys (compact keys off), landscape and portrait.*

To turn portrait upside down, uncomment `-DTAB5_ROTATE_180=1` in `platformio.ini`. If tilting
picks the wrong orientation, the `TAB5_GRAVITY_*` settings in `src/config.h` map the
accelerometer's axes to orientations; the log's `orientation:` lines show the readings.

## Controls

- **Keys:** every key a finger is touching counts as held, so M8 combos work as on the
  hardware: hold SHIFT, EDIT or OPTION and tap or hold a cursor key. Each key's touch area
  reaches halfway into the gap around it, so near misses still count without neighbouring
  keys overlapping.
- **Arrows in landscape:**
  - **Arrow pad:** the four arrows in the left panel. Each arrow owns a triangle of the
    panel, cut by the diagonals of the square around the arrows, so the whole panel is
    touchable.
  - **Swipes:** swipe right, left, up or down anywhere on the left half of the screen (the M8
    screen included). Keep the finger down to hold the arrow; lifting it releases.
- **Volume icon** (a speaker, or headphones when they're the output): opens a row with an
  output selector (speaker and headphones, the selected one lit) and a volume slider (0–10).
  Plugging headphones in switches to them automatically, and unplugging switches back; at
  power-on the output follows the jack. Each output keeps its own volume.
- **Brightness icon** (sun): opens a brightness slider. The lowest step is 30%, where the
  backlight stops dimming. Dimming also cuts power: about 0.8 W between 30% and full.
- **Rotation icon:** locks or unlocks rotation (crossed out in red when locked).
- **Key layout icon:** switches compact keys on or off (lit while on).
- **HOLD icon** (an H key cap): for playing chords one key at a time, with one hand or one
  finger. While HOLD is on (the icon fills white), tapping SHIFT or OPTION holds it down until
  it's tapped again, whatever else you press in between: SHIFT, then RIGHT, RIGHT, RIGHT, then
  SHIFT again. HOLD stays on until you tap its icon.
- **Power icon:** a battery filled and coloured by charge (green when full, yellow around
  half, red when low); an empty battery with a green bolt while charging; a full battery with
  a light bolt when plugged in and full; a plug when no battery is fitted. Tap it for the
  details in words.
- **Sliders:** once a finger grabs a slider, the slider follows it until it lifts, wherever
  it moves. Slide your thumb off to see the knob.
- **Mic icon:** opens the mic row: an off/on switch and a boost selector (see Recording below).
  Struck through while off, red while live.
- Rows open under the status bar (in landscape, in the left panel) without covering the M8
  screen, and close when you tap the icon again or 4 seconds after the last touch. Volumes,
  output, brightness, mic boost, compact keys and HOLD are saved to flash.

## Recording

The Tab5's two microphones can be recorded on the M8 like any input:

1. Tap the mic icon and switch the mic on (it starts off at every power-on, so the speaker
   can't unexpectedly feed back into it). The icon turns red.
2. In the M8's sample recorder, set SRC to Line (or MIC: on the Headless they're the same
   input). The USB source records nothing from the Tab5.
3. Set the level with the recorder's VOL; FF gives a good level for speech within about a
   foot. If that's not enough, the mic row's boost adds +6 or +12 dB digitally, which raises
   the noise along with the signal.

The mics arrive as stereo (left and right mic), streamed to the M8's USB audio input at the
M8's own clock rate (from its feedback endpoint), so recordings don't crackle. Use headphones,
or turn the M8's input monitoring down, if the speaker feeds back into the mics.

## Using a computer

While the M8 is connected, the Tab5's USB-C port presents it to a computer as if it were
plugged in directly: same name, same USB IDs, the same serial port, and its audio as a
44.1 kHz stereo input called "M8". m8c, [m8.run](https://m8.run) and other M8 tools find it
as usual, and a DAW can record it and use its USB MIDI. The Tab5 keeps showing the M8 and
playing its audio throughout.

- **Keys** pressed on the computer and on the Tab5 combine: hold SHIFT on the touchscreen and
  press an arrow key on the computer, or the other way round.
- **Closing m8c or m8.run** doesn't stop the M8's screen on the Tab5: their "disconnect"
  command isn't passed on.
- **Not passed through:** the M8's audio input (sending audio from the computer into the M8's
  sample recorder). The Tab5's USB-C controller runs out of endpoints for it. Record into the
  M8 from the Tab5's mics instead (see Recording).
- **The link is full speed** (12 Mbit/s), which carries the M8's screen, stereo audio and MIDI
  with room to spare.

With no M8 connected, USB-C is the P4's own USB serial port, for flashing, logs and
screenshots. With the M8 connected, `python3 tools/passthru.py serial` (needs
`brew install libusb`) gives USB-C back to the serial port until the M8 reconnects or the Tab5
restarts, so you can flash, watch the log or take a screenshot; flashing restarts the Tab5, which
then passes the M8 through again. `python3 tools/passthru.py stats 10` prints the passthrough's
audio counters. To keep USB-C as the serial port at all times, set `MICROM8_USB_PASSTHROUGH` to
0 in `src/config.h`.

## Hardware setup

- **M8:** connect the Teensy 4.1's micro-USB to the Tab5's **USB-A** port with a data cable.
  The Tab5 supplies 5 V on that port, so the Teensy needs no other power.
- **USB-C:** charging, and either the M8 passed through to a computer (while the M8 is
  connected) or flashing and serial logs (while it isn't). See [Using a computer](#using-a-computer).
- **Battery:** the Tab5's NP-F550 pack powers everything.

## Building and flashing

You need [PlatformIO](https://platformio.org/). The project uses the
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform (Arduino core 3.x
on ESP-IDF 5.5), because the stock PlatformIO platform's ESP-IDF 4.4 USB host stack can't do
isochronous transfers, which USB audio needs.

```bash
pio run -t upload -t monitor
```

While the M8 is connected, USB-C is the M8 passed through rather than the Tab5's serial port:
run `python3 tools/passthru.py serial` first, or unplug the M8.

The first build takes several minutes: `custom_sdkconfig` makes pioarduino rebuild the Arduino
core from ESP-IDF source (see [Troubleshooting](#troubleshooting)). Later builds take seconds.

### Tests

The M8 protocol parser and framebuffer renderer have host-side unit tests:

```bash
pio test -e native
```

### Screenshots

`tools/screenshot.py` saves exactly what's on the screen as a PNG, over the USB-C serial port
(it sends `S`; the firmware replies with its framebuffer, see `src/diag/screenshot.cpp`). It
needs only Python 3, on macOS or Linux. Close any serial monitor first, since the port can't be
shared. While the M8 is connected, run `python3 tools/passthru.py serial` first to get the serial
port back:

```bash
python3 tools/screenshot.py landscape.png
```

## Architecture

| Module | Role |
|---|---|
| `src/usb/m8_usb.cpp` | USB host client on the P4's high-speed controller. Finds the M8's serial and audio interfaces by parsing descriptors, sets DTR/RTS and line coding, keeps bulk and isochronous transfers in flight, and forwards the M8's USB MIDI for passthrough. While the mic is on, streams to the M8's audio input, sizing each packet from the M8's rate feedback. Replaces m8c's libusb backend. The M8's descriptor is decoded in [docs/m8-headless-usb.md](docs/m8-headless-usb.md). |
| `src/usb/passthru.cpp` | USB passthrough on USB-C. While an M8 is connected, swaps the P4's two full-speed PHYs so its OTG 1.1 controller drives USB-C (the Serial/JTAG controller moves to the PHY on the I2S pins, with its pads off), then runs a TinyUSB device with the M8's identity: one class driver owns the whole configuration (CDC serial, UAC2 audio to the computer, USB MIDI), numbered as on the M8 except audio, which moves to endpoint 0x81 (the controller has TX FIFOs for IN endpoints 1-4 only). Vendor requests read audio counters and hand USB-C back to Serial/JTAG (`tools/passthru.py`). Copies of the M8's serial, audio and MIDI go to the computer; the computer's commands go to the M8, with its key state merged into the touch keys and its disconnect command dropped. Audio packets are queued from the USB interrupt, 44 or 45 frames per 1 ms, adjusted to keep about 10 ms buffered. |
| `src/m8/slip.c`, `protocol.cpp` | SLIP framing and parsing of M8 commands: rectangle, character, oscilloscope and system info. Ported from m8c. |
| `src/m8/screen.cpp` | 320×240 RGB565 framebuffer in PSRAM, drawn with m8c's semantics: font offsets, background-colour tracking, scope clearing. Uses m8c's five bitmap fonts. Tracks changed regions as 8-row bands. |
| `src/display/ui.cpp` | Portrait and landscape layouts. Changed areas of the M8 screen are scaled (pixel-area resampling) in the panel's native orientation into internal RAM, then copied into the panel's framebuffer by the DSI driver's 2D DMA, timed against the panel's scan so updates don't tear. Keys, status icons, the volume/brightness/power row and the landscape arrow pad are drawn through M5GFX. `splash.cpp` shows the splash and on-screen log while no M8 is connected. |
| `src/input/touch.cpp`, `keypad.cpp`, `keypad_layout.cpp` | Multi-touch through M5Unified, and the portrait and landscape key layouts and hit-testing. `main.cpp` gives each finger a role (keys, slider, swipe, arrow pad) and produces the M8 key bitmask (`'C'` + mask). |
| `src/audio/audio_out.cpp` | Ring buffer for USB audio, clock-drift compensation (drops or repeats single frames), a silence gate so the codec goes quiet between songs, per-output volume levels, mono mix for the speaker. |
| `src/audio/audio_hw.cpp` | ES8388 codec on I2S0. Volume is set in the codec's analog output stage, so its noise floor drops with the volume. Speaker amp enable, headphone detect. The ES7210 mic ADC on the same bus. |
| `src/audio/mic.cpp` | Mic capture: reads the mics into a buffer that the USB task drains at the M8's rate, dropping or repeating single frames to absorb clock drift. Optional digital boost. |
| `src/board.cpp` | M5Unified bring-up, USB-A 5 V, and the lock for the shared internal I2C bus. Switches off hardware microm8 doesn't use (the ESP32-C6 wireless chip and the camera). Battery state from the INA226 current sensor: charging, plugged in, or no battery. |

FreeRTOS tasks: USB (host and passthrough) and audio run on core 0; display and touch run on core 1. Serial data
passes from USB to the UI task through a stream buffer, so a slow display update never stalls
USB.

## Status

**Working on the Tab5:** the M8 connects over high-speed USB, and its screen, keys, and audio
through the speaker and headphones all work, as does recording from the Tab5's mics. Passed
through to a computer on USB-C, m8.run gets the M8's screen, keys and audio while a DAW plays
from its USB MIDI, with the Tab5 running alongside.

### Shelved ideas

- **USB MIDI devices on USB-C:** the P4's second (full-speed) USB controller could host a MIDI
  keyboard or a DIN-MIDI cable on the Tab5's USB-C port and forward to the M8's USB MIDI.
  Shelved because USB-C can't power them: it's the charger's input, so nothing on the Tab5
  supplies 5 V there, and feeding it from the Tab5's own 5 V would just charge the battery from
  itself. A hub on USB-A doesn't work either: ESP-IDF's USB host can't run a full-speed device
  through a high-speed hub (no transaction translator support) alongside the high-speed M8.

## Troubleshooting

**Measuring power draw:** build with `-DMICROM8_POWER_DIAG=1`. The status bar then shows the
power drawn from the battery, the log prints a power summary every 10 seconds, and boot
measures what switching off the C6 and camera saves (about 0.2 W). Only meaningful on
battery.

**While no M8 is connected, the M8 area shows the last 12 log lines**, so many problems can be
diagnosed without a computer. Errors are red and warnings amber.

**"Waiting for M8" and the `port:` log line shows `connected=0` the whole time.** The Tab5
sees nothing on the data lines. The cable is probably charge-only, or the Teensy isn't
running M8 Headless. Test both by plugging the Teensy into a computer with the same cable and
checking that m8c finds it.

**The log shows `ENUM: Abort request of enumeration process (0x48a:0x16c0)`.** ESP-IDF
rejected the device because no enumeration filter approved it. The Arduino core enables the
filter hook, and ESP-IDF then rejects anything a filter hasn't approved. The firmware
installs a filter that accepts all devices; if you see this, that code is missing.

**The log shows `Configuration descriptor larger than control transfer max length`.** The
USB stack's control-transfer limit is too small for the M8's 376-byte descriptor.
`platformio.ini` raises it through `custom_sdkconfig`. Check that setting is still there and
do a clean build (`pio run -t clean`, then build again).

**The build fails with `No module named 'SCons.Tool.FortranCommon'` (or another SCons
error).** pioarduino swaps build-tool versions during a build, mostly on the first run after
`custom_sdkconfig` changes, and with more than one PlatformIO Core installed. Run the build
again. If it keeps happening, delete `~/.platformio/packages/tool-scons` and retry.

## History

microm8 began on a Guition JC4827W543 (ESP32-S3, 4.3" 480×272). That version rendered the M8
and took touch input, but the S3's USB is full-speed only. The M8's audio is timed for
high-speed USB, so a full-speed host receives only about an eighth of it, and the Headless
doesn't output audio on I2S either. That's why the project moved to the Tab5. The
JC4827W543 code remains in the git history: commit `0821825` is the last version with it.

## License

MIT; see [LICENSE](LICENSE). Portions are derived from m8c, the SLIP decoder and M5Unified
(all MIT); see [LICENSE-THIRD-PARTY.md](LICENSE-THIRD-PARTY.md).
