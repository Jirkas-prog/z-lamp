#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "model.h"
#define IR_KEYS 24
typedef struct { uint16_t us; bool high; } ir_pulse_t;
typedef enum { IR_INVALID,IR_FRAME,IR_REPEAT } ir_frame_t;
typedef enum { IR_STATIC,IR_FLASH,IR_STROBE,IR_FADE,IR_SMOOTH } ir_effect_t;
typedef struct { const char *name; uint32_t code,old_code; int hue; ir_effect_t effect; } ir_key_t;
extern const ir_key_t ir_keys[IR_KEYS];
// Canonical code: first received bit is bit 31, matching legacy 0x00F7xxxx tables.
ir_frame_t ir_decode(const ir_pulse_t *p,size_t count,uint32_t *code);
int ir_match(const uint32_t codes[IR_KEYS],uint32_t code);
#define IR_LEVELS 32
typedef struct {
    uint32_t version;
    bool custom;
    unsigned count,member_count;
    uint8_t percent[IR_LEVELS];
    char members[MAX_LIGHTS+1][ID_LEN];
} remote_config_t;
void remote_default(remote_config_t *config,unsigned count);
void ir_default_scale(uint8_t percent[IR_LEVELS],unsigned count);
bool remote_valid(const remote_config_t *config);
bool ir_key_patch(int key,light_values_t current,const remote_config_t *remote,cJSON *patch);
int ir_brightness_step(int level,int direction,const remote_config_t *remote);
