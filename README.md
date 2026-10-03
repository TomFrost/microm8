# microm8

Standalone firmware that turns a **Guition JC4827W543** (ESP32-S3, 4.3" 480×272 capacitive touchscreen, speaker) into a screen, speaker and keypad for a **Dirtywave M8 Headless** running on a Teensy 4.1.

The Teensy plugs into the board's USB-C port. The ESP32-S3 acts as the USB host. It renders the M8's display, plays its audio through the onboard speaker, and sends key presses from an on-screen multi-touch keypad.

The protocol handling comes from [m8c](https://github.com/laamaa/m8c), reimplemented for the ESP32.

![Screen layout](docs/layout-preview.png)

*Layout preview at 2× scale, with SHIFT and RIGHT held. The M8 area shows test output from the unit-test harness.*

## Screen layout: why landscape

The M8 Headless draws a **320×240** screen. The panel is **480×272**.

| Orientation | Width available | Fits 320×240 at 1:1? |
|---|---|---|
| Portrait (272×480) | 272 px | **No.** It would have to shrink to 85%, which garbles the M8's 5×7 pixel font. |
| Landscape (480×272) | 480 px | **Yes**, and it leaves a free **160×272** column plus a 320×32 strip. |

Landscape at 1:1 leaves enough room for all eight M8 keys on screen. Each key is about **53–80 px × 64 px**, or about 10.5–16 mm × 12.7 mm on this panel, which is comfortable for fingertips. The keys keep the M8's grouping, folded into a column:

```
 ┌───────┬───────┐
 │ SHIFT │ PLAY  │
 ├───────┼───────┤
 │OPTION │ EDIT  │
 └───────┴───────┘
     ┌───┐
     │ ▲ │
 ┌───┼───┼───┐
 │ ◀ │ ▼ │ ▶ │
 └───┴───┴───┘
```

The strip under the M8 screen is a status bar showing connection state, firmware version and volume.

To put the keypad on the left instead, add `-DMICROM8_PANEL_LEFT=1` to `build_flags` in `platformio.ini`.

### Why not a Bluetooth gamepad?

The ESP32-S3 only supports **Bluetooth LE**. The 8BitDo Pro 2 (like most gamepads) uses **Bluetooth Classic (BR/EDR)** in every mode, so the two can't pair. See [Bluepad32's notes](https://bluepad32.readthedocs.io/en/latest/supported_gamepads/). A BLE-only controller could be added later. The on-screen keypad makes a gamepad unnecessary.

## Controls

- **Keys:** the GT911 tracks up to 5 fingers. Every key a finger is touching counts as held, so M8 combos work as on the hardware: hold SHIFT, EDIT or OPTION and tap or hold a cursor key. Sliding a finger across the cursor keys moves from key to key with no dead space in between.
- **Volume:** use the `−`/`+` buttons in the status bar (0–10). The setting is saved to flash.
- **Speaker / headphones:** with the optional headphone DAC built in, an `SPK`/`HP` button appears in the status bar. Only the selected output plays, and each output keeps its own volume.

## Hardware setup

### Power (important)

The board's USB-C VBUS line goes **into** the board through a Schottky diode (D1). **The board cannot power the Teensy over USB**, and its own LiPo circuit can't either (the boosted battery output only reaches test pad S1). Supply 5 V some other way.

**Simplest: one 5 V source into the Teensy.** Feed a regulated 5.1–5.2 V supply (for example an 18650 → charger → switch → 5 V boost) into the Teensy 4.1's **VIN** and **GND** pins. Leave the VUSB–VIN pad joined, as it ships. The Teensy then drives VBUS on its USB jack, and the display is powered back through the USB cable and D1. Notes:

- Leave the display's BAT connector empty, or its charger chip will try to charge that LiPo from your supply.
- Use a supply rated for 1 A or more. Expect about 400 mA typical and 600 mA with the speaker loud.
- Stay at or below 5.5 V, the limit for both the Teensy's VIN and the charger chip.
- D1 drops about 0.35 V, and the display's charger chip wants at least 4.65 V. If the display resets when audio gets loud, also run the 5 V into the display's 4-pin JST **P1** header (pin 1 = VIN, pin 4 = GND), which bypasses D1.

### Data

Connect the board's **USB-C** to the Teensy's **micro-USB** with a plain USB-C ↔ micro-B data cable. You don't need an OTG adapter: host mode is set in firmware.

### Headphones (optional)

For stereo headphones, wire an I2S DAC module such as the common PCM5102A board to two of the display's headers, and build with `-DMICROM8_HEADPHONES=1` (uncomment it in `platformio.ini`).

| Display header | Signal | PCM5102A pin |
|---|---|---|
| P4 pin 1 (or P5 pin 1) | GND | GND |
| P4 pin 2 (or P5 pin 2) | 3.3 V | VIN |
| P4 pin 3 (or P5 pin 3) | IO17 | BCK |
| P4 pin 4 (or P5 pin 4) | IO18 | LCK |
| P3 pin 1 | IO6 | DIN |

P4 (JST 1.25 mm) and P5 (JST SH 1.0 mm) carry the same signals, so use whichever connector you have cables for.

On the PCM5102A module:
- **SCK** must be grounded. That makes the DAC derive its clock from BCK; the board's SCK jumper usually does this already.
- **Config pins 1–4** (FLT, DEMP, XSMT, FMT) must be set to **L, L, H, L**. If XSMT is low, the DAC stays muted.

The PCM5102A outputs line level: fine for high-impedance headphones, a bit strained on 16–32 Ω ones. Headphone volume starts at 4/10.

**Jack detect (optional):** if your jack has a detect switch, wire it to P3 pin 2 (IO7) and add `-DHP_DETECT_PIN=7` so plugging in switches outputs automatically. Common PCM5102A boards don't have one, so use the `SPK`/`HP` button instead. The input is pulled up internally. If the switch reads high rather than low with a plug inserted, also add `-DHP_DETECT_INSERTED_LEVEL=1`.

### Serial log

Once the firmware is running, the USB port is in host mode, so logs go to **UART0** on the P1 header (TX = GPIO43, RX = GPIO44) at 115200 baud. Use a 3.3 V USB-UART adapter there to watch them (`pio device monitor`).

## Building and flashing

You need [PlatformIO](https://platformio.org/). The project uses the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform (Arduino core 3.x on ESP-IDF 5.5), because the stock PlatformIO platform's ESP-IDF 4.4 USB host stack can't do isochronous transfers, which USB audio needs.

```bash
pio run -e jc4827w543
```

The first build rebuilds the Arduino core with the settings in `custom_sdkconfig` (a bigger USB control-transfer limit and INFO-level logging), which takes several minutes.

To flash, disconnect the Teensy and connect the board to your computer. Because the firmware takes over the USB port, auto-reset doesn't work after the first flash. Enter the bootloader manually: **hold BOOT, tap RST, release BOOT**. Then run:

```bash
pio run -e jc4827w543 -t upload
```

Tap RST after flashing, then plug in the Teensy.

### Tests

The M8 protocol parser and framebuffer renderer have host-side unit tests:

```bash
pio test -e native
```

## Architecture

| Module | Role |
|---|---|
| `src/usb/m8_usb.cpp` | USB host client. Finds the M8's CDC data interface and USB Audio streaming interface by parsing descriptors, sets DTR/RTS and line coding, keeps bulk IN, bulk OUT and isochronous IN transfers in flight. Replaces m8c's libusb backend. |
| `src/m8/slip.c`, `protocol.cpp` | SLIP framing and parsing of M8 commands: rectangle, character, oscilloscope and system info. Ported from m8c. |
| `src/m8/screen.cpp` | 320×240 RGB565 framebuffer in PSRAM, drawn with m8c's semantics: font offsets, background-colour tracking, scope clearing. Uses m8c's five bitmap fonts. Tracks changed regions as 8-row bands. |
| `src/display/ui.cpp` | Drives the NV3041A over QSPI using Arduino_GFX. Copies changed bands to the LCD at up to 60 Hz, and draws the keypad and status bar. |
| `src/input/gt911.cpp`, `keypad.cpp` | Multi-touch GT911 driver and key hit-testing. Produces the M8 key bitmask (`'C'` + mask). |
| `src/audio/audio_out.cpp` | Ring buffer → I2S → NS4168 speaker amp (mixed to mono), or → second I2S port → headphone DAC (stereo). Applies per-output volume, and compensates for clock drift by dropping or repeating single frames. |

FreeRTOS tasks: USB and audio run on core 0; display and touch run on core 1. Serial data passes from USB to the UI task through a stream buffer, so a slow LCD transfer never stalls USB.

Board pin assignments are in `src/config.h`. They come from the vendor's IO sheet and schematic (see the [profi-max](https://github.com/profi-max/JC4827W543_4.3inch_ESP32S3_board) and [OldNKrusty](https://github.com/OldNKrusty/JC4827W543-ESP32S3-Guide) references).

## Troubleshooting

**While no M8 is connected, the M8 area shows the last 12 log lines**, so you can debug without a USB-UART adapter. Errors are red and warnings amber.

**The display stays on "Waiting for M8" and the log shows nothing about a device.**
- Check that the cable carries data, not just power: plug the Teensy into a computer with the same cable and see whether m8c finds it.
- That same test confirms the Teensy is running M8 Headless.

**The `port:` log lines show `connected=0 lines=SE0` the whole time.** The ESP32 sees nothing on the data lines. The cable is probably charge-only (it powers the display but carries no data), or the Teensy isn't running. A working connection shows `connected=1 lines=J … speed=full`.

**The log shows `ENUM: Abort request of enumeration process (0x48a:0x16c0)`.** ESP-IDF rejected the device because no enumeration filter approved it: the Arduino core enables the filter hook, and then ESP-IDF rejects anything a filter hasn't approved. The firmware installs a filter that accepts all devices; if you see this message, that code is missing.

**The log shows `Configuration descriptor larger than control transfer max length`.** The USB stack's control-transfer limit is too small for the M8's descriptor. `platformio.ini` raises it through `custom_sdkconfig`. If you see this, check that setting is still there and do a clean build (`pio run -t clean`, then build again).

**The first build takes several minutes** because `custom_sdkconfig` makes pioarduino rebuild the Arduino core from ESP-IDF source. Later builds are fast. If the build fails while installing tools, run it again; pioarduino sometimes swaps tool versions on the first run.

## Status and known limitations

- **Not yet tested on hardware.** The firmware compiles cleanly and the protocol and renderer tests pass. USB enumeration, audio streaming and touch orientation still need checking on a real board with an M8.
- **Touch orientation:** if touches land in the wrong place, set `TOUCH_SWAP_XY`, `TOUCH_MIRROR_X` or `TOUCH_MIRROR_Y` in `build_flags`.
- **Model:02 resolution:** if a device reports Model:02 (480×320), only the top-left 320×240 is shown and the status bar says `(crop)`. Headless reports 320×240, so this shouldn't come up.
- **Not implemented:** m8c's keyjazz mode (computer keyboard as a note keyboard) and its screensaver.

## License

Portions are derived from m8c and the SLIP decoder (both MIT). See [LICENSE-THIRD-PARTY.md](LICENSE-THIRD-PARTY.md).
