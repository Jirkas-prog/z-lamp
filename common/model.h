#pragma once
#include "cJSON.h"
#include "lamp_control.h"
#define MAX_LIGHTS 16
#define MAX_GROUPS 8
#define NAME_LEN 33
#define ID_LEN 17
typedef struct { bool on; int level,kelvin,hue,saturation; } light_values_t;
bool values_patch(light_values_t *values, const cJSON *patch);
cJSON *values_json(light_values_t values);
light_values_t values_default(void);
lamp_command_t values_command(light_values_t values);
void values_rgb(light_values_t values, uint8_t rgb[3]);
bool json_integer(const cJSON *value,int low,int high,int *out);
bool valid_light_id(const char *id);
bool valid_name(const char *name);
bool valid_alias(const char *alias);
// Power mode: untouched, off, recover on/off+colour, on with memory, on with preset.
typedef struct { int wake_mode,power_mode; light_values_t wake,power; } startup_config_t;
startup_config_t startup_default(void);
bool startup_valid(const startup_config_t *config);
bool startup_patch(startup_config_t *config,const cJSON *json);
cJSON *startup_json(const startup_config_t *config);
bool wake_patch(light_values_t *values,const startup_config_t *config,const cJSON *patch);
typedef struct { bool awake; uint64_t touched_ms; } screen_gate_t;
void screen_button(screen_gate_t *screen,uint64_t now);
void screen_timeout(screen_gate_t *screen,uint64_t now,unsigned seconds);
typedef struct { bool down,long_sent; uint64_t since; } button_hold_t;
typedef enum { BUTTON_NONE, BUTTON_SHORT, BUTTON_LONG } button_action_t;
// Input is already debounced. A long hold emits once and suppresses the short release.
button_action_t button_hold(button_hold_t *button,bool down,uint64_t now,unsigned hold_ms);
