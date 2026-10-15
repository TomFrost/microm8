# AGENTS.md

Notes for AI coding agents working on microm8. Read [README.md](README.md) first for what the
project does and how it's used; this file covers what you need to change it safely.

## What this is

Arduino-on-ESP-IDF firmware for one board only: the **M5Stack Tab5** (ESP32-P4). A Teensy 4.1
running **Dirtywave M8 Headless** plugs into the Tab5's USB-A port (the P4's high-speed USB host
controller). microm8 renders the M8's screen, plays its USB audio through the Tab5's codec,
sends touch key presses back, streams the Tab5's mics into the M8's USB audio input, and passes
the M8 through to a computer on USB-C. There is no other target board; don't add board
abstraction layers.

The M8 Headless firmware itself is closed source and not part of this repo. Its USB descriptor
and audio timing are documented in [docs/m8-headless-usb.md](docs/m8-headless-usb.md).

## Build and test

```bash
pio run                      # build (env tab5)
pio run -t upload -t monitor # flash and watch the log
pio test -e native           # host-side unit tests (M8 protocol parser and renderer)
```

- The platform is **pioarduino**, pinned by URL in `platformio.ini`. The stock PlatformIO
  espressif32 platform won't work (its USB host stack can't do isochronous audio).
- Build with the **pioarduino core** (PyPI package `pioarduino`, 6.2.0 or later; the pioarduino
  IDE extension installs it at `~/.platformio/penv/bin/pio`). The upstream PlatformIO Core
  pins different build tools (SCons) in the shared `~/.platformio`, and running both makes
  builds fail with `No module named 'SCons.Tool.FortranCommon'`. If `pio --version` says
  6.1.x, you have the wrong one.
- `custom_sdkconfig` makes pioarduino rebuild the Arduino core. The first build, and the first
  after editing `platformio.ini`, takes several minutes. Editing `platformio.ini` also wipes
  `.pio/build`, so don't edit it while a build is running.
- Generated files (`sdkconfig.*`, `CMakeLists.txt`, `managed_components/`, `dependencies.lock`)
  are gitignored; don't commit them.
- Every build writes `.pio/build/<env>/firmware.factory.bin`: bootloader, partition table and
  app merged into one image flashed at `0x0`.
- `tab5` targets ESP32-P4 chip revisions before v3 (the board definition's "ES" variant).
  `tab5_r3` is the same firmware for rev v3 chips, untested on hardware. pioarduino keeps one
  rebuilt core at a time, so switching between the two envs rebuilds the core (several
  minutes); see the comment on `tab5_r3` in `platformio.ini`.

## Releases

`.github/workflows/release.yml` runs on a `v*` tag push: native tests, then each env built in
its own job, then a GitHub release with `microm8-tab5.bin`, `microm8-tab5-rev3.bin` and
`SHA256SUMS.txt`. The README's install steps link to the latest release's
`microm8-tab5.bin`, so keep that name. Running the workflow by hand (workflow_dispatch) builds
the images as an artifact without releasing.

## Testing on hardware

Almost everything here (USB timing, audio, display tearing, touch feel) can only be verified on
a real Tab5 with a Teensy running M8 Headless. If you can't run on hardware, say so plainly in
your summary and list what the human should check. Don't claim something works because it
compiles.

- **Flashing while the M8 is connected:** USB-C is the M8 (passthrough), not the Tab5's
  serial port. Run `python3 tools/passthru.py serial` first (needs libusb), or unplug the
  Teensy. `python3 tools/passthru.py stats 10` reads passthrough audio counters over a vendor
  request, since the log isn't reachable while USB-C is the M8.
- **Screenshots:** `python3 tools/screenshot.py out.png` over the serial port (close any
  monitor first). The README screenshots come from this.
- **On-screen log:** while no M8 is connected, the last 12 log lines show in the M8 area.
- **Power:** build with `-DMICROM8_POWER_DIAG=1` for a power readout (meaningful on battery
  only).

## Architecture in brief

See the module table in the README's "How it works" section. Key points:

- Tasks: USB host, passthrough and audio on core 0; UI and touch on core 1. M8 serial data
  reaches the UI task through a stream buffer so a slow redraw never stalls USB.
- **Never write the panel framebuffer (PSRAM) from the CPU in bulk.** Pushing that much data
  through the shared cache slowed the other core enough to drop USB audio packets. The M8 area
  is scaled into small internal-RAM buffers and copied by the DSI driver's 2D DMA, timed
  against the panel's scan to avoid tearing (`src/display/ui.cpp`).
- The touch controller, codec, I/O expanders, battery gauge and IMU share one internal I2C bus
  used from both cores. Every access goes through `board::I2CLock`.
- Audio clocks (M8 vs. Tab5 I2S) drift; both directions absorb it by dropping or repeating
  single frames. Audio to the M8 is sized from its feedback endpoint, which reports 16.16
  samples per 2-microframe packet, not per microframe as the spec says.
- Passthrough (`src/usb/passthru.cpp`) swaps the P4's two full-speed PHYs so the OTG 1.1
  controller drives USB-C, then runs TinyUSB with the M8's identity. Constraints:
  - The full-speed controller has TX FIFOs 0-4 only, and TinyUSB uses FIFO = endpoint number,
    so IN endpoints must be 1-4. That's why audio is `0x81`, and why the M8's audio input
    (whose feedback endpoint would be a fifth IN endpoint) isn't passed through. Implicit
    feedback from the audio IN endpoint might get around that; it's untested.
  - Don't use LP_SYS `force_download_boot` to get into the ROM bootloader: it survives resets
    and leaves the Tab5 stuck in ROM.
  - `passthru::start()` must run after `audio::start()`, because routing the I2S pins turns
    off the OTG pads passthrough turns on.
- USB-C can't supply 5 V (it's the charger input), so a USB device on USB-C would need power
  from elsewhere. Running the full-speed controller as a second host (USB MIDI devices, or
  USB-C headphones with resampling to 48 kHz) alongside the M8 on USB-A is untested.
- ESP-IDF's USB host can't run a full-speed device behind a high-speed hub, so hubs on USB-A
  don't work.

## UI conventions

The project owner cares a lot about these; follow them unless asked otherwise.

- Nothing is ever drawn over the M8 screen. Popups open under the status bar (portrait) or in
  the left panel (landscape).
- Held keys pass straight through to the M8. Don't add microm8-side key repeat or timing: it
  breaks the M8's own hold handling (for example SHIFT+DOWN into the song view).
  `ARROW_REPEAT_MS` stays 0.
- Alignment and consistent sizing matter. Mock up layout changes (screenshots) before
  committing to them, and keep `static_assert`s in `keypad_layout.cpp` passing rather than
  loosening them.
- Keys are dimmed M8 cyan (`rgb(0, 180, 180)`), white while held.
- The mic always starts off at boot. HOLD, once on, stays on until its icon is tapped.
- Only add features other users can use without hardware modifications.

## Writing style

Applies to code comments, docs and commit messages.

- American spelling (color, center, behavior, initialize).
- No em dashes. Use a colon, a comma, parentheses or a new sentence.
- Plain, direct sentences. No marketing filler in comments.
- Comments describe how things work now and why. No project history, no "previously",
  "used to", or notes about approaches that were tried and dropped. Git history holds that.
- Match the existing comment density: a short comment on each non-obvious function, constant
  or block, and none on obvious code.
- Code style: 2-space indent, `constexpr` constants named `kCamelCase`, file-local code
  in anonymous namespaces, one namespace per module (`m8usb`, `passthru`, `ui`, `audio`, ...).
- Don't add AI attribution (`Co-Authored-By`, "Generated with ...") to commits or PRs.

## Licensing

microm8 is MIT. Code adapted from m8c, the SLIP decoder and M5Unified keeps its attribution in
the file header and in [LICENSE-THIRD-PARTY.md](LICENSE-THIRD-PARTY.md). If you adapt more
third-party code, add it there too.
