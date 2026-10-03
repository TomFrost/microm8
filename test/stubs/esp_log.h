// Host-side stand-in for ESP-IDF logging, used by the native unit tests.
#pragma once
#include <stdio.h>

#define ESP_LOG_DEBUG 4
#define ESP_LOGE(tag, ...) (fprintf(stderr, "E %s: ", tag), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#define ESP_LOGW ESP_LOGE
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOG_BUFFER_HEXDUMP(tag, buf, len, level) ((void)(tag), (void)(buf), (void)(len))
