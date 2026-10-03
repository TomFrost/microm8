// Board pins and layout constants for the Guition JC4827W543 (capacitive "C" variant).
// Pin assignments are from the vendor IO distribution sheet and schematic (V1.1).
// Included via config.h; don't include directly.
#pragma once

#define BOARD_NAME "JC4827W543"

// ---------------------------------------------------------------------------
// LCD: NV3041A, 480x272, QSPI
// ---------------------------------------------------------------------------
#define LCD_CS 45
#define LCD_SCK 47
#define LCD_D0 21
#define LCD_D1 48
#define LCD_D2 40
#define LCD_D3 39
#define LCD_BL 1 // backlight via AO3402 MOSFET, PWM capable
#define LCD_WIDTH 480
#define LCD_HEIGHT 272

// ---------------------------------------------------------------------------
// Touch: GT911 capacitive, I2C (up to 5 simultaneous points)
// ---------------------------------------------------------------------------
#define TOUCH_SDA 8
#define TOUCH_SCL 4
#define TOUCH_INT 3
#define TOUCH_RST 38
#define TOUCH_I2C_HZ 400000

// Touch orientation fix-ups, in case a panel's GT911 config is rotated relative to the LCD.
#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 0
#endif
#ifndef TOUCH_MIRROR_X
#define TOUCH_MIRROR_X 0
#endif
#ifndef TOUCH_MIRROR_Y
#define TOUCH_MIRROR_Y 0
#endif

// ---------------------------------------------------------------------------
// Audio: NS4168 mono class-D amp on I2S
// ---------------------------------------------------------------------------
#define I2S_BCLK 42
#define I2S_LRCLK 2
#define I2S_DOUT 41

// ---------------------------------------------------------------------------
// Optional headphone output: external I2S DAC (e.g. PCM5102A module), enabled with
// -DMICROM8_HEADPHONES=1. Wired through two headers:
//
//   P4 (JST 1.25) or P5 (JST SH 1.0) -- same signals on both
//     pin 1  GND   -> DAC GND
//     pin 2  3.3V  -> DAC VIN
//     pin 3  IO17  -> DAC BCK
//     pin 4  IO18  -> DAC LCK
//   P3 (JST 1.25)
//     pin 1  IO6   -> DAC DIN
//     pin 2  IO7   <- jack-detect switch (optional, see below)
//
// On the PCM5102A, tie SCK to GND (it then derives its clock from BCK).
// ---------------------------------------------------------------------------
#ifndef MICROM8_HEADPHONES
#define MICROM8_HEADPHONES 0
#endif
#define HP_BCLK 17
#define HP_LRCLK 18
#define HP_DOUT 6

// Jack-detect input for automatic speaker/headphone switching. Many DAC modules (including
// the common PCM5102A boards) don't expose one; leave it disabled and use the on-screen
// SPK/HP toggle instead. Set to the GPIO, and the level it reads with a plug inserted.
#ifndef HP_DETECT_PIN
#define HP_DETECT_PIN -1 // e.g. 7 (P3 pin 2)
#endif
#ifndef HP_DETECT_INSERTED_LEVEL
#define HP_DETECT_INSERTED_LEVEL 0 // pulled up internally; a switch to GND reads 0 when inserted
#endif

// ---------------------------------------------------------------------------
// Layout (landscape, 480x272)
//
//   +--------------------------+-----------+
//   |                          |  key      |
//   |   M8 screen 320x240      |  panel    |
//   |                          |  160x272  |
//   +--------------------------+           |
//   |   status bar 320x32      |           |
//   +--------------------------+-----------+
// ---------------------------------------------------------------------------
#define PANEL_W (LCD_WIDTH - M8_SCREEN_W)
#define PANEL_H LCD_HEIGHT

#ifdef MICROM8_PANEL_LEFT
#define PANEL_X 0
#define M8_SCREEN_X PANEL_W
#else
#define PANEL_X M8_SCREEN_W
#define M8_SCREEN_X 0
#endif
#define M8_SCREEN_Y 0

#define STATUS_X M8_SCREEN_X
#define STATUS_Y M8_SCREEN_H
#define STATUS_W M8_SCREEN_W
#define STATUS_H (LCD_HEIGHT - M8_SCREEN_H)
