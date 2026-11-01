#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "infrared.h"
#include "scenes.h"
#include "ble_wire.h"
typedef int SemaphoreHandle_t;
typedef int esp_err_t;
#define portMAX_DELAY 0
static int calls;
static bool start_ota_on_lock;
static int xSemaphoreTake(int s,int t);
static void xSemaphoreGive(int s) { (void)s; }
static const char *updates_url(const char *target,const char *url) { (void)target;(void)url;calls++;return NULL; }
static const char *updates_cancel(void) { calls++;return NULL; }
static const char *ble_configure(const cJSON *o) { (void)o;calls++;return NULL; }
#include "api-under-test.inc"
app_state_t app;
SemaphoreHandle_t app_mutex=1,command_mutex=2;
static int xSemaphoreTake(int s,int t) { (void)t;if(s==command_mutex && start_ota_on_lock) {start_ota_on_lock=false;app.updating=true;}return 1; }
uint64_t app_now(void) { return 1000; }
void app_changed(void) { app.state_revision++; }
light_t *app_light(const char *id) { for(int i=0;id && i<MAX_LIGHTS;i++)if(app.lights[i].known && !strcmp(id,app.lights[i].id))return &app.lights[i];return NULL; }
const char *app_name(const char *id) { (void)id;return "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"; }
const char *app_set(const char *target,const cJSON *patch) { assert(!strcmp(target,"display"));calls++;return values_patch(&app.config.lamp,patch)?NULL:"invalid_patch"; }
const char *app_arrival(const char *target,int level,int seconds,int kelvin) { assert(target && level==3 && seconds==300 && kelvin==2700);calls++;return NULL; }
const char *app_timer(const char *target,int seconds,int level) { assert(target && seconds==3600 && level==26);calls++;return NULL; }
const char *app_device(const char *op,const cJSON *params) { (void)op;(void)params;calls++;return NULL; }
const char *app_start_scene(int index,bool physical,uint32_t revision) { (void)index;(void)physical;(void)revision;calls++;return NULL; }
const char *app_stop_scene(void) { calls++;return NULL; }
bool bridge_request(cJSON *command,char error[80]) { (void)command;(void)error;calls++;return true; }
static cJSON *query(const char *op,const char *args)
{
    const char *error=NULL;cJSON *input=cJSON_Parse(args),*out=app_query(op,input,&error);assert(!error && out);cJSON_Delete(input);
    char *text=cJSON_PrintUnformatted(out);assert(text && strlen(text)+70<BLE_MESSAGE_MAX);
    assert(!strstr(text,"SECRET"));free(text);return out;
}
static const char *command(const char *uri,const char *json)
{ cJSON *o=cJSON_Parse(json);assert(o);const char *error=app_api(uri,o);cJSON_Delete(o);return error; }
int main(void)
{
    strcpy(app.config.ap_password,"SECRET_AP");strcpy(app.options.networks[0].password,"SECRET_WIFI");
    app.c6_last_ms=999;app.c6_ready=true;app.config.lamp=values_default();scenes_default(&app.scenes);remote_default(&app.remote,32);
    app.remote.member_count=17;
    for(int i=0;i<MAX_LIGHTS;i++) {
        light_t *l=&app.lights[i];l->known=l->online=l->reported=true;
        snprintf(l->id,sizeof(l->id),"00178801%08x",i);l->wanted=values_default();l->startup=startup_default();
        memset(l->manufacturer,'M',32);memset(l->model,'P',32);memset(l->remove_error,'R',23);memset(l->power_error,'E',39);
        strcpy(app.remote.members[i],l->id);
    }
    strcpy(app.remote.members[16],"display");
    for(int i=0;i<MAX_GROUPS;i++) {
        group_t *g=&app.config.groups[i];g->used=true;g->count=17;memset(g->name,'G',32);
        for(unsigned j=0;j<17;j++)strcpy(g->members[j],app.remote.members[j]);
    }
    int total=0,next=0;
    do {
        char args[40];snprintf(args,sizeof(args),"{\"offset\":%d}",next);cJSON *out=query("list_lights",args);
        int n=cJSON_GetArraySize(cJSON_GetObjectItem(out,"items"));assert(n<=4 && n>0);total+=n;
        cJSON *p=cJSON_GetObjectItem(out,"next");next=cJSON_IsNull(p)?-1:p->valueint;cJSON_Delete(out);
    }while(next>=0);
    assert(total==17);
    const char *ops[]={"get_light","get_light","get_group","get_remote","get_network_status","list_groups","list_scenes"};
    const char *args[]={"{\"id\":\"0017880100000000\"}","{\"id\":\"display\"}","{\"id\":\"g8\"}","{}","{}","{\"offset\":4}","{}"};
    for(unsigned i=0;i<sizeof(ops)/sizeof(ops[0]);i++)cJSON_Delete(query(ops[i],args[i]));
    scene_t *s=&app.scenes.items[0];*s=scene_default();s->used=true;s->use_ir=false;s->member_count=17;s->color_count=8;
    for(int i=0;i<17;i++) {strcpy(s->members[i].id,app.remote.members[i]);s->members[i].offset=7;}
    for(int i=0;i<8;i++)s->hues[i]=359;
    memset(s->name,'S',32);cJSON_Delete(query("get_scene","{\"id\":\"s1\"}"));
    cJSON *bad=cJSON_Parse("{\"offset\":0,\"password\":true}");const char *error=NULL;
    assert(!app_query("get_network_status",bad,&error) && error);cJSON_Delete(bad);
    assert(!command("/api/control","{\"target\":\"display\",\"patch\":{\"level\":254}}"));assert(calls==1 && app.config.lamp.level==254);
    assert(command("/api/control","{\"target\":\"display\",\"target\":\"all\",\"patch\":{\"on\":true}}"));assert(calls==1);
    app.updating=true;
    const char *mutations[]={"control","group","rename","remote","scenes","scene/stop","pair","ble"};
    for(unsigned i=0;i<sizeof(mutations)/sizeof(mutations[0]);i++) {
        char path[48];snprintf(path,sizeof(path),"/api/%s",mutations[i]);assert(!strcmp(command(path,"{}"),"update_busy"));
    }
    assert(calls==1);
    app.updating=false;start_ota_on_lock=true;
    assert(!strcmp(command("/api/rename","{\"id\":\"display\",\"name\":\"Changed\"}"),"update_busy"));assert(!app.display_name[0]);
    app.updating=false;int before=calls;
    assert(!command("/api/arrival","{\"target\":\"0017880100000000\",\"level\":3,\"offSeconds\":300,\"kelvin\":2700}"));assert(calls==before+1);
    assert(command("/api/arrival","{\"target\":\"all\",\"level\":0,\"offSeconds\":300,\"kelvin\":2700}"));
    assert(command("/api/arrival","{\"target\":\"all\",\"level\":3,\"offSeconds\":300,\"offSeconds\":0,\"kelvin\":2700}"));
    assert(command("/api/arrival","{\"target\":\"all\",\"level\":3,\"offSeconds\":3601,\"kelvin\":2700}"));assert(calls==before+1);
    assert(!command("/api/timer","{\"target\":\"all\",\"seconds\":3600,\"level\":26}"));
    assert(command("/api/timer","{\"target\":\"all\",\"seconds\":86401}"));
    assert(command("/api/timer","{\"target\":\"all\",\"seconds\":1.5}"));
    assert(command("/api/timer","{\"target\":\"all\",\"seconds\":30,\"seconds\":40}"));
    for(int i=0;i<MAX_LIGHTS;i++) {snprintf(app.arrival[i].id,ID_LEN,"%016x",i+1);app.arrival[i].due=86401000;app.arrival[i].explicit_timer=true;}
    cJSON *timers=query("get_timers","{}");assert(cJSON_GetArraySize(cJSON_GetObjectItem(timers,"items"))==MAX_LIGHTS);cJSON_Delete(timers);
    puts("Shared API: paged inventory, maximum entities within BLE envelope, no credentials, shared mutations and OTA lock race verified.");
    return 0;
}
