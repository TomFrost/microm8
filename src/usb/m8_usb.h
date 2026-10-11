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

// Called from the USB task to fill `frames` stereo frames of audio for the M8's USB input.
using AudioSource = void (*)(int16_t *stereo, int frames);

// Called from the USB task with bytes the M8 sends on its serial port or its USB MIDI.
using DataSink = void (*)(const uint8_t *data, size_t bytes);

// Copies of the M8's serial (display protocol) and MIDI data, for USB passthrough. Call
// before start(); either may be null.
void set_taps(DataSink serial, DataSink midi);

// Installs the USB host stack and starts its tasks. Serial bytes from the M8 are written to
// `rx` (single reader). `audio` and `input` may be null.
bool start(StreamBufferHandle_t rx, AudioSink audio, AudioSource input);

// Streams audio from the input source to the M8's USB audio input while on (and an M8 is
// connected). Any task.
void set_input_enabled(bool on);

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

// Audio to the M8's input, for diagnostics. `feedback` is the M8's rate, 16.16 samples per
// microframe (5.5125 at 44.1 kHz).
struct InputStats {
  uint32_t packets_ok, packets_failed;
  uint32_t feedback, feedback_count;          // last accepted report, and how many
  uint32_t feedback_raw, feedback_bytes;      // the last report as received
  uint32_t feedback_empty, feedback_failed;   // reports that carried nothing, or failed
};
InputStats input_stats_snapshot();

// Queues bytes for the M8's CDC OUT endpoint. Thread-safe. len <= 8.
bool send(const uint8_t *data, size_t len);

// Queues USB-MIDI event packets for the M8's MIDI OUT endpoint. Thread-safe. len <= 64.
bool send_midi(const uint8_t *data, size_t len);

// The connected M8's USB identity, so passthrough can present the same device to a computer.
// Valid while state() is kConnected; strings are ASCII and may be empty.
struct Identity {
  uint16_t vid, pid, bcd_device;
  char manufacturer[32], product[32], serial[32];
};
Identity identity();

} // namespace m8usb
