#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WIRE_CAP 1536
typedef struct { char data[WIRE_CAP]; size_t used; bool dropping; } wire_rx_t;
uint32_t wire_crc(const void *data, size_t size);
size_t wire_encode(const char *json, char *out, size_t capacity);
// On true, rx->data contains a CRC-verified, NUL-terminated JSON document.
bool wire_feed(wire_rx_t *rx, uint8_t byte);
