#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_MESSAGE_MAX 1536
#define BLE_FRAGMENT_TIMEOUT_MS 5000
typedef struct {
    uint16_t total, received;
    uint64_t last_ms;
    char data[BLE_MESSAGE_MAX+1];
} ble_message_t;
// -1 rejects and resets the partial message; 0 needs more; 1 is complete.
int ble_message_feed(ble_message_t *message,const uint8_t *frame,size_t size,uint64_t now);
size_t ble_message_frame(const char *message,size_t total,size_t offset,unsigned mtu,uint8_t *out,size_t capacity);
