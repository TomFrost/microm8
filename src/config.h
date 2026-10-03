// microm8 configuration for the M5Stack Tab5 (ESP32-P4). Pins are from M5Stack's M5Unified
// board support.
#pragma once

#include <stdint.h>

#define BOARD_NAME "M5Stack Tab5"

// ---------------------------------------------------------------------------
// Display: 5" 720x1280 MIPI-DSI, used in portrait. Set -DTAB5_ROTATE_180=1 to flip it.
//
//   +----------------------+   y
//   |  +----------------+  |   40
//   |  |  M8 screen 2x  |  |
//   |  |    640x480     |  |
//   |  +----------------+  |   520
//   |  status bar          |   544 .. 608
//   |                      |
//   |          OPT  EDIT   |   708
//   |      ^               |
//   |  <   v   >           |
//   |                      |
//   |     SHIFT PLAY       |   .. 1232
//   +----------------------+   1280
// ---------------------------------------------------------------------------
#define LCD_WIDTH 720
#define LCD_HEIGHT 1280

#define M8_SCALE 2
#define M8_DRAW_X 40
#define M8_DRAW_Y 40

#define STATUS_X 0
#define STATUS_Y 544
#define STATUS_W LCD_WIDTH
#define STATUS_H 64

// Keypad (see keypad_layout.cpp): the M8's own arrangement on a regular grid, with OPTION/EDIT
// shifted up and right, and SHIFT/PLAY down, by KEY_OFFSET. Proportions follow the M8.
#define KEY_SIZE 144
#define KEY_SPACING 24 // between neighbouring keys on the grid
#define KEY_OFFSET 22  // extra shift of the OPTION/EDIT and SHIFT/PLAY pairs
#define KEY_BOTTOM_MARGIN 48

// ---------------------------------------------------------------------------
// Audio: ES8388 codec (speaker amp + headphone jack) and ES7210 mic ADC share I2S0.
// ---------------------------------------------------------------------------
#define TAB5_I2S_MCLK 30
#define TAB5_I2S_BCLK 27
#define TAB5_I2S_WS 29
#define TAB5_I2S_DOUT 26 // to ES8388
#define TAB5_I2S_DIN 28  // from ES7210 (mics)

#define TAB5_ES8388_ADDR 0x10
#define TAB5_IOE0_ADDR 0x43 // PI4IOE5V6408: HP_DET (bit 7, input), SPK_EN (bit 1)

// HP_DET's polarity isn't documented. Until it's confirmed on hardware, jack changes are only
// logged. Once known, set -DTAB5_HP_DET_INSERTED_LEVEL=0 or 1 to switch outputs automatically.
#ifndef TAB5_HP_DET_INSERTED_LEVEL
#define TAB5_HP_DET_INSERTED_LEVEL -1
#endif

// ---------------------------------------------------------------------------
// The M8 Headless draws a 320x240 screen; it's shown at M8_SCALE.
// ---------------------------------------------------------------------------
#define M8_SCREEN_W 320
#define M8_SCREEN_H 240

// Task layout. USB and audio run on core 0, display and touch on core 1.
#define USB_CORE 0
#define AUDIO_CORE 0
#define UI_CORE 1
