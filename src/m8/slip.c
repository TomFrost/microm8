// SLIP decoder for the M8 serial protocol.
// Adapted from m8c (MIT, Jonne Kokkonen), originally by marcinbor85
// (https://github.com/marcinbor85/slip, MIT). See LICENSE-THIRD-PARTY.md.

#include "slip.h"

static void reset_rx(slip_handler_t *slip) {
  slip->escaped = 0;
  slip->size = 0;
}

void slip_init(slip_handler_t *slip, uint8_t *buf, uint32_t buf_size, slip_recv_message_fn recv) {
  slip->buf = buf;
  slip->buf_size = buf_size;
  slip->recv_message = recv;
  reset_rx(slip);
}

static slip_error_t put_byte(slip_handler_t *slip, uint8_t byte) {
  if (slip->size >= slip->buf_size) {
    reset_rx(slip);
    return SLIP_ERROR_BUFFER_OVERFLOW;
  }
  slip->buf[slip->size++] = byte;
  slip->escaped = 0;
  return SLIP_NO_ERROR;
}

slip_error_t slip_read_byte(slip_handler_t *slip, uint8_t byte) {
  if (!slip->escaped) {
    switch (byte) {
    case SLIP_SPECIAL_BYTE_END: {
      slip_error_t err = SLIP_NO_ERROR;
      if (slip->size > 0 && !slip->recv_message(slip->buf, slip->size)) {
        err = SLIP_ERROR_INVALID_PACKET;
      }
      reset_rx(slip);
      return err;
    }
    case SLIP_SPECIAL_BYTE_ESC:
      slip->escaped = 1;
      return SLIP_NO_ERROR;
    default:
      return put_byte(slip, byte);
    }
  }

  switch (byte) {
  case SLIP_ESCAPED_BYTE_END:
    return put_byte(slip, SLIP_SPECIAL_BYTE_END);
  case SLIP_ESCAPED_BYTE_ESC:
    return put_byte(slip, SLIP_SPECIAL_BYTE_ESC);
  default:
    reset_rx(slip);
    return SLIP_ERROR_UNKNOWN_ESCAPED_BYTE;
  }
}
