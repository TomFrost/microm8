// Board bring-up that doesn't belong to a specific subsystem: power rails, I/O expanders,
// vendor libraries. Each board provides an implementation.
#pragma once

namespace board {

// Called first in setup(), before the display, touch, audio and USB are started.
bool begin();

const char *name();

} // namespace board
