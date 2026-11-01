#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#define UPDATE_HEADER 144
#define UPDATE_CHUNK 512
typedef struct {
    uint8_t board,sha[32],elf[32];
    uint32_t size;
    char project[32],version[32];
} update_meta_t;
typedef struct {
    update_meta_t meta;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    mbedtls_sha256_context hash;
    uint32_t received;
    unsigned state; // 0 idle, 1 receiving, 2 verified, 3 selected for reboot.
} update_t;
bool update_header(const uint8_t bytes[UPDATE_HEADER],update_meta_t *meta);
void update_hex(const uint8_t *bytes,size_t size,char *out);
int update_unhex(const char *hex,uint8_t *out,size_t capacity);
const char *update_begin(update_t *update,const update_meta_t *meta,unsigned board);
const char *update_write(update_t *update,uint32_t offset,const uint8_t *bytes,size_t size);
const char *update_finish(update_t *update);
const char *update_commit(update_t *update);
void update_abort(update_t *update);
void update_health_tick(bool healthy);
bool update_confirmed(void);
