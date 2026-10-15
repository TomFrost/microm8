// Screenshots over the USB-C serial console, for documentation: send 'S' and the Tab5 replies
// with its framebuffer. tools/screenshot.py does both ends and saves a PNG.
//
// The reply, between log lines (which are held back while it's sent):
//   MICROM8-SCREENSHOT 1 <width> <height> <rotation>
//   <base64 lines: the pixels, run-length encoded>
//   MICROM8-SCREENSHOT-END <encoded bytes> <crc32 hex>
// Pixels are the panel's native portrait framebuffer, RGB565 little-endian, row by row,
// encoded as (uint16 count, uint16 color) runs. `rotation` is how many quarter turns the UI
// is rotated on the panel, as M5GFX counts them.
#pragma once

namespace screenshot {

// Starts listening on the serial console.
void begin();

// UI task: true when a screenshot has been asked for; capture() then takes it.
bool requested();
void capture();

} // namespace screenshot
