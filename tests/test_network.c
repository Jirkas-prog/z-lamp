// Runs the production Wi-Fi task with simulated time and driver events, without a radio.
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "model.h"
#include "infrared.h"
#include "scenes.h"
typedef int esp_err_t;
typedef int esp_event_base_t;
typedef int SemaphoreHandle_t;
typedef int EventGroupHandle_t;
typedef struct { char hostname[NAME_LEN]; } esp_netif_t;
static esp_netif_t ap_netif,sta_netif;
static char mdns_host[NAME_LEN],mdns_instance[NAME_LEN];
typedef struct { int unused; } wifi_init_config_t;
typedef struct { unsigned num; } wifi_sta_list_t;
typedef struct { struct { uint32_t ip; } ip_info; } ip_event_got_ip_t;
typedef union {
    struct { uint8_t ssid[32],password[64]; unsigned ssid_len,channel,max_connection,authmode; } ap;
    struct { uint8_t ssid[32],password[64]; struct { int authmode; } threshold; int scan_method; } sta;
} wifi_config_t;
#define ESP_OK 0
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
#define ESP_LOGI(...) ((void)0)
#define WIFI_INIT_CONFIG_DEFAULT() {0}
#define WIFI_EVENT 1
#define IP_EVENT 2
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_STOP 1
#define WIFI_EVENT_AP_STOP 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define WIFI_EVENT_AP_STACONNECTED 4
#define WIFI_EVENT_AP_STADISCONNECTED 5
#define IP_EVENT_STA_GOT_IP 6
#define WIFI_MODE_AP 1
#define WIFI_MODE_STA 2
#define WIFI_IF_AP 0
#define WIFI_IF_STA 1
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_ALL_CHANNEL_SCAN 1
#define WIFI_STORAGE_RAM 0
#define BIT0 1
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT_MAX
#define pdMS_TO_TICKS(n) (n)
#define IPSTR "%u"
#define IP2STR(p) (*(p))
static uint64_t clock_ms,finish;
static int scenario,mode,ap_starts,sta_starts,attempts[100],attempt_count;
static bool running,changed;
static char configured[33];
static jmp_buf done;
static void (*worker)(void *);
static void (*handler)(void *,esp_event_base_t,int32_t,void *);
static int xSemaphoreTake(int s,unsigned wait) { (void)s; (void)wait; return 1; }
static void xSemaphoreGive(int s) { (void)s; }
static int xEventGroupCreate(void) { return 1; }
static void xEventGroupSetBits(int e,int b) { (void)e; (void)b; }
static void xEventGroupClearBits(int e,int b) { (void)e; (void)b; }
static int xEventGroupWaitBits(int e,int b,int clear,int all,int wait) { (void)e;(void)b;(void)clear;(void)all;(void)wait;return 1; }
static int xTaskCreate(void (*task)(void *),const char *name,unsigned size,void *arg,int priority,void *handle)
{ (void)name;(void)size;(void)arg;(void)priority;(void)handle;worker=task;return 1; }
static int esp_netif_init(void) { return 0; }
static int esp_event_loop_create_default(void) { return 0; }
static esp_netif_t *esp_netif_create_default_wifi_ap(void) { return &ap_netif; }
static esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &sta_netif; }
static int esp_netif_set_hostname(esp_netif_t *interface,const char *name)
{ assert(!running);snprintf(interface->hostname,sizeof(interface->hostname),"%s",name);return 0; }
static int mdns_init(void) { return 0; }
static int mdns_hostname_set(const char *name) { snprintf(mdns_host,sizeof(mdns_host),"%s",name);return 0; }
static int mdns_instance_name_set(const char *name) { snprintf(mdns_instance,sizeof(mdns_instance),"%s",name);return 0; }
static int mdns_service_add(const char *instance,const char *type,const char *protocol,int port,const void *txt,int count)
{ assert(!instance && !strcmp(type,"_http") && !strcmp(protocol,"_tcp") && port==80 && !txt && !count);return 0; }
static int esp_wifi_init(const wifi_init_config_t *cfg) { (void)cfg; return 0; }
static int esp_wifi_set_storage(int storage) { (void)storage; return 0; }
static int esp_event_handler_register(int base,int id,void (*fn)(void *,esp_event_base_t,int32_t,void *),void *arg)
{ (void)base;(void)id;(void)arg;handler=fn;return 0; }
static int esp_wifi_ap_get_sta_list(wifi_sta_list_t *list) { list->num=0; return 0; }
static const char *esp_err_to_name(int err) { (void)err; return "simulated error"; }
static int esp_wifi_stop(void)
{ assert(running); running=false;handler(NULL,WIFI_EVENT,mode==WIFI_MODE_AP?WIFI_EVENT_AP_STOP:WIFI_EVENT_STA_STOP,NULL);return 0; }
static int esp_wifi_set_mode(int next) { assert(!running);mode=next;return 0; }
static int esp_wifi_set_config(int interface,const wifi_config_t *cfg)
{ if(interface==WIFI_IF_STA)snprintf(configured,sizeof(configured),"%.32s",(const char *)cfg->sta.ssid);return 0; }
static int esp_wifi_start(void);
static int esp_wifi_connect(void);
static void vTaskDelay(unsigned ticks);
#include "network-under-test.inc"
#include "preset-under-test.inc"
app_state_t app;
SemaphoreHandle_t app_mutex;
SemaphoreHandle_t command_mutex;
uint64_t app_now(void) { return clock_ms; }
void app_changed(void) { app.dirty_ms=clock_ms; }
static cJSON *sent_command;
static bool bridge_ok=true,cancel_during_bridge;
static bool arrival_storage_ok=true;
bool app_save_arrival(void) { return arrival_storage_ok; }
bool bridge_request(cJSON *command,char error[80])
{
    if(cancel_during_bridge)app.ir_generation++;
    cJSON_Delete(sent_command);sent_command=cJSON_Duplicate(command,true);
    snprintf(error,80,"simulated_timeout");return bridge_ok;
}
static int esp_wifi_start(void)
{
    assert(!running);running=true;
    if(mode==WIFI_MODE_AP) { assert(app.options.ap_enabled);ap_starts++; }
    else { assert(!app.options.ap_enabled);sta_starts++; }
    return 0;
}
static int esp_wifi_connect(void)
{
    assert(running && mode==WIFI_MODE_STA && attempt_count<100);
    int index=configured[0]-'A';attempts[attempt_count++]=index;
    if(scenario==3 && index==1) { ip_event_got_ip_t ip={{1234}};handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip); }
    else if(scenario!=6) handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    return 0;
}
static void vTaskDelay(unsigned ticks)
{
    clock_ms+=ticks;
    if(scenario==3 && clock_ms>=15000 && !changed) { changed=true;handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL); }
    if(scenario==4) {
        bool want=clock_ms>=3000 && clock_ms<8500;
        if(want!=app.options.ap_enabled) { app.options.ap_enabled=want;app.wifi_revision++; }
    }
    if(scenario==5 && clock_ms>=400 && !changed) { changed=true;app.options.network_count=0;app.wifi_revision++; }
    if(scenario==7 && clock_ms>=2000 && !changed) { changed=true;app.options.network_count=0;app.wifi_revision++; }
    if(scenario==9 && clock_ms>=2000 && !changed) { changed=true;strcpy(app.alias,"Loznice"); }
    if(clock_ms>=finish) longjmp(done,1);
}
static void run(int which,unsigned count,uint64_t duration)
{
    memset(&app,0,sizeof(app));events=0;started=false;accept_ip=false;failed=false;
    clock_ms=0;finish=duration;scenario=which;mode=0;ap_starts=sta_starts=attempt_count=0;running=changed=false;
    strcpy(app.config.ap_ssid,"Test");strcpy(app.config.ap_password,"test-pass-123");
    strcpy(app.alias,which==8?"Kuchyn-2":"Z-lamp");
    app.options.network_count=count;
    app.options.ap_enabled=which==7;
    strcpy(app.options.networks[0].ssid,"A");strcpy(app.options.networks[1].ssid,"B");
    strcpy(app.options.networks[0].password,"test-pass-123");strcpy(app.options.networks[1].password,"test-pass-123");
    wifi_start();
    assert(!strcmp(ap_netif.hostname,which==8?"kuchyn-2":"z-lamp"));
    assert(!strcmp(sta_netif.hostname,ap_netif.hostname) && !strcmp(mdns_host,ap_netif.hostname));
    assert(!strcmp(mdns_instance,app.alias) && !strcmp(app.runtime_alias,app.alias));
    if(!setjmp(done)) worker(NULL);
}
int main(void)
{
    run(1,0,300000);assert(ap_starts==0 && sta_starts==0); // Empty list must never start an AP.
    run(2,2,25000);assert(ap_starts==0 && attempt_count>=6);
    for(int i=0;i<attempt_count;i++)assert(attempts[i]==i%2); // Exhaustion restarts priority order forever.
    run(3,2,30000);assert(ap_starts==0 && attempt_count==4);
    assert(attempts[0]==0 && attempts[1]==1 && attempts[2]==0 && attempts[3]==1); // Stay connected, restart at first priority on loss.
    run(4,2,20000);assert(ap_starts==1 && sta_starts>=3 && !app.ap_active); // Manual AP on then off resumes STA.
    run(5,2,20000);assert(ap_starts==0 && attempt_count==1 && !running); // Editing list during attempt, including deleting all.
    run(6,2,35000);assert(ap_starts==0 && attempt_count==2); // Missing disconnect/DHCP event still advances after deadline.
    run(7,2,10000);assert(ap_starts==1 && sta_starts==0 && running); // Saving networks over AP must preserve its connection.
    run(8,0,5000);assert(ap_starts==0 && sta_starts==0); // Naming alone must never enable the radio/AP.
    run(9,0,5000);assert(!strcmp(app.alias,"Loznice") && !strcmp(app.runtime_alias,"Z-lamp") && !strcmp(mdns_host,"z-lamp")); // Saved alias waits for restart.
    memset(&app,0,sizeof(app));clock_ms=1000;app.c6_ready=true;app.c6_last_ms=999;
    app.lights[0].known=true;app.lights[0].reported=true;
    strcpy(app.lights[0].id,"001788010203abcd");strcpy(app.options.preset_target,"g1");
    app.config.groups[0].used=true;app.config.groups[0].count=2;
    strcpy(app.config.groups[0].members[0],app.lights[0].id);strcpy(app.config.groups[0].members[1],"display");
    app.options.preset=(light_values_t){true,64,2200,240,60};
    preset_toggle(); // A physical ON applies every preset value and wakes a display in the group.
    const cJSON *patch=cJSON_GetObjectItemCaseSensitive(sent_command,"patch");
    assert(cJSON_GetArraySize(patch)==5 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(patch,"on")));
    assert(app.lights[0].wanted.level==64 && app.lights[0].wanted.kelvin==2200 && app.lights[0].wanted.hue==240 && app.lights[0].wanted.saturation==60);
    assert(app.screen.awake && app.config.lamp_mode && app.config.lamp.on);
    preset_toggle(); // OFF changes only power, preserving both stored preset and wanted color.
    patch=cJSON_GetObjectItemCaseSensitive(sent_command,"patch");
    assert(cJSON_GetArraySize(patch)==1 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(patch,"on")));
    assert(!app.screen.awake && !app.lights[0].wanted.on && app.lights[0].wanted.level==64 && app.options.preset.on);
    cJSON *web_patch=cJSON_Parse("{\"on\":true}");
    assert(!app_set("display",web_patch) && !app.screen.awake);cJSON_Delete(web_patch);
    bridge_ok=false;preset_toggle();assert(app.error[0] && !app.screen.awake && !app.lights[0].wanted.on);
    app.config.groups[0].used=false;bridge_ok=true;preset_toggle();assert(!strcmp(app.error,"unknown_group"));
    app.ir.enabled=true; app.ir_learn_key=-1; app.dirty_ms=0;
    scenes_default(&app.scenes);remote_default(&app.remote,10);strcpy(app.ir.target,"display");scene_t *scene=&app.scenes.items[0];scene->mode=SCENE_CUSTOM;scene->level=64;
    scene->use_ir=false;scene->member_count=1;strcpy(scene->members[0].id,"display");scene->hold_ds=0;scene->fade_ds=20;
    assert(!app_start_scene(0,true,app.ir_revision));
    assert(app.ir_effect==IR_SMOOTH && app.screen.awake && !app.dirty_ms);
    app.screen.awake=false;
    clock_ms+=1000;scene_tick();
    assert(app.config.lamp.hue==280 && !app.screen.awake && !app.dirty_ms && app.config.lamp.saturation==100);
    assert(app_scene_remote(0,app.ir_revision));assert(app.playing.level==81 && app.scenes.items[0].level==64 && app.ir_effect!=IR_STATIC);
    scene_tick();assert(!app.screen.awake && !app.dirty_ms);
    cJSON *stop=cJSON_Parse("{\"on\":false}");assert(!app_set("display",stop));
    clock_ms+=1000;scene_tick();assert(!app.config.lamp.on); // A later tick cannot undo web OFF.
    assert(!app_start_scene(0,true,app.ir_revision));app.scenes.white_kelvin=2200;app.scenes.white_level=17;
    assert(app_scene_remote(7,app.ir_revision));assert(app.ir_effect==IR_STATIC && app.config.lamp.kelvin==2200 && app.config.lamp.level==17 && app.config.lamp.saturation==0);
    app.ir_learn_key=4;assert(!strcmp(app_set_ir("display",stop,app.ir_revision),"ir_inactive"));
    app.ir_learn_key=-1;assert(!strcmp(app_set_ir("display",stop,app.ir_revision+1),"ir_inactive"));
    strcpy(scene->members[0].id,app.lights[0].id);app.device_api=3;app.c6_last_ms=clock_ms;cancel_during_bridge=true;
    assert(!strcmp(app_start_scene(0,false,0),"scene_changed") && app.ir_effect==IR_STATIC);cancel_during_bridge=false;
    scene->member_count=2;strcpy(scene->members[1].id,"display");scene->members[1].offset=1;
    strcpy(app.ir.target,"remote");app.remote.member_count=1;strcpy(app.remote.members[0],"display");
    assert(!app_start_scene(0,true,app.ir_revision));
    assert(app.scene_count==1 && !strcmp(app.scene_members[0].id,"display") && app.scene_members[0].offset==1);
    assert(!app_start_scene(0,false,0) && app.scene_count==2); // Web scenes retain their explicit participants.
    unsigned scene_level=app.playing.level;
    assert(!app_scene_remote(0,app.ir_revision) && app.playing.level==scene_level); // IR cannot dim an excluded participant of a web scene.
    assert(!app_set_ir("remote",stop,app.ir_revision));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(sent_command,"lights"))==1); // Display-only command sent no new UART frame.
    app.remote.member_count=0;assert(!strcmp(app_start_scene(0,true,app.ir_revision),"empty_scene"));
    app.updating=true;assert(!strcmp(app_start_scene(0,false,0),"update_busy"));assert(!strcmp(app_set("display",stop),"update_busy"));app.updating=false;
    cJSON_Delete(stop);
    cJSON_Delete(sent_command);
    sent_command=NULL;
    memset(&app,0,sizeof(app));clock_ms=1000;app.c6_ready=true;app.c6_last_ms=999;
    strcpy(app.lights[0].id,"001788010203abcd");app.lights[0].known=true;
    strcpy(app.config.names[0].id,app.lights[0].id);strcpy(app.config.names[0].name,"Removed");
    strcpy(app.options.preset_target,app.lights[0].id);strcpy(app.ir.target,app.lights[0].id);app.ir.enabled=true;
    remote_default(&app.remote,10);app.remote.member_count=2;strcpy(app.remote.members[0],app.lights[0].id);strcpy(app.remote.members[1],"display");
    app.config.groups[0].used=true;app.config.groups[0].count=2;
    strcpy(app.config.groups[0].members[0],app.lights[0].id);strcpy(app.config.groups[0].members[1],"display");
    cJSON *remove=cJSON_Parse("{\"id\":\"001788010203abcd\",\"force\":true}");
    assert(!strcmp(app_device("remove",remove),"coordinator_update_required"));
    app.device_api=1;bridge_ok=false;assert(app_device("remove",remove));assert(app.lights[0].known);
    bridge_ok=true;assert(!app_device("remove",remove) && app.lights[0].removing);
    assert(app.config.groups[0].count==2); // ACK is not completion; inventory is authoritative.
    cJSON *inventory=cJSON_Parse("{\"v\":1,\"type\":\"hello\",\"ready\":true,\"deviceApi\":1,\"ids\":[]}");
    app_receive(inventory);
    assert(!app.lights[0].known && !app.config.names[0].id[0]);
    assert(app.config.groups[0].count==1 && !strcmp(app.config.groups[0].members[0],"display"));
    assert(!app.options.preset_target[0] && !app.ir.enabled && !app.ir.target[0]);
    assert(app.remote.member_count==1 && !strcmp(app.remote.members[0],"display"));
    assert(app.dirty_ms);
    cJSON_Delete(inventory);cJSON_Delete(remove);cJSON_Delete(sent_command);
    sent_command=NULL;
    memset(&app,0,sizeof(app));clock_ms=1000;app.c6_ready=true;app.c6_last_ms=999;app.device_api=4;
    light_t *night=&app.lights[0];strcpy(night->id,"001788010203abcd");night->known=night->online=night->reported=true;night->wanted=values_default();
    night->startup=startup_default();night->startup.wake_mode=1;night->startup.wake.level=254;night->startup.wake.saturation=100;
    assert(!app_arrival(night->id,3,300,2700));
    assert(night->wanted.on && night->wanted.level==3 && night->wanted.saturation==0 && night->wanted.kelvin==2700);
    assert(app.arrival[0].due==301000 && !strcmp(app.arrival[0].id,night->id));
    uint64_t deadline=app.arrival[0].due;
    assert(!app_arrival(night->id,254,300,2700));assert(night->wanted.level==3 && app.arrival[0].due==deadline); // Already on: leave it alone.
    clock_ms=300999;app.c6_last_ms=clock_ms;arrival_tick();assert(night->wanted.on);
    clock_ms=301001;app.c6_last_ms=clock_ms;arrival_tick();assert(!night->wanted.on && !app.arrival[0].id[0]);
    assert(!app_arrival(night->id,3,300,2700));
    cJSON *manual=cJSON_Parse("{\"level\":80}");assert(!app_set(night->id,manual));cJSON_Delete(manual);
    assert(!app.arrival[0].id[0]);clock_ms+=301000;app.c6_last_ms=clock_ms;arrival_tick();assert(night->wanted.on && night->wanted.level==80);
    night->wanted.on=false;night->on=false;
    arrival_storage_ok=false;assert(!strcmp(app_arrival(night->id,3,300,2700),"arrival_storage_failed"));assert(!night->wanted.on && !app.arrival[0].id[0]);arrival_storage_ok=true;
    assert(!app_arrival(night->id,3,300,2700));
    app.arrival[0].due=1;app.arrival[0].retry=0; // Restored durable timer expires after reboot, without the phone.
    bridge_ok=false;arrival_tick();assert(app.arrival[0].id[0] && night->wanted.on);
    clock_ms+=5001;app.c6_last_ms=clock_ms;bridge_ok=true;arrival_tick();assert(!app.arrival[0].id[0] && !night->wanted.on);
    assert(!strcmp(app_arrival("all",3,300,2700),"arrival_requires_bulb"));
    app.device_api=3;assert(!strcmp(app_arrival(night->id,3,300,2700),"coordinator_update_required"));
    app.device_api=4;clock_ms=1000000;app.c6_last_ms=clock_ms;
    light_t *second=&app.lights[1];*second=*night;strcpy(second->id,"001788010203abce");
    assert(!app_timer("all",3600,26));
    assert(night->wanted.on && second->wanted.on && night->wanted.level==26 && second->wanted.level==26);
    assert(app.arrival[0].explicit_timer && app.arrival[1].explicit_timer);
    deadline=app.arrival[0].due;assert(deadline==app.arrival[1].due);
    manual=cJSON_Parse("{\"level\":90}");assert(!app_set(night->id,manual));cJSON_Delete(manual);
    assert(app.arrival[0].due==deadline); // Manual brightness does not cancel an explicit timer.
    arrival_storage_ok=false;assert(app_timer("all",10,254));arrival_storage_ok=true;
    assert(app.arrival[0].due==deadline && night->wanted.level==90); // No ON before durable group save.
    assert(!app_timer(second->id,0,0));assert(!app.arrival[1].id[0]);
    clock_ms=deadline+1;app.c6_last_ms=clock_ms;bridge_ok=false;arrival_tick();assert(app.arrival[0].id[0]);
    clock_ms+=5001;app.c6_last_ms=clock_ms;bridge_ok=true;arrival_tick();
    assert(!night->wanted.on && second->wanted.on && !app.arrival[0].id[0]);
    assert(!app_timer(night->id,60,0));manual=cJSON_Parse("{\"on\":false}");assert(!app_set(night->id,manual));cJSON_Delete(manual);
    assert(!app.arrival[0].id[0]);
    assert(app_timer("all",-1,0) && app_timer("all",86401,0) && app_timer("all",0,26));
    assert(!strcmp(app_timer("display",60,0),"timer_requires_bulb"));
    night->on=night->wanted.on=false;
    assert(!app_timer(night->id,600,0));deadline=app.arrival[0].due;
    assert(!app_arrival(night->id,3,300,2700));assert(app.arrival[0].explicit_timer && app.arrival[0].due==deadline);
    app.ir_effect=(ir_effect_t)1;app.scene_count=1;strcpy(app.scene_members[0].id,night->id);
    clock_ms=deadline+1;app.c6_last_ms=clock_ms;arrival_tick();assert(app.ir_effect==IR_STATIC && !night->wanted.on);
    cJSON_Delete(sent_command);
    puts("Production Wi-Fi worker and preset: manual AP, priorities, retries, loss, edits, DHCP timeout, full preset ON, power-only OFF, physical display wake and failed commands passed.");
    return 0;
}
