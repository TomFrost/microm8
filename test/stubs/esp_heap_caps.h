// Host-side stand-in for ESP-IDF heap capabilities, used by the native unit tests.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_calloc(size_t n, size_t s, int c){(void)c;return calloc(n,s);}
