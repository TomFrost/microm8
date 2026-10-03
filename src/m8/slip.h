// SLIP decoder for the M8 serial protocol.
// Adapted from m8c (MIT, Jonne Kokkonen), originally by marcinbor85
// (https://github.com/marcinbor85/slip, MIT). See LICENSE-THIRD-PARTY.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SLIP_SPECIAL_BYTE_END 0xC0
#define SLIP_SPECIAL_BYTE_ESC 0xDB
#define SLIP_ESCAPED_BYTE_END 0xDC
#define SLIP_ESCAPED_BYTE_ESC 0xDD

typedef enum {
  SLIP_NO_ERROR = 0,
  SLIP_ERROR_BUFFER_OVERFLOW,
  SLIP_ERROR_UNKNOWN_ESCAPED_BYTE,
  SLIP_ERROR_INVALID_PACKET,
} slip_error_t;

typedef int (*slip_recv_message_fn)(const uint8_t *data, uint32_t size);

typedef struct {
  uint8_t *buf;
  uint32_t buf_size;
  uint32_t size;
  uint8_t escaped;
  slip_recv_message_fn recv_message;
} slip_handler_t;

#ifdef __cplusplus
extern "C" {
#endif

void slip_init(slip_handler_t *slip, uint8_t *buf, uint32_t buf_size, slip_recv_message_fn recv);
slip_error_t slip_read_byte(slip_handler_t *slip, uint8_t byte);

#ifdef __cplusplus
}
#endif
