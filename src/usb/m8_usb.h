// USB host driver for the M8 Headless (Teensy 4.1) on the Tab5's high-speed USB-A port.
//
// The M8 enumerates as a composite device: a CDC-ACM serial port carrying the SLIP display
// protocol and key input, USB Audio Class 2.0 streaming in both directions (44.1 kHz, 16-bit
// stereo), and USB MIDI. See docs/m8-headless-usb.md. This replaces m8c's libusb backend
// (m8_libusb.c / audio_libusb.c).
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <stddef.h>
#include <stdint.h>

namespace m8usb {

enum class State : uint8_t {
  kWaiting,     // no device attached
  kConnected,   // M8 found and serial + audio running
  kUnsupported, // something attached that is not an M8
  kError,       // M8 attached but setup failed
};

// Called from the USB task with raw PCM (S16LE stereo) from each isochronous packet.
using AudioSink = void (*)(const uint8_t *pcm, size_t bytes);

// Installs the USB host stack and starts its tasks. Serial bytes from the M8 are written to
// `rx` (single reader). `audio` may be null.
bool start(StreamBufferHandle_t rx, AudioSink audio);

State state();

// Increments every time an M8 finishes connecting; lets other tasks detect reconnects.
uint32_t connection_count();

bool audio_streaming();

// Cumulative isochronous audio counters, for diagnostics.
struct AudioStats {
  uint32_t urbs_ok, urbs_failed;
  uint32_t packets_ok, packets_empty, packets_failed;
  uint32_t bytes;
  int last_urb_status, last_packet_status; // usb_transfer_status_t
};
AudioStats audio_stats();

// Queues bytes for the M8's CDC OUT endpoint. Thread-safe. len <= 8.
bool send(const uint8_t *data, size_t len);

} // namespace m8usb
