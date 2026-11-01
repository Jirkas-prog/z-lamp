#pragma once
#include "model.h"
#include "infrared.h"
#include "scenes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

typedef struct { char id[ID_LEN],name[NAME_LEN]; } named_light_t;
typedef struct { bool used; char name[NAME_LEN]; unsigned count; char members[MAX_LIGHTS+1][ID_LEN]; } group_t;
// Reserved input fields retain the v2 NVS layout; no analog inputs are configured or read.
typedef struct {
    uint32_t version;
    char ap_ssid[33],ap_password[64],sta_ssid[33],sta_password[65];
    bool sta_enabled,boot_on,lamp_mode,reserved_input_enabled;
    unsigned timeout,status_brightness;
    light_values_t lamp;
    char reserved_input_target[ID_LEN];
    named_light_t names[MAX_LIGHTS];
    group_t groups[MAX_GROUPS];
} settings_t;
// Separate NVS key preserves the exact v2 settings layout already flashed on the board.
#define MAX_NETWORKS 5
typedef struct { char ssid[33],password[64]; } network_t;
typedef struct {
    uint32_t version;
    bool ap_enabled;
    unsigned network_count;
    network_t networks[MAX_NETWORKS];
    char preset_target[ID_LEN];
    light_values_t preset;
} options_t;
typedef struct {
    uint32_t version;
    bool enabled;
    char target[ID_LEN];
    uint32_t codes[IR_KEYS];
} ir_settings_t;
typedef struct {
    char id[ID_LEN]; bool known,online,reported,on,color_known,requested;
    int level,mode,mired,x,y;
    float age;
    light_values_t wanted;
    bool removing,legacy_xy;
    int transition,hue,saturation,capabilities,min_mired,max_mired;
    char remove_error[24],manufacturer[33],model[33];
    startup_config_t startup;
    int power_status;
    char power_error[40];
} light_t;
typedef struct { char id[ID_LEN]; uint64_t due,retry; bool explicit_timer; } arrival_timer_t;
typedef struct {
    arrival_timer_t arrival[MAX_LIGHTS];
    settings_t config;
    options_t options;
    ir_settings_t ir;
    scene_settings_t scenes;
    remote_config_t remote;
    scene_t playing;
    scene_member_t scene_members[MAX_LIGHTS+1];
    unsigned scene_count;
    int active_scene;
    uint64_t scene_started,scene_segment;
    bool scene_force;
    uint32_t ir_last_code,ir_generation,ir_revision;
    uint64_t ir_last_ms,ir_learn_until;
    int ir_learn_key; // -1 when not learning.
    ir_effect_t ir_effect;
    light_t lights[MAX_LIGHTS];
    screen_gate_t screen;
    unsigned page;
    uint32_t state_revision;
    uint64_t ble_help_until;
    bool c6_ready,sta_connected,ap_active,preset_pending;
    unsigned wifi_revision;
    int c6_boot,join_seconds,channel,ap_clients;
    int device_api,ota_api,c6_ota_state;
    bool c6_confirmed,updating;
    char c6_version[32],c6_firmware[65];
    uint64_t c6_last_ms,dirty_ms,restart_ms,status_until;
    char sta_ip[16],runtime_ssid[33],runtime_password[64];
    char sta_ssid[33],wifi_error[80];
    char alias[NAME_LEN],runtime_alias[NAME_LEN],hostname[NAME_LEN]; // Alias has its own NVS key.
    char display_name[NAME_LEN];
    char error[80];
} app_state_t;
extern app_state_t app;
extern SemaphoreHandle_t app_mutex;
extern SemaphoreHandle_t command_mutex;
static inline void app_lock(void) { xSemaphoreTake(app_mutex,portMAX_DELAY); }
static inline void app_unlock(void) { xSemaphoreGive(app_mutex); }
uint64_t app_now(void);
void app_changed(void); // Caller holds app_mutex.
const char *app_name(const char *id); // Caller holds app_mutex.
light_t *app_light(const char *id); // Caller holds app_mutex.
const char *app_set(const char *target,const cJSON *patch);
const char *app_arrival(const char *target,int level,int seconds,int kelvin);
const char *app_timer(const char *target,int seconds,int level); // level=0 keeps the current light state.
bool app_save_arrival(void); // Caller holds app_mutex; only IDs persist, reboot expires them.
const char *app_device(const char *op,const cJSON *params);
const char *app_set_ir(const char *target,const cJSON *patch,uint32_t revision);
const char *app_start_scene(int index,bool physical,uint32_t revision);
const char *app_stop_scene(void);
bool app_scene_remote(int key,uint32_t revision);
light_values_t app_target_values(const char *target);
void ir_start(void);
void app_toggle_ap(void);
void wifi_start(void);
void app_receive(const cJSON *message);
void bridge_start(void);
bool bridge_request(cJSON *command,char error[80]);
void display_start(void);
void web_start(void);
