#pragma once
#include "model.h"
#define MAX_SCENES 8
#define SCENE_COLORS 8
#define SCENE_MODES 13
#define SCENE_CUSTOM 12
typedef struct { char id[ID_LEN]; unsigned offset; } scene_member_t;
typedef struct {
    bool used,use_ir,stagger;
    char name[NAME_LEN];
    unsigned mode,level,hold_ds,fade_ds,color_count,member_count;
    unsigned hues[SCENE_COLORS];
    scene_member_t members[MAX_LIGHTS+1];
} scene_t;
typedef struct {
    uint32_t version;
    unsigned white_kelvin,white_level,brightness_steps;
    int buttons[4]; // -1 = unassigned, otherwise scene index.
    scene_t items[MAX_SCENES];
} scene_settings_t;
typedef struct { const char *name,*description; unsigned hold_ds,fade_ds; } scene_mode_t;
extern const scene_mode_t scene_modes[SCENE_MODES];
scene_t scene_default(void);
void scenes_default(scene_settings_t *settings);
bool scene_valid(const scene_t *scene);
bool scenes_valid(const scene_settings_t *settings);
// Every animated keyframe is fully saturated; brightness never adds white.
light_values_t scene_value(const scene_t *scene,uint64_t step,unsigned member,unsigned members,unsigned offset);
unsigned scene_period_ms(const scene_t *scene);
int scene_index(const char *id);
bool scene_parse(scene_t *out,const cJSON *json);
cJSON *scene_json(const scene_t *scene,int index);
