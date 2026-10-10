// microm8 configuration for the M5Stack Tab5 (ESP32-P4). Pins are from M5Stack's M5Unified
// board support.
#pragma once

#include <stdint.h>

#define BOARD_NAME "M5Stack Tab5"

// ---------------------------------------------------------------------------
// Display: 5" 720x1280 MIPI-DSI. Portrait or landscape, following the accelerometer unless
// rotation is locked. Set -DTAB5_ROTATE_180=1 if portrait comes up upside down.
//
// Portrait (720x1280)                    Landscape (1280x720)
//   M8 screen 720x540 (2.25x) at top       M8 screen 960x720 (3x), centred
//   status bar          548..612           left margin: icons, then a panel (vertical slider,
//   slider/info row     616..692             power details, swipe hint), then SHIFT
//   keypad (M8 layout)  ..1240             right margin: EDIT, OPTION, PLAY, SHIFT
//                                          arrows: swipes on the left half of the screen
// ---------------------------------------------------------------------------
#define LCD_WIDTH 720 // portrait
#define LCD_HEIGHT 1280

#define PORT_M8_X 0
#define PORT_M8_Y 0
#define PORT_M8_W 720
#define PORT_M8_H 540
#define PORT_STATUS_Y 548
#define PORT_STATUS_H 64
#define PORT_ROW_Y 616 // must end above the keypad's hit rects (OPTION/EDIT's start at y 704)
#define PORT_ROW_H 76

// Portrait keypad (see keypad_layout.cpp): the M8's own arrangement on a regular grid, with
// OPTION/EDIT shifted up and right, and SHIFT/PLAY down, by KEY_OFFSET.
#define KEY_SIZE 144
#define KEY_SPACING 24 // between neighbouring keys on the grid
#define KEY_OFFSET 22  // extra shift of the OPTION/EDIT and SHIFT/PLAY pairs
#define KEY_BOTTOM_MARGIN 40

#define LAND_M8_X 160
#define LAND_M8_Y 0
#define LAND_M8_W 960
#define LAND_M8_H 720
#define LAND_ROW_Y 300 // the panel in the left margin, between the icons and SHIFT
#define LAND_ROW_H 236

// Landscape keys, in the margins either side of the M8 screen: a column of EDIT, OPTION, PLAY
// and SHIFT up from the bottom-right corner, and SHIFT in the bottom-left corner.
#define LKEY_SIZE 136
#define LKEY_GAP 12
#define LKEY_MARGIN 12

// Landscape arrows are swipes: a finger moving this far from where it landed, mostly along one
// axis, holds that arrow until it lifts.
#define SWIPE_DISTANCE 32
#define SWIPE_MIN_HOLD_MS 50 // quick flicks still reach the M8 as a press

// Optionally, held arrows repeat at microm8's pace rather than the M8's: the M8 is sent a tap of
// ARROW_TAP_MS straight away, then after ARROW_REPEAT_DELAY_MS one every ARROW_REPEAT_MS (SHIFT +
// arrow passes through as held). Off (0) by default: the M8 knows when a held arrow should stop
// acting (e.g. SHIFT+DOWN into the song view doesn't carry on moving the cursor down), and
// re-tapping from outside defeats that.
#define ARROW_TAP_MS 50
#define ARROW_REPEAT_DELAY_MS 400
#define ARROW_REPEAT_MS 0

// Accelerometer (BMI270) orientation: which screen orientation each axis reading means. At
// rest the sensor reads +1 g pointing up, away from the ground (opposite to gravity). 0 = portrait, 1 = landscape, 2 = portrait upside down,
// 3 = landscape the other way round. The "orientation:" log line shows what was sensed.
#ifndef TAB5_GRAVITY_POS_X
#define TAB5_GRAVITY_POS_X 3
#endif
#ifndef TAB5_GRAVITY_NEG_X
#define TAB5_GRAVITY_NEG_X 1
#endif
#ifndef TAB5_GRAVITY_POS_Y
#define TAB5_GRAVITY_POS_Y 0
#endif
#ifndef TAB5_GRAVITY_NEG_Y
#define TAB5_GRAVITY_NEG_Y 2
#endif

// ---------------------------------------------------------------------------
// Audio: ES8388 codec (speaker amp + headphone jack) and ES7210 mic ADC share I2S0.
// ---------------------------------------------------------------------------
#define TAB5_I2S_MCLK 30
#define TAB5_I2S_BCLK 27
#define TAB5_I2S_WS 29
#define TAB5_I2S_DOUT 26 // to ES8388
#define TAB5_I2S_DIN 28  // from ES7210 (mics)

#define TAB5_ES8388_ADDR 0x10

// PI4IOE5V6408 I/O expanders on the internal I2C bus.
#define TAB5_IOE0_ADDR 0x43 // HP_DET (bit 7, input), CAM_RST (bit 6), SPK_EN (bit 1)
#define TAB5_IOE0_CAM_RST (1 << 6)
#define TAB5_IOE1_ADDR 0x44 // CHG_STAT (bit 6, input), USB5V_EN (bit 3), WLAN_PWR_EN (bit 0)
#define TAB5_IOE1_WLAN_PWR_EN (1 << 0)

// Power diagnostics: measure the battery-current saving of each power cut at boot (adds about
// 2 s to boot), log a power summary every 10 s, and show the power draw in the status bar.
// Only meaningful on battery.
#ifndef MICROM8_POWER_DIAG
#define MICROM8_POWER_DIAG 0
#endif

// HP_DET reads 1 with headphones plugged in (confirmed on hardware). Plugging in switches to
// headphones and unplugging back to the speaker. -1 disables automatic switching.
#ifndef TAB5_HP_DET_INSERTED_LEVEL
#define TAB5_HP_DET_INSERTED_LEVEL 1
#endif

// ---------------------------------------------------------------------------
// The M8 Headless draws a 320x240 screen, scaled to the layout's M8 rectangle.
// ---------------------------------------------------------------------------
#define M8_SCREEN_W 320
#define M8_SCREEN_H 240

// Task layout. USB and audio run on core 0, display and touch on core 1.
#define USB_CORE 0
#define AUDIO_CORE 0
#define UI_CORE 1
