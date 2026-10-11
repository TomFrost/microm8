// USB passthrough: the Tab5's USB-C port presents the connected M8 to a computer, so m8c,
// m8.run or a DAW can use it through the Tab5 while the Tab5 keeps showing and playing it.
//
// While no M8 is connected, USB-C stays the ESP32-P4's USB Serial/JTAG port (flashing and
// logs). When an M8 connects on USB-A, the P4's full-speed USB controller takes over USB-C and
// enumerates with the M8's identity: its serial port (display protocol and keys), its audio
// output (44.1 kHz stereo, to the computer) and its USB MIDI. When the M8 goes, USB-C returns
// to Serial/JTAG.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace passthru {

// Starts the task that follows the M8's connection state. Call after audio::start(): routing
// the I2S pins (GPIO 26/27, also a USB PHY's pads) turns off the OTG pads this turns on.
bool start();

// From the M8's USB task: copies of what the M8 sends, forwarded to the computer.
void push_audio(const uint8_t *pcm, size_t bytes); // S16LE stereo
void push_serial(const uint8_t *data, size_t bytes);
void push_midi(const uint8_t *data, size_t bytes);

// Keys the computer is holding (the M8 key bitmask from its 'C' commands), to be combined with
// the touch keys. 0 when no computer is connected.
uint8_t remote_keys();

// True while a computer has configured the device.
bool mounted();

} // namespace passthru
