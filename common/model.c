#include "model.h"
#include <math.h>
#include <string.h>
bool json_integer(const cJSON *v,int low,int high,int *out)
{
    if (!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<low||v->valuedouble>high||floor(v->valuedouble)!=v->valuedouble) return false;
    *out=(int)v->valuedouble; return true;
}
bool values_patch(light_values_t *values,const cJSON *patch)
{
    if (!cJSON_IsObject(patch)||!patch->child) return false;
    light_values_t next=*values;
    unsigned seen=0;
    for (const cJSON *v=patch->child;v;v=v->next) {
        unsigned bit;
        if (!strcmp(v->string,"on")) { bit=1; if (!cJSON_IsBool(v)) return false; next.on=cJSON_IsTrue(v); }
        else if (!strcmp(v->string,"level")) { bit=2; if (!json_integer(v,1,254,&next.level)) return false; }
        else if (!strcmp(v->string,"kelvin")) { bit=4; if (!json_integer(v,2000,6500,&next.kelvin)) return false; }
        else if (!strcmp(v->string,"hue")) { bit=8; if (!json_integer(v,0,360,&next.hue)) return false; }
        else if (!strcmp(v->string,"saturation")) { bit=16; if (!json_integer(v,0,100,&next.saturation)) return false; }
        else return false;
        if (seen&bit) return false;
        seen|=bit;
    }
    *values=next; return true;
}
light_values_t values_default(void) { return (light_values_t){false,127,2700,0,0}; }
startup_config_t startup_default(void)
{
    startup_config_t s={0};s.wake=s.power=values_default();s.wake.on=s.power.on=true;return s;
}
static bool startup_values_valid(light_values_t v)
{
    return v.on && v.level>=1 && v.level<=254 && v.kelvin>=2000 && v.kelvin<=6500
        && v.hue>=0 && v.hue<=360 && v.saturation>=0 && v.saturation<=100;
}
bool startup_valid(const startup_config_t *s)
{
    return s->wake_mode>=0 && s->wake_mode<=1 && s->power_mode>=0 && s->power_mode<=4
        && startup_values_valid(s->wake) && startup_values_valid(s->power);
}
bool startup_patch(startup_config_t *config,const cJSON *o)
{
    if(!cJSON_IsObject(o))return false;
    startup_config_t s=*config;unsigned seen=0;
    for(const cJSON *v=o->child;v;v=v->next) {
        unsigned bit;
        if(!strcmp(v->string,"id")) { bit=16;if(!valid_light_id(cJSON_GetStringValue(v)))return false; }
        else if(!strcmp(v->string,"wakeMode")) { bit=1;if(!json_integer(v,0,1,&s.wake_mode))return false; }
        else if(!strcmp(v->string,"powerMode")) { bit=2;if(!json_integer(v,0,4,&s.power_mode))return false; }
        else if(!strcmp(v->string,"wake")) { bit=4;if(!values_patch(&s.wake,v))return false; }
        else if(!strcmp(v->string,"power")) { bit=8;if(!values_patch(&s.power,v))return false; }
        else return false;
        if(seen&bit)return false;
        seen|=bit;
    }
    if(!(seen&15) || !startup_valid(&s))return false;
    *config=s;return true;
}
cJSON *startup_json(const startup_config_t *s)
{
    cJSON *o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"wakeMode",s->wake_mode);cJSON_AddNumberToObject(o,"powerMode",s->power_mode);
    cJSON_AddItemToObject(o,"wake",values_json(s->wake));cJSON_AddItemToObject(o,"power",values_json(s->power));return o;
}
bool wake_patch(light_values_t *v,const startup_config_t *s,const cJSON *patch)
{
    // Only plain ON recalls the device preset. Explicit W/colour/scenes/presets take precedence.
    bool plain=cJSON_IsObject(patch) && patch->child && !patch->child->next
        && !strcmp(patch->child->string,"on") && cJSON_IsTrue(patch->child);
    light_values_t next=plain && !v->on && s->wake_mode==1?s->wake:*v;
    if(!values_patch(&next,patch))return false;
    *v=next;return true;
}
cJSON *values_json(light_values_t v)
{
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"on",v.on); cJSON_AddNumberToObject(o,"level",v.level);
    cJSON_AddNumberToObject(o,"kelvin",v.kelvin); cJSON_AddNumberToObject(o,"hue",v.hue);
    cJSON_AddNumberToObject(o,"saturation",v.saturation); return o;
}
lamp_command_t values_command(light_values_t v)
{
    float p[4]={(v.level-1)/253.0f,(v.kelvin-2000)/4500.0f,v.hue/360.0f,v.saturation/100.0f};
    lamp_command_t command=lamp_command(v.on,p,true);
    // Integer percentages are already filtered at the physical controls.
    command.white=v.saturation==0;
    return command;
}
void values_rgb(light_values_t v,uint8_t rgb[3])
{
    if (v.saturation>0) {
        uint8_t h,s; lamp_native_hs((float)v.kelvin,v.hue/360.0f,v.saturation/100.0f,&h,&s);
        lamp_hsv_rgb(h/254.0f,s/254.0f,rgb); return;
    }
    lamp_xy_t xy=lamp_mix((float)v.kelvin,v.hue/360.0f,v.saturation/100.0f);
    lamp_xy_rgb(xy.x,xy.y,1,rgb);
}
bool valid_light_id(const char *id)
{
    if (!id||strlen(id)!=16) return false;
    for (int i=0;i<16;i++) if (!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f'))) return false;
    return true;
}
bool valid_name(const char *name)
{
    if (!name||!*name||strlen(name)>=NAME_LEN) return false;
    for (const unsigned char *p=(const unsigned char *)name;*p;p++) if (*p<32||*p==127) return false;
    return true;
}
bool valid_alias(const char *alias)
{
    if (!alias || !*alias || strlen(alias)>=NAME_LEN) return false;
    size_t n=strlen(alias);
    for (size_t i=0;i<n;i++) {
        char c=alias[i];
        if ((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9')) continue;
        if (c!='-' || i==0 || i==n-1) return false;
    }
    return true;
}
void screen_button(screen_gate_t *s,uint64_t now) { s->awake=!s->awake; s->touched_ms=now; }
void screen_timeout(screen_gate_t *s,uint64_t now,unsigned seconds)
{
    if (seconds&&s->awake&&now-s->touched_ms>=(uint64_t)seconds*1000) s->awake=false;
}
button_action_t button_hold(button_hold_t *b,bool down,uint64_t now,unsigned hold_ms)
{
    button_action_t action=BUTTON_NONE;
    if (down && !b->down) { b->since=now; b->long_sent=false; }
    if ((down || b->down) && !b->long_sent && now-b->since>=hold_ms) { b->long_sent=true; action=BUTTON_LONG; }
    if (!down && b->down && !b->long_sent) action=BUTTON_SHORT;
    b->down=down;
    return action;
}
