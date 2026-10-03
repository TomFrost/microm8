// Settings shared by all boards. Board-specific pins and layout live in
// src/boards/<board>/board_config.h, selected by the PlatformIO environment.
#pragma once

#include <stdint.h>

#include "board_config.h"

// The M8 Headless draws a 320x240 screen. Boards may scale it up when presenting it.
#define M8_SCREEN_W 320
#define M8_SCREEN_H 240

// Task layout. USB and audio run on core 0, display and touch on core 1.
#define USB_CORE 0
#define AUDIO_CORE 0
#define UI_CORE 1
