// JC4827W543 board bring-up. Nothing beyond what the display, touch and audio drivers do.
#include "board.h"

#include "config.h"

namespace board {

bool begin() { return true; }

const char *name() { return BOARD_NAME; }

} // namespace board
