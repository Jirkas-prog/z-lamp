#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "update.h"
#include "ble.h"

app_state_t app;
SemaphoreHandle_t app_mutex;
SemaphoreHandle_t command_mutex;
static nvs_handle_t nvs;
static const char *TAG="gateway";
uint64_t app_now(void) { return (uint64_t)esp_timer_get_time()/1000; }
void app_changed(void) { app.dirty_ms=app_now(); app.state_revision++; }
bool app_save_arrival(void)
{
    char ids[MAX_LIGHTS][ID_LEN]={0};
    for(int i=0;i<MAX_LIGHTS;i++)memcpy(ids[i],app.arrival[i].id,ID_LEN);
    return nvs_set_blob(nvs,"arrival_off",ids,sizeof(ids))==ESP_OK && nvs_commit(nvs)==ESP_OK;
}
light_t *app_light(const char *id)
{
    if (!id) return NULL;
    for (int i=0;i<MAX_LIGHTS;i++) if (app.lights[i].known && !strcmp(app.lights[i].id,id)) return &app.lights[i];
    return NULL;
}
const char *app_name(const char *id)
{
    if (!strcmp(id,"display")) return app.display_name[0]?app.display_name:"Displej";
    for (int i=0;i<MAX_LIGHTS;i++) if (!strcmp(app.config.names[i].id,id)) return app.config.names[i].name;
    return id;
}
static int number(const cJSON *o,const char *key,int fallback,int max)
{
    int n; return json_integer(cJSON_GetObjectItemCaseSensitive(o,key),0,max,&n)?n:fallback;
}
static void prune_devices(void)
{
    bool changed=false;
    for (int i=0;i<MAX_LIGHTS;i++) if (app.config.names[i].id[0] && !app_light(app.config.names[i].id)) {
        memset(&app.config.names[i],0,sizeof(named_light_t)); changed=true;
    }
    for (int i=0;i<MAX_GROUPS;i++) {
        group_t *g=&app.config.groups[i]; unsigned n=0;
        for (unsigned j=0;j<g->count;j++) {
            if (!strcmp(g->members[j],"display") || app_light(g->members[j])) {
                if (n!=j) memcpy(g->members[n],g->members[j],ID_LEN);
                n++;
            } else changed=true;
        }
        g->count=n;
    }
    for(int i=0;i<MAX_SCENES;i++) {
        scene_t *s=&app.scenes.items[i];unsigned n=0;
        for(unsigned j=0;j<s->member_count;j++) {
            if(!strcmp(s->members[j].id,"display") || app_light(s->members[j].id))s->members[n++]=s->members[j];
            else changed=true;
        }
        s->member_count=n;
    }
    unsigned keep=0;
    for(unsigned i=0;i<app.remote.member_count;i++) {
        if(!strcmp(app.remote.members[i],"display") || app_light(app.remote.members[i])) {
            if(keep!=i)memcpy(app.remote.members[keep],app.remote.members[i],ID_LEN);
            keep++;
        } else changed=true;
    }
    app.remote.member_count=keep;
    if (valid_light_id(app.options.preset_target) && !app_light(app.options.preset_target)) { app.options.preset_target[0]=0; changed=true; }
    if (valid_light_id(app.ir.target) && !app_light(app.ir.target)) {
        app.ir.target[0]=0; app.ir.enabled=false; app.ir_revision++; changed=true;
    }
    if (changed) { app.ir_effect=IR_STATIC; app.ir_generation++; app_changed(); }
}
void app_receive(const cJSON *o)
{
    const char *type=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"type"));
    if (number(o,"v",0,1)!=1 || !type) return;
    app_lock(); app.state_revision++;
    if (!strcmp(type,"hello")) {
        int boot=number(o,"boot",0,INT32_MAX);
        if (boot!=app.c6_boot) {
            for (int i=0;i<MAX_LIGHTS;i++) { app.lights[i].reported=false; app.lights[i].online=false; }
            app.c6_boot=boot;
        }
        app.c6_last_ms=app_now();
        app.c6_ready=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"ready"));
        app.device_api=number(o,"deviceApi",0,4);app.ota_api=number(o,"otaApi",0,1);app.c6_ota_state=number(o,"otaState",0,3);
        app.c6_confirmed=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"confirmed"));
        const char *version=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"version")),*firmware=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"firmwareId"));
        snprintf(app.c6_version,sizeof(app.c6_version),"%s",version?version:"");snprintf(app.c6_firmware,sizeof(app.c6_firmware),"%s",firmware?firmware:"");
        app.join_seconds=number(o,"join",0,180); app.channel=number(o,"channel",0,26);
        const cJSON *ids=cJSON_GetObjectItemCaseSensitive(o,"ids");
        if (cJSON_IsArray(ids) && cJSON_GetArraySize(ids)<=MAX_LIGHTS) {
            for (int i=0;i<MAX_LIGHTS;i++) app.lights[i].known=false;
            for (const cJSON *p=ids->child;p;p=p->next) {
                const char *id=cJSON_GetStringValue(p); if (!valid_light_id(id)) continue;
                light_t *light=NULL;
                for (int i=0;i<MAX_LIGHTS;i++) if (!strcmp(app.lights[i].id,id)) { light=&app.lights[i]; break; }
                if (!light) for (int i=0;i<MAX_LIGHTS;i++) if (!app.lights[i].id[0]) { light=&app.lights[i]; break; }
                if (!light) {
                    // Reuse a slot only if its old IEEE id is absent from this entire inventory.
                    for (int i=0;i<MAX_LIGHTS && !light;i++) {
                        bool present=false;
                        for (const cJSON *q=ids->child;q;q=q->next) if (cJSON_IsString(q) && !strcmp(q->valuestring,app.lights[i].id)) present=true;
                        if (!present) { light=&app.lights[i]; memset(light,0,sizeof(*light)); }
                    }
                }
                if (light) { snprintf(light->id,sizeof(light->id),"%s",id); light->known=true; }
            }
            if (app.c6_ready) prune_devices();
        }
    } else if (!strcmp(type,"light")) {
        light_t *l=app_light(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id")));
        if (l) {
            l->online=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"online"));
            l->reported=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"reported"));
            l->on=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"on"));
            l->requested=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"requested"));
            l->color_known=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"colorKnown"));
            l->level=number(o,"level",0,254); l->mode=number(o,"mode",0,2);
            l->mired=number(o,"mired",0,65535); l->x=number(o,"x",0,65535); l->y=number(o,"y",0,65535);
            l->hue=number(o,"hue",0,254); l->saturation=number(o,"saturation",0,254);
            l->transition=number(o,"transition",3,100); l->legacy_xy=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"legacyXy"));
            l->min_mired=number(o,"minMired",0,65535); l->max_mired=number(o,"maxMired",0,65535);
            l->capabilities=number(o,"capabilities",-1,65535);
            l->startup=startup_default();startup_patch(&l->startup,cJSON_GetObjectItemCaseSensitive(o,"startup"));
            l->power_status=number(o,"powerStatus",0,4);
            const char *power_error=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"powerError"));
            snprintf(l->power_error,sizeof(l->power_error),"%s",power_error?power_error:"");
            l->removing=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"removing"));
            const char *text=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"removeError"));
            snprintf(l->remove_error,sizeof(l->remove_error),"%s",text?text:"");
            text=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"manufacturer"));
            snprintf(l->manufacturer,sizeof(l->manufacturer),"%s",text?text:"");
            text=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"model"));
            snprintf(l->model,sizeof(l->model),"%s",text?text:"");
            cJSON *age=cJSON_GetObjectItemCaseSensitive(o,"age"); l->age=cJSON_IsNumber(age)?(float)age->valuedouble:-1;
            light_values_t v=values_default();
            if (values_patch(&v,cJSON_GetObjectItemCaseSensitive(o,"wanted"))) l->wanted=v;
        }
    }
    app_unlock();
}
static bool arrival_cancel(const char *id);
const char *app_device(const char *op,const cJSON *params)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY);
    app_lock();
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params,"id"));
    const char *failure=app.updating?"update_busy":!app_light(id)?"unknown_light":!app.c6_ready || app_now()-app.c6_last_ms>=6000?"coordinator_offline":!app.device_api?"coordinator_update_required":NULL;
    if(!failure && !strcmp(op,"startup") && app.device_api<3)failure="coordinator_update_required";
    if(!failure && !strcmp(op,"remove") && !arrival_cancel(id))failure="arrival_storage_failed";
    if (!failure) { app.ir_effect=IR_STATIC; app.ir_generation++; }
    app_unlock();
    if (!failure) {
        cJSON *request=cJSON_Duplicate(params,true); cJSON_AddStringToObject(request,"op",op);
        char message[80]; bool ok=bridge_request(request,message); cJSON_Delete(request);
        app_lock();
        snprintf(app.error,sizeof(app.error),"%s",ok?"":message);
        if (ok && !strcmp(op,"remove")) {
            light_t *l=app_light(id); if (l) { l->removing=true; l->remove_error[0]=0; }
        }
        if(ok && !strcmp(op,"startup")) {
            light_t *l=app_light(id);if(l) {
                startup_patch(&l->startup,params);
                if(cJSON_HasObjectItem(params,"powerMode") || cJSON_HasObjectItem(params,"power")) {
                    l->power_status=l->startup.power_mode?1:0;l->power_error[0]=0;
                }
            }
        }
        app_unlock(); if (!ok) failure="device_command_failed";
    }
    xSemaphoreGive(command_mutex); return failure;
}
static const char *targets_locked(const char *target,char ids[MAX_LIGHTS][ID_LEN],int *count,bool *display)
{
    if (!target) return "unknown_target";
    if (!strcmp(target,"display")) *display=true;
    else if(!strcmp(target,"remote")) {
        for(unsigned i=0;i<app.remote.member_count;i++) {
            if(!strcmp(app.remote.members[i],"display"))*display=true;
            else snprintf(ids[(*count)++],ID_LEN,"%s",app.remote.members[i]);
        }
    }
    else if (!strcmp(target,"all")) {
        for (int i=0;i<MAX_LIGHTS;i++) if (app.lights[i].known) snprintf(ids[(*count)++],ID_LEN,"%s",app.lights[i].id);
    } else if (target[0]=='g' && target[1]>='1' && target[1]<='8' && !target[2]) {
        // ponytail: up to 16 lights by unicast; use native Zigbee groups for tighter synchronization.
        group_t *g=&app.config.groups[target[1]-'1'];
        if (!g->used) { return "unknown_group"; }
        for (unsigned i=0;i<g->count;i++) {
            if (!strcmp(g->members[i],"display")) *display=true;
            else snprintf(ids[(*count)++],ID_LEN,"%s",g->members[i]);
        }
    } else if (app_light(target)) snprintf(ids[(*count)++],ID_LEN,"%s",target);
    else return "unknown_target";
    return !*count && !*display?"empty_group":NULL;
}
static bool ir_includes_locked(const char *id)
{
    char ids[MAX_LIGHTS][ID_LEN];int count=0;bool display=false;
    if(targets_locked(app.ir.target,ids,&count,&display))return false;
    if(!strcmp(id,"display"))return display;
    for(int i=0;i<count;i++)if(!strcmp(id,ids[i]))return true;
    return false;
}
// Commands and timer expiration share command_mutex; manual input always wins a later race.
static bool arrival_cancel(const char *id)
{
    for(int i=0;i<MAX_LIGHTS;i++)if(!strcmp(app.arrival[i].id,id)) {
        arrival_timer_t previous=app.arrival[i];memset(&app.arrival[i],0,sizeof(app.arrival[i]));
        if(!app_save_arrival()) { app.arrival[i]=previous;return false; }
        app.state_revision++;
    }
    return true;
}
static bool timer_manual_change(const char *id,bool off)
{
    for(int i=0;i<MAX_LIGHTS;i++)if(!strcmp(app.arrival[i].id,id) && app.arrival[i].explicit_timer && !off)return true;
    return arrival_cancel(id);
}
static const char *set_lights(char ids[MAX_LIGHTS][ID_LEN],int count,bool display,const cJSON *patch,bool physical,bool persist,bool automated)
{
    light_values_t check=values_default();
    if (!values_patch(&check,patch)) return "invalid_patch";
    app_lock();
    bool link=app.c6_ready && app.c6_last_ms && app_now()-app.c6_last_ms<6000,updating=app.updating;
    app_unlock();
    if(updating)return "update_busy";
    if (!count && !display) return "empty_group";
    if (!automated) {
        app_lock();bool saved=true;
        for(int i=0;i<count && saved;i++)saved=timer_manual_change(ids[i],cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(patch,"on")));
        app_unlock();if(!saved)return "arrival_storage_failed";
    }
    if (count) {
        if (!link) return "coordinator_offline";
        cJSON *request=cJSON_CreateObject(),*array=cJSON_AddArrayToObject(request,"targets");
        cJSON_AddStringToObject(request,"op","set");
        for (int i=0;i<count;i++) cJSON_AddItemToArray(array,cJSON_CreateString(ids[i]));
        cJSON_AddItemToObject(request,"patch",cJSON_Duplicate(patch,true));
        char error[80]; bool ok=bridge_request(request,error); cJSON_Delete(request);
        if (!ok) { app_lock(); snprintf(app.error,sizeof(app.error),"%s",error); app_unlock(); return "coordinator_rejected_or_timeout"; }
    }
    app_lock();
    for (int i=0;i<count;i++) { light_t *l=app_light(ids[i]); if (l) { wake_patch(&l->wanted,&l->startup,patch); l->requested=true; } }
    if (display) {
        values_patch(&app.config.lamp,patch);
        app.config.lamp_mode=true;
        // Explicit local controls (button or IR) may wake the display; effect ticks never do.
        if (physical) { app.screen.awake=app.config.lamp.on; app.screen.touched_ms=app_now(); app.status_until=0; }
        if (persist) app_changed();
    }
    app.error[0]=0;
    app.state_revision++;
    app_unlock();
    return NULL;
}
static const char *set_target(const char *target,const cJSON *patch,bool physical,bool persist)
{
    char ids[MAX_LIGHTS][ID_LEN];int count=0;bool display=false;
    app_lock();const char *failure=targets_locked(target,ids,&count,&display);app_unlock();
    return failure?failure:set_lights(ids,count,display,patch,physical,persist,false);
}
static void stop_effect(void)
{
    app_lock(); app.ir_effect=IR_STATIC; app.ir_generation++; app.state_revision++; app_unlock();
}
static const char *user_command(const char *target,const cJSON *patch,bool physical)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY);
    stop_effect();
    const char *failure=set_target(target,patch,physical,true);
    xSemaphoreGive(command_mutex); return failure;
}
const char *app_set(const char *target,const cJSON *patch) { return user_command(target,patch,false); }
const char *app_set_ir(const char *target,const cJSON *patch,uint32_t revision)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY);
    app_lock(); bool active=app.ir.enabled && app.ir_learn_key<0 && revision==app.ir_revision; app_unlock();
    const char *failure="ir_inactive";
    if (active) { stop_effect(); failure=set_target(target,patch,true,true); }
    xSemaphoreGive(command_mutex); return failure;
}
const char *app_arrival(const char *target,int level,int seconds,int kelvin)
{
    if(level<1 || level>254 || seconds<0 || seconds>3600 || (kelvin && (kelvin<2000 || kelvin>6500)))return "invalid_arrival";
    xSemaphoreTake(command_mutex,portMAX_DELAY);
    char ids[MAX_LIGHTS][ID_LEN];int count=0;bool display=false;
    app_lock();
    const char *failure=app.updating?"update_busy":app.device_api<4?"coordinator_update_required":targets_locked(target,ids,&count,&display);
    if(!failure && (display || (seconds && (count!=1 || !valid_light_id(target)))))failure="arrival_requires_bulb";
    bool scene=app.ir_effect!=IR_STATIC;
    app_unlock();
    // Don't interrupt a running scene or another person's light.
    if(!failure && !scene)for(int i=0;i<count;i++) {
        app_lock();light_t *l=app_light(ids[i]);
        bool off=l && l->online && (l->reported || l->requested) && !l->wanted.on && (!l->reported || !l->on);
        light_values_t v=l?(l->startup.wake_mode==1?l->startup.wake:l->wanted):values_default();
        bool explicit_timer=false;
        for(int j=0;j<MAX_LIGHTS;j++)if(!strcmp(app.arrival[j].id,ids[i]))explicit_timer=app.arrival[j].explicit_timer;
        if(off && !seconds && !timer_manual_change(ids[i],false))failure="arrival_storage_failed";
        if(off && seconds && !explicit_timer) {
            int slot=-1;
            for(int j=0;j<MAX_LIGHTS;j++)if(!strcmp(app.arrival[j].id,ids[i])) {slot=j;break;}
            if(slot<0)for(int j=0;j<MAX_LIGHTS;j++)if(!app.arrival[j].id[0]) {slot=j;break;}
            if(slot<0)failure="arrival_timer_full";
            else {
                arrival_timer_t previous=app.arrival[slot];
                snprintf(app.arrival[slot].id,ID_LEN,"%s",ids[i]);app.arrival[slot].due=app_now()+(uint64_t)seconds*1000;app.arrival[slot].retry=0;app.arrival[slot].explicit_timer=false;
                if(!app_save_arrival()) {app.arrival[slot]=previous;failure="arrival_storage_failed";}
            }
        }
        app_unlock();if(failure)break;if(!off)continue;
        v.on=true;v.level=level;if(kelvin) {v.kelvin=kelvin;v.saturation=0;}
        cJSON *patch=values_json(v);char one[MAX_LIGHTS][ID_LEN];snprintf(one[0],ID_LEN,"%s",ids[i]);
        failure=set_lights(one,1,false,patch,false,false,true);cJSON_Delete(patch);
        // Keep the deadline even after an ambiguous UART result: ON may have reached the bulb.
        if(failure)break;
    }
    xSemaphoreGive(command_mutex);return failure;
}
const char *app_timer(const char *target,int seconds,int level)
{
    if(seconds<0 || seconds>86400 || level<0 || level>254 || (!seconds && level))return "invalid_timer";
    xSemaphoreTake(command_mutex,portMAX_DELAY);
    char ids[MAX_LIGHTS][ID_LEN];int count=0;bool display=false;
    app_lock();
    const char *failure=app.updating?"update_busy":targets_locked(target,ids,&count,&display);
    // A mixed group applies to its Zigbee bulbs; LCD sleep has its own timer.
    if(!failure && !count)failure="timer_requires_bulb";
    if(!failure && level && app.device_api<4)failure="coordinator_update_required";
    arrival_timer_t previous[MAX_LIGHTS];memcpy(previous,app.arrival,sizeof(previous));
    uint64_t due=app_now()+(uint64_t)seconds*1000;
    for(int i=0;!failure && i<count;i++) {
        int slot=-1;
        for(int j=0;j<MAX_LIGHTS;j++)if(!strcmp(app.arrival[j].id,ids[i])) {slot=j;break;}
        if(slot<0 && !seconds)continue;
        if(slot<0)for(int j=0;j<MAX_LIGHTS;j++)if(!app.arrival[j].id[0]) {slot=j;break;}
        if(slot<0) {failure="arrival_timer_full";break;}
        memset(&app.arrival[slot],0,sizeof(app.arrival[slot]));
        if(seconds) {snprintf(app.arrival[slot].id,ID_LEN,"%s",ids[i]);app.arrival[slot].due=due;app.arrival[slot].explicit_timer=true;}
    }
    if(!failure && !app_save_arrival())failure="arrival_storage_failed";
    if(failure)memcpy(app.arrival,previous,sizeof(previous));else app.state_revision++;
    app_unlock();
    if(!failure && level) {
        stop_effect();
        cJSON *patch=cJSON_CreateObject();cJSON_AddBoolToObject(patch,"on",true);cJSON_AddNumberToObject(patch,"level",level);
        failure=set_lights(ids,count,false,patch,false,false,true);cJSON_Delete(patch);
        // Persisted deadline stays armed if UART acknowledgement is lost after ON.
    }
    xSemaphoreGive(command_mutex);return failure;
}
static void arrival_tick(void)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY);
    for(int i=0;i<MAX_LIGHTS;i++) {
        app_lock();arrival_timer_t *t=&app.arrival[i];uint64_t now=app_now();
        bool due=t->id[0] && now>=t->due && now>=t->retry && !app.updating;
        bool link=app.c6_ready && app.c6_last_ms && now-app.c6_last_ms<6000;
        char id[ID_LEN];snprintf(id,sizeof(id),"%s",t->id);
        if(due)t->retry=now+5000;
        bool known=app_light(id)!=NULL;app_unlock();
        if(!due || !link)continue;
        const char *failure=NULL;
        if(known) {
            app_lock();bool scene_member=false;
            if(app.ir_effect!=IR_STATIC)for(unsigned j=0;j<app.scene_count;j++)if(!strcmp(app.scene_members[j].id,id))scene_member=true;
            app_unlock();if(scene_member)stop_effect(); // An animation must not relight an expired bulb.
            char one[MAX_LIGHTS][ID_LEN];snprintf(one[0],ID_LEN,"%s",id);
            cJSON *off=cJSON_CreateObject();cJSON_AddBoolToObject(off,"on",false);
            failure=set_lights(one,1,false,off,false,false,true);cJSON_Delete(off);
        }
        app_lock();if(!failure && !arrival_cancel(id))failure="arrival_storage_failed";
        if(failure)snprintf(app.error,sizeof(app.error),"%s",failure);
        app_unlock();
    }
    xSemaphoreGive(command_mutex);
}
light_values_t app_target_values(const char *target)
{
    app_lock();
    light_values_t values=app.options.preset;
    char ids[MAX_LIGHTS][ID_LEN]; int count=0; bool display=false;
    if (!targets_locked(target,ids,&count,&display)) {
        light_t *l=count?app_light(ids[0]):NULL;
        if (l && (l->requested || l->reported)) values=l->wanted;
        else if (display) values=app.config.lamp;
    }
    app_unlock(); return values;
}
// All scene frame writes are serialized with web/IR/preset commands. No animation tick writes NVS.
static const char *scene_send(uint64_t step,unsigned transition,bool start,bool physical)
{
    cJSON *o=cJSON_CreateObject(),*array=cJSON_AddArrayToObject(o,"lights");
    cJSON_AddStringToObject(o,"op","scene");cJSON_AddBoolToObject(o,"start",start);
    light_values_t values[MAX_LIGHTS+1];
    app_lock();
    for(unsigned i=0;i<app.scene_count;i++) {
        scene_member_t *m=&app.scene_members[i];values[i]=scene_value(&app.playing,step,i,app.scene_count,m->offset);
        if(!strcmp(m->id,"display"))continue;
        if(!app_light(m->id)) { app_unlock();cJSON_Delete(o);return "scene_missing_device"; }
        if(!timer_manual_change(m->id,false)) { app_unlock();cJSON_Delete(o);return "arrival_storage_failed"; }
        cJSON *row=cJSON_CreateArray();cJSON_AddItemToArray(row,cJSON_CreateString(m->id));
        cJSON_AddItemToArray(row,cJSON_CreateBool(values[i].on));cJSON_AddItemToArray(row,cJSON_CreateNumber(values[i].level));
        cJSON_AddItemToArray(row,cJSON_CreateNumber(values[i].hue));cJSON_AddItemToArray(row,cJSON_CreateNumber(transition));cJSON_AddItemToArray(array,row);
    }
    bool bulbs=cJSON_GetArraySize(array)>0;
    bool link=app.c6_ready && app.c6_last_ms && app_now()-app.c6_last_ms<6000;
    int device_api=app.device_api;app_unlock();
    const char *failure=bulbs?(!link?"coordinator_offline":device_api<2?"coordinator_update_required":NULL):NULL;
    if(!failure && bulbs) {
        char error[80];if(!bridge_request(o,error))failure="coordinator_rejected_or_timeout";
    }
    cJSON_Delete(o);if(failure)return failure;
    app_lock();
    for(unsigned i=0;i<app.scene_count;i++) {
        const char *id=app.scene_members[i].id;
        if(!strcmp(id,"display")) {
            app.config.lamp=values[i];app.config.lamp_mode=true;
            if(start && physical) { app.screen.awake=true;app.screen.touched_ms=app_now();app.status_until=0; }
        } else { light_t *l=app_light(id);if(l) { l->wanted=values[i];l->requested=true; } }
    }
    app.state_revision++;app_unlock();return NULL;
}
const char *app_start_scene(int index,bool physical,uint32_t revision)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY);app_lock();
    const char *failure=app.updating?"update_busy":NULL;
    if(physical && (!app.ir.enabled || app.ir_learn_key>=0 || revision!=app.ir_revision))failure="ir_inactive";
    else if(index<0 || index>=MAX_SCENES || !app.scenes.items[index].used)failure="unknown_scene";
    if(!failure) {
        app.ir_effect=IR_STATIC;app.ir_generation++;
        app.playing=app.scenes.items[index];app.scene_count=0;
        if(app.playing.use_ir) {
            char ids[MAX_LIGHTS][ID_LEN];int count=0;bool display=false;
            failure=targets_locked(app.ir.target,ids,&count,&display);
            for(int i=0;!failure && i<count;i++)snprintf(app.scene_members[app.scene_count++].id,ID_LEN,"%s",ids[i]);
            if(!failure && display)snprintf(app.scene_members[app.scene_count++].id,ID_LEN,"display");
            for(unsigned i=0;i<app.scene_count;i++)app.scene_members[i].offset=app.playing.stagger?i:0;
        } else {
            app.scene_count=app.playing.member_count;
            memcpy(app.scene_members,app.playing.members,app.scene_count*sizeof(scene_member_t));
        }
        if(physical) {
            unsigned n=0;for(unsigned i=0;i<app.scene_count;i++)if(ir_includes_locked(app.scene_members[i].id))app.scene_members[n++]=app.scene_members[i];
            app.scene_count=n;
        }
        if(!app.scene_count)failure="empty_scene";
    }
    app_unlock();
    if(!failure) {
        stop_effect();app_lock();uint32_t generation=app.ir_generation;app_unlock();
        failure=scene_send(0,0,true,physical);
        app_lock();
        if(!failure && generation!=app.ir_generation)failure="scene_changed";
        if(!failure) {
            app.active_scene=index;app.ir_effect=IR_SMOOTH;app.scene_started=app_now();
            app.scene_segment=UINT64_MAX;app.scene_force=false;
            app.state_revision++;
        }
        app_unlock();
    }
    app_lock();snprintf(app.error,sizeof(app.error),"%s",failure?failure:"");app_unlock();
    xSemaphoreGive(command_mutex);return failure;
}
const char *app_stop_scene(void) {
    xSemaphoreTake(command_mutex,portMAX_DELAY);app_lock();bool busy=app.updating;app_unlock();
    if(!busy)stop_effect();
    xSemaphoreGive(command_mutex);return busy?"update_busy":NULL;
}
static void scene_tick(void)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY);app_lock();
    if(app.updating || app.ir_effect==IR_STATIC) { app_unlock();xSemaphoreGive(command_mutex);return; }
    uint64_t elapsed=app_now()-app.scene_started;unsigned period=scene_period_ms(&app.playing);
    uint64_t step=elapsed/period;unsigned within=(unsigned)(elapsed%period);
    bool fade=app.playing.fade_ds && within>=app.playing.hold_ds*100;
    uint64_t segment=step*2+(fade?1:0);bool send=segment!=app.scene_segment || app.scene_force;
    unsigned transition=fade?(period-within+99)/100:0;
    app.scene_force=false;app.scene_segment=segment;app_unlock();
    const char *failure=send?scene_send(step+(fade?1:0),transition,false,false):NULL;
    if(failure) { stop_effect();app_lock();snprintf(app.error,sizeof(app.error),"%s",failure);app_unlock(); }
    else {
        app_lock();
        for(unsigned i=0;i<app.scene_count;i++) if(!strcmp(app.scene_members[i].id,"display")) {
            light_values_t a=scene_value(&app.playing,step,i,app.scene_count,app.scene_members[i].offset);
            if(fade) {
                light_values_t b=scene_value(&app.playing,step+1,i,app.scene_count,app.scene_members[i].offset);
                float t=(float)(within-app.playing.hold_ds*100)/(app.playing.fade_ds*100);
                int delta=(b.hue-a.hue+540)%360-180;
                a.hue=(a.hue+(int)(delta*t)+360)%360;a.level+=(int)((b.level-a.level)*t);a.on=b.on;
            }
            app.config.lamp=a; // Interpolate LCD locally; keep a manually sleeping screen asleep.
        }
        app_unlock();
    }
    xSemaphoreGive(command_mutex);
}
bool app_scene_remote(int key,uint32_t revision)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY);app_lock();
    if(app.updating || app.ir_effect==IR_STATIC) { app_unlock();xSemaphoreGive(command_mutex);return false; }
    if(!app.ir.enabled || app.ir_learn_key>=0 || app.ir_revision!=revision) { app_unlock();xSemaphoreGive(command_mutex);return true; }
    for(unsigned i=0;i<app.scene_count;i++)if(!ir_includes_locked(app.scene_members[i].id)) {
        app_unlock();xSemaphoreGive(command_mutex);return false;
    }
    if(key<=1) {
        app.playing.level=(unsigned)ir_brightness_step((int)app.playing.level,key==0?1:-1,&app.remote);
        app.scene_force=true;app_unlock();xSemaphoreGive(command_mutex);return true;
    }
    char ids[MAX_LIGHTS][ID_LEN];int count=0;bool display=false;
    for(unsigned i=0;i<app.scene_count;i++) {
        const char *id=app.scene_members[i].id;if(!strcmp(id,"display"))display=true;else snprintf(ids[count++],ID_LEN,"%s",id);
    }
    light_values_t base=scene_value(&app.playing,0,0,app.scene_count,0);
    if(key==7) { base.kelvin=(int)app.scenes.white_kelvin;base.level=(int)app.scenes.white_level; }
    app_unlock();stop_effect();cJSON *patch=cJSON_CreateObject();const char *failure=NULL;
    if(ir_key_patch(key,base,&app.remote,patch))failure=set_lights(ids,count,display,patch,true,true,false);
    cJSON_Delete(patch);app_lock();snprintf(app.error,sizeof(app.error),"%s",failure?failure:"");app_unlock();
    xSemaphoreGive(command_mutex);return true;
}
static void preset_toggle(void)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY); stop_effect();
    app_lock();
    char target[ID_LEN],ids[MAX_LIGHTS][ID_LEN]; int count=0; bool display=false,on=false;
    snprintf(target,sizeof(target),"%s",app.options.preset_target);
    light_values_t preset=app.options.preset;
    const char *failure=targets_locked(target,ids,&count,&display);
    if (!failure) {
        on=display && app.screen.awake && app.config.lamp_mode && app.config.lamp.on;
        for (int i=0;i<count;i++) {
            light_t *l=app_light(ids[i]);
            if (l && (l->requested?l->wanted.on:(l->reported && l->on))) on=true;
        }
    }
    app_unlock();
    if (!failure) {
        preset.on=true;
        cJSON *patch=on?cJSON_CreateObject():values_json(preset);
        if (on) cJSON_AddBoolToObject(patch,"on",false);
        failure=set_target(target,patch,true,true); cJSON_Delete(patch);
    }
    app_lock();
    snprintf(app.error,sizeof(app.error),"%s",failure?failure:"");
    app_unlock();
    ESP_LOGI(TAG,"Preset button: %s",failure?failure:(on?"off":"on"));
    xSemaphoreGive(command_mutex);
}
void app_toggle_ap(void)
{
    app_lock();
    app.options.ap_enabled=!app.options.ap_enabled;
    app.wifi_revision++;
    // Save the physical switch before changing radio mode, including across power cycles.
    esp_err_t err=nvs_set_blob(nvs,"options",&app.options,sizeof(app.options));
    if (err==ESP_OK) err=nvs_commit(nvs);
    if (err!=ESP_OK) { snprintf(app.error,sizeof(app.error),"NVS: %s",esp_err_to_name(err)); app_changed(); }
    if (app.options.ap_enabled) { app.status_until=app_now()+30000; app.page=0; }
    ESP_LOGI(TAG,"Hotspot manually %s",app.options.ap_enabled?"enabled":"disabled");
    app_unlock();
}
static void maintenance(void *unused)
{
    for (;;) {
        app_lock();
        uint64_t now=app_now();
        if (app.dirty_ms && now-app.dirty_ms>=1000) {
            esp_err_t err=nvs_set_blob(nvs,"config",&app.config,sizeof(app.config));
            if (err==ESP_OK) err=nvs_set_blob(nvs,"options",&app.options,sizeof(app.options));
            if (err==ESP_OK) err=nvs_set_blob(nvs,"ir",&app.ir,sizeof(app.ir));
            if (err==ESP_OK) err=nvs_set_blob(nvs,"remote_v2",&app.remote,sizeof(app.remote));
            if (err==ESP_OK) err=nvs_set_blob(nvs,"scenes_v1",&app.scenes,sizeof(app.scenes));
            if (err==ESP_OK) err=nvs_set_str(nvs,"alias",app.alias);
            if (err==ESP_OK) err=nvs_set_str(nvs,"screen_name",app.display_name);
            if (err==ESP_OK) err=nvs_commit(nvs);
            if (err==ESP_OK) app.dirty_ms=0;
            else snprintf(app.error,sizeof(app.error),"NVS: %s",esp_err_to_name(err));
        }
        bool restart=app.restart_ms && now>=app.restart_ms && !app.dirty_ms;
        bool updating=app.updating;
        bool preset=app.preset_pending && !updating; app.preset_pending=false;
        app_unlock();
        update_health_tick(true);
        if (preset) preset_toggle();
        if(!updating) { arrival_tick();scene_tick(); }
        if (restart) esp_restart();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
void app_main(void)
{
    // Backlight is forced off before Wi-Fi or LCD initialization, including boot-dark mode.
    gpio_set_direction(BOARD_TFT_BACKLIGHT,GPIO_MODE_OUTPUT); gpio_set_level(BOARD_TFT_BACKLIGHT,0);
    app_mutex=xSemaphoreCreateMutex(); command_mutex=xSemaphoreCreateMutex(); assert(app_mutex && command_mutex);
    ESP_ERROR_CHECK(nvs_flash_init()); ESP_ERROR_CHECK(nvs_open("gateway",NVS_READWRITE,&nvs));
    size_t n=sizeof(app.config); esp_err_t err=nvs_get_blob(nvs,"config",&app.config,&n);
    if (err==ESP_ERR_NVS_NOT_FOUND) {
        app.config.version=2; app.config.timeout=120; app.config.status_brightness=40;
        app.config.lamp=values_default(); app.config.lamp.on=true;
        uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP));
        snprintf(app.config.ap_ssid,sizeof(app.config.ap_ssid),"HueLamp-%02X%02X%02X",mac[3],mac[4],mac[5]);
        static const char alphabet[]="ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
        for (int i=0;i<12;i++) app.config.ap_password[i]=alphabet[esp_random()%(sizeof(alphabet)-1)];
        ESP_ERROR_CHECK(nvs_set_blob(nvs,"config",&app.config,sizeof(app.config))); ESP_ERROR_CHECK(nvs_commit(nvs));
    } else {
        ESP_ERROR_CHECK(err);
        if (n!=sizeof(app.config) || app.config.version!=2) { ESP_LOGE(TAG,"Unsupported settings layout; preserve NVS and migrate before flashing"); abort(); }
    }
    n=sizeof(app.options); err=nvs_get_blob(nvs,"options",&app.options,&n);
    if (err==ESP_ERR_NVS_NOT_FOUND) {
        app.options.version=1; app.options.ap_enabled=false;
        app.options.preset=values_default(); app.options.preset.on=true;
        snprintf(app.options.preset_target,sizeof(app.options.preset_target),"all");
        if (app.config.sta_enabled && app.config.sta_ssid[0]) {
            app.options.network_count=1;
            snprintf(app.options.networks[0].ssid,33,"%s",app.config.sta_ssid);
            memcpy(app.options.networks[0].password,app.config.sta_password,63);
        }
        ESP_ERROR_CHECK(nvs_set_blob(nvs,"options",&app.options,sizeof(app.options)));
        ESP_ERROR_CHECK(nvs_commit(nvs));
    } else {
        ESP_ERROR_CHECK(err);
        if (n!=sizeof(app.options) || app.options.version!=1 || app.options.network_count>MAX_NETWORKS) {
            ESP_LOGE(TAG,"Unsupported network/preset settings; preserve NVS before migrating"); abort();
        }
    }
    n=sizeof(app.ir); err=nvs_get_blob(nvs,"ir",&app.ir,&n);
    if (err==ESP_ERR_NVS_NOT_FOUND) {
        app.ir.version=1; app.ir.enabled=true; snprintf(app.ir.target,sizeof(app.ir.target),"all");
        for (int i=0;i<IR_KEYS;i++) app.ir.codes[i]=ir_keys[i].code;
        ESP_ERROR_CHECK(nvs_set_blob(nvs,"ir",&app.ir,sizeof(app.ir))); ESP_ERROR_CHECK(nvs_commit(nvs));
    } else {
        ESP_ERROR_CHECK(err);
        if (n!=sizeof(app.ir) || app.ir.version!=1) { ESP_LOGE(TAG,"Unsupported IR settings; preserve NVS before migrating"); abort(); }
    }
    n=sizeof(app.alias); err=nvs_get_str(nvs,"alias",app.alias,&n);
    if (err==ESP_ERR_NVS_NOT_FOUND) {
        snprintf(app.alias,sizeof(app.alias),"Z-lamp");
        ESP_ERROR_CHECK(nvs_set_str(nvs,"alias",app.alias)); ESP_ERROR_CHECK(nvs_commit(nvs));
    } else ESP_ERROR_CHECK(err);
    if (!valid_alias(app.alias)) { ESP_LOGE(TAG,"Invalid stored alias; preserve NVS before repairing"); abort(); }
    n=sizeof(app.display_name); err=nvs_get_str(nvs,"screen_name",app.display_name,&n);
    if (err==ESP_ERR_NVS_NOT_FOUND) snprintf(app.display_name,sizeof(app.display_name),"Displej / lampička");
    else ESP_ERROR_CHECK(err);
    app.ir_learn_key=-1;
    n=sizeof(app.scenes);err=nvs_get_blob(nvs,"scenes_v1",&app.scenes,&n);
    if(err==ESP_ERR_NVS_NOT_FOUND) { scenes_default(&app.scenes);app_changed(); }
    else {
        ESP_ERROR_CHECK(err);
        if(n!=sizeof(app.scenes) || !scenes_valid(&app.scenes)) { ESP_LOGE(TAG,"Invalid scene settings; preserve NVS before migration");abort(); }
    }
    n=sizeof(app.remote);err=nvs_get_blob(nvs,"remote_v2",&app.remote,&n);
    if(err==ESP_ERR_NVS_NOT_FOUND) { remote_default(&app.remote,app.scenes.brightness_steps);app_changed(); }
    else { ESP_ERROR_CHECK(err);if(n!=sizeof(app.remote) || !remote_valid(&app.remote)) { ESP_LOGE(TAG,"Invalid remote settings; preserve NVS");abort(); } }
    char arrival_ids[MAX_LIGHTS][ID_LEN]={0};n=sizeof(arrival_ids);
    err=nvs_get_blob(nvs,"arrival_off",arrival_ids,&n);
    if(err==ESP_OK && n==sizeof(arrival_ids)) {
        for(int i=0;i<MAX_LIGHTS;i++)if(memchr(arrival_ids[i],0,ID_LEN) && valid_light_id(arrival_ids[i])) {
            memcpy(app.arrival[i].id,arrival_ids[i],ID_LEN);app.arrival[i].due=1;
        }
    } else if(err!=ESP_ERR_NVS_NOT_FOUND) { ESP_LOGE(TAG,"Invalid arrival timer storage");abort(); }
    app.active_scene=-1;
    app.screen=(screen_gate_t){app.config.boot_on,app_now()};
    bridge_start(); wifi_start(); display_start(); ir_start(); ble_start(); web_start();
    BaseType_t task=xTaskCreate(maintenance,"settings",10240,NULL,2,NULL);
    assert(task==pdPASS);
    ESP_LOGI(TAG,"Gateway ready; AP=%s (%s); UART1 TX=%d RX=%d; LCD boot=%s",
        app.runtime_ssid,app.options.ap_enabled?"on":"off",BOARD_UART_TX,BOARD_UART_RX,app.config.boot_on?"on":"off");
}
