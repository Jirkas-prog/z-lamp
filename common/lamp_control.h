#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { float x, y; } lamp_xy_t;
typedef struct {
    bool on;
    bool white;
    uint8_t level;
    uint16_t mired;
    uint16_t x, y;
} lamp_command_t;

typedef struct {
    bool stable, candidate;
    uint32_t since_ms;
} lamp_debounce_t;

float lamp_normalize(int raw, int low, int high);
bool lamp_debounce(lamp_debounce_t *state, bool value, uint32_t now_ms);
lamp_xy_t lamp_mix(float kelvin, float hue, float saturation);
lamp_command_t lamp_command(bool on, const float pots[4], bool was_white);
bool lamp_color_changed(lamp_command_t a, lamp_command_t b);
void lamp_xy_rgb(float x, float y, float brightness, uint8_t rgb[3]);
// Native Zigbee HS uses the bulb's own gamut; 254 is maximum saturation.
void lamp_native_hs(float kelvin, float hue, float saturation, uint8_t *h, uint8_t *s);
void lamp_hsv_rgb(float hue, float saturation, uint8_t rgb[3]);
