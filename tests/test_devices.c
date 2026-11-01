// Production coordinator state, removal callbacks and actual Zigbee command construction.
#include <assert.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
typedef uint8_t esp_zb_ieee_addr_t[8];
typedef int nvs_handle_t;
typedef struct { unsigned state; } update_t;
typedef int esp_zb_zdp_status_t;
typedef struct { uint8_t src_endpoint,dst_endpoint; union { uint16_t addr_short; } dst_addr_u; } esp_zb_zcl_basic_cmd_t;
typedef struct { int addr_type; union { uint16_t short_addr; } u; } esp_zb_zcl_addr_t;
typedef struct { esp_zb_zcl_basic_cmd_t zcl_basic_cmd; int address_mode,on_off_cmd_id; } esp_zb_zcl_on_off_cmd_t;
typedef struct { esp_zb_zcl_basic_cmd_t zcl_basic_cmd; int address_mode,level,transition_time; } esp_zb_zcl_move_to_level_cmd_t;
typedef struct { esp_zb_zcl_basic_cmd_t zcl_basic_cmd; int address_mode,color_temperature,transition_time; } esp_zb_zcl_color_move_to_color_temperature_cmd_t;
typedef struct { esp_zb_zcl_basic_cmd_t zcl_basic_cmd; int address_mode,transition_time; uint8_t hue,saturation; } esp_zb_color_move_to_hue_saturation_cmd_t;
typedef struct { esp_zb_zcl_basic_cmd_t zcl_basic_cmd; int address_mode,color_x,color_y,transition_time; } esp_zb_zcl_color_move_to_color_cmd_t;
#define ESP_ZB_ZCL_ADDR_TYPE_SHORT 2
#define ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT 2
#define ESP_ZB_ZCL_CMD_ON_OFF_ON_ID 1
#define ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID 0
#define ESP_ZB_ZDP_STATUS_SUCCESS 0
#define ESP_ZB_ZDP_STATUS_TIMEOUT 1
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define LAMP_ENDPOINT 1
static int64_t esp_timer_get_time(void) { return 1000000; }
static int commits,hs_count,xy_count,ct_count,on_count,level_on_count;
static esp_zb_zcl_move_to_level_cmd_t sent_level_on;
static esp_zb_color_move_to_hue_saturation_cmd_t sent_hs;
static esp_zb_zcl_color_move_to_color_temperature_cmd_t sent_ct;
static unsigned char saved_ids[256];
static int nvs_set_blob(int handle,const char *key,const void *value,size_t size) {
    (void)handle;if(!strcmp(key,"lights_v2")){assert(size<=sizeof(saved_ids));memcpy(saved_ids,value,size);}return 0;
}
static int nvs_commit(int handle) { (void)handle;commits++;return 0; }
static void esp_zb_zcl_on_off_cmd_req(const esp_zb_zcl_on_off_cmd_t *c) { if(c->on_off_cmd_id==1)on_count++; }
static void esp_zb_zcl_level_move_to_level_with_onoff_cmd_req(const esp_zb_zcl_move_to_level_cmd_t *c) { sent_level_on=*c;level_on_count++; }
static void esp_zb_zcl_level_move_to_level_cmd_req(const esp_zb_zcl_move_to_level_cmd_t *c) { (void)c; }
static void esp_zb_zcl_color_move_to_color_temperature_cmd_req(const esp_zb_zcl_color_move_to_color_temperature_cmd_t *c) { sent_ct=*c;ct_count++; }
static void esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req(const esp_zb_color_move_to_hue_saturation_cmd_t *c) { sent_hs=*c;hs_count++; }
static void esp_zb_zcl_color_move_to_color_cmd_req(const esp_zb_zcl_color_move_to_color_cmd_t *c) { (void)c;xy_count++; }
#define ESP_ZB_ZCL_STATUS_SUCCESS 0
typedef struct { uint16_t id;struct { uint8_t type;uint16_t size;void *value; } data; } esp_zb_zcl_attribute_t;
typedef struct { esp_zb_zcl_basic_cmd_t zcl_basic_cmd;int address_mode;uint16_t clusterID,manuf_code;uint8_t manuf_specific,attr_number;uint16_t *attr_field; } esp_zb_zcl_read_attr_cmd_t;
typedef struct { esp_zb_zcl_basic_cmd_t zcl_basic_cmd;int address_mode;uint16_t clusterID,manuf_code;uint8_t manuf_specific,attr_number;esp_zb_zcl_attribute_t *attr_field; } esp_zb_zcl_write_attr_cmd_t;
typedef struct { uint16_t cluster;struct { uint8_t fc,tsn;uint16_t manuf_code; } header; } esp_zb_zcl_cmd_info_t;
typedef struct { int status;esp_zb_zcl_attribute_t attribute; } esp_zb_zcl_read_attr_resp_variable_t;
static unsigned power_writes,power_reads;static uint8_t tsn;
static uint8_t esp_zb_zcl_write_attr_cmd_req(esp_zb_zcl_write_attr_cmd_t *r) {
    assert(r->attr_number==1);if(r->manuf_specific)assert(r->manuf_code==0x100b && r->clusterID==0x300 && (r->attr_field->id==3 || r->attr_field->id==4));
    power_writes++;return ++tsn;
}
static uint8_t esp_zb_zcl_read_attr_cmd_req(esp_zb_zcl_read_attr_cmd_t *r) { assert(r->attr_number==1);power_reads++;return ++tsn; }
#include "devices-under-test.inc"
int main(void)
{
#ifdef _MSC_VER
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);_set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    bulb_t *b=&bulbs[0];b->identity.endpoint=11;b->identity.ieee[0]=1;id_string(b);b->token=42;
    b->addressed=true;b->address=0x1234;b->config.transition=17;
    b->wanted=(light_values_t){true,254,2000,120,100};b->dirty=7;
    send_values(b);assert(hs_count==1 && !xy_count && !ct_count && sent_hs.saturation==254 && sent_hs.hue==85);
    assert(sent_hs.transition_time==17 && sent_hs.zcl_basic_cmd.dst_endpoint==11 && sent_hs.zcl_basic_cmd.dst_addr_u.addr_short==0x1234);
    assert(on_count==0 && level_on_count==1 && sent_level_on.level==254 && sent_level_on.transition_time==0);
    b->wanted.level=3;b->dirty=3;send_values(b);assert(on_count==0 && sent_level_on.level==3 && sent_level_on.transition_time==0);
    b->wanted.kelvin=6500;b->dirty=4;send_values(b);assert(sent_hs.saturation==254 && sent_hs.hue==85);
    b->wanted.saturation=0;b->min_mired=160;b->max_mired=450;b->dirty=4;send_values(b);assert(ct_count==1 && sent_ct.color_temperature==160);
    b->wanted.saturation=100;b->caps_known=true;b->capabilities=8;b->dirty=4;send_values(b);assert(xy_count==1); // XY-only device.
    b->capabilities=31;b->config.xy=true;b->dirty=4;send_values(b);assert(xy_count==2); // Explicit compatibility mode.
    b->leave_ticket=7;b->leave_until=1000;leave_done(ESP_ZB_ZDP_STATUS_TIMEOUT,(void *)(uintptr_t)7);
    assert(b->identity.endpoint==11 && !b->leave_ticket && !strcmp(b->remove_error,"leave_timeout") && commits==0);
    b->leave_ticket=8;leave_done(ESP_ZB_ZDP_STATUS_SUCCESS,(void *)(uintptr_t)7);assert(b->identity.endpoint==11); // Late previous response.
    char id[ID_LEN];strcpy(id,b->id);leave_done(ESP_ZB_ZDP_STATUS_SUCCESS,(void *)(uintptr_t)8);
    assert(!b->identity.endpoint && commits==1 && !strcmp(removed[0],id));
    assert(!callback_bulb((void *)(uintptr_t)42));
    assert(((identity_t *)saved_ids)[0].endpoint==0); // Removal persists before next inventory.
    b->identity.endpoint=12;b->identity.ieee[0]=2;id_string(b);b->token=43;
    leave_done(ESP_ZB_ZDP_STATUS_SUCCESS,(void *)(uintptr_t)8);assert(b->identity.endpoint==12);
    forget(b);assert(!b->identity.endpoint && commits==2 && !callback_bulb((void *)(uintptr_t)43)); // Force and stale discovery response.
    cJSON *frame=cJSON_CreateObject(),*rows=cJSON_AddArrayToObject(frame,"lights");cJSON_AddBoolToObject(frame,"start",true);
    cJSON_AddStringToObject(frame,"op","scene");cJSON_AddNumberToObject(frame,"v",1);cJSON_AddNumberToObject(frame,"sid",2147483647);cJSON_AddNumberToObject(frame,"seq",2147483647);
    for(int i=0;i<MAX_LIGHTS;i++) {
        bulb_t *l=&bulbs[i];l->identity.endpoint=11;l->identity.ieee[0]=(uint8_t)(i+1);id_string(l);l->config.transition=3;
        cJSON *row=cJSON_CreateArray();cJSON_AddItemToArray(row,cJSON_CreateString(l->id));cJSON_AddItemToArray(row,cJSON_CreateBool(true));
        cJSON_AddItemToArray(row,cJSON_CreateNumber(254));cJSON_AddItemToArray(row,cJSON_CreateNumber(i*20));cJSON_AddItemToArray(row,cJSON_CreateNumber(30));cJSON_AddItemToArray(rows,row);
    }
    char json[1500];assert(cJSON_PrintPreallocated(frame,json,sizeof(json),false));assert(strlen(json)+12<1536);
    assert(!scene_frame(frame));assert(bulbs[3].wanted.hue==60 && bulbs[3].wanted.saturation==100 && bulbs[3].scene_override);
    send_values(&bulbs[3]);assert(sent_hs.saturation==254 && sent_hs.transition_time==30 && bulbs[3].config.transition==3);
    cJSON_ReplaceItemInArray(cJSON_GetArrayItem(rows,MAX_LIGHTS-1),3,cJSON_CreateNumber(360));
    bulbs[0].wanted.hue=99;assert(scene_frame(frame) && bulbs[0].wanted.hue==99); // Invalid last row cannot partially apply earlier rows.
    cJSON_ReplaceItemInArray(cJSON_GetArrayItem(rows,MAX_LIGHTS-1),3,cJSON_CreateNumber(300));
    bulbs[5].leave_ticket=12;assert(!strcmp(scene_frame(frame),"remove_pending"));cJSON_Delete(frame);
    b=&bulbs[0];b->startup=startup_default();b->caps_known=true;b->capabilities=31;b->on_at=1;b->addressed=true;strcpy(b->manufacturer,"Signify Netherlands B.V.");
    cJSON *config=cJSON_Parse("{\"powerMode\":4,\"power\":{\"on\":true,\"level\":42,\"kelvin\":2700,\"hue\":240,\"saturation\":100}}");
    int before=commits;assert(!startup_set(b,config));assert(commits==before+1 && b->power_status==1 && b->power_count==5 && power_writes==5);
    assert(b->power_attrs[0].value==42 && b->power_attrs[1].value==366 && b->power_attrs[2].hue && b->power_attrs[3].hue && b->power_attrs[4].value==1);
    assert(!strcmp(startup_set(b,config),"startup_busy"));
    for(unsigned i=0;i<b->power_count;i++) {
        power_attr_t *a=&b->power_attrs[i];esp_zb_zcl_cmd_info_t info={.cluster=a->cluster,.header={.tsn=a->write_tsn,.fc=a->hue?4:0,.manuf_code=a->hue?0x100b:0}};
        assert(power_response(b,&info,1)==a);info.header.tsn+=100;assert(!power_response(b,&info,1));
        power_read(b,a);info.header.tsn=a->read_tsn;assert(power_response(b,&info,2)==a);
        esp_zb_zcl_read_attr_resp_variable_t response={.attribute={.id=a->attr,.data={.size=a->size,.value=&a->value}}};
        power_verify(b,a,&response);assert(b->power_status==(i+1==b->power_count?2:1));
    }
    assert(commits==before+1 && power_reads==5); // Confirmation cannot write settings every poll.
    b->startup.power_mode=2;assert(!power_prepare(b,&b->startup));assert(b->power_attrs[0].value==255 && b->power_attrs[1].value==65535 && b->power_attrs[2].value==65535 && b->power_attrs[4].value==255);
    power_begin(b,false);power_attr_t *a=&b->power_attrs[0];uint8_t wrong=42;
    esp_zb_zcl_read_attr_resp_variable_t mismatch={.attribute={.id=a->attr,.data={.size=1,.value=&wrong}}};power_verify(b,a,&mismatch);
    assert(b->power_status==3 && !strcmp(b->power_error,"startup_readback_mismatch"));
    b->startup.power_mode=1;assert(!power_prepare(b,&b->startup) && b->power_count==1 && b->power_attrs[0].value==0);
    power_begin(b,false);power_verify(b,&b->power_attrs[0],NULL);assert(b->power_status==3);
    b->startup.power_mode=3;assert(!power_prepare(b,&b->startup) && b->power_attrs[4].value==1 && b->power_attrs[0].value==255);
    b->startup.power_mode=4;b->startup.power.saturation=100;strcpy(b->manufacturer,"Other");assert(!strcmp(power_prepare(b,&b->startup),"startup_color_unsupported"));
    b->startup=startup_default();b->startup.wake_mode=1;b->startup.wake.level=9;b->startup.wake.hue=300;b->startup.wake.saturation=100;
    light_values_t value=values_default();cJSON *on=cJSON_Parse("{\"on\":true}");assert(wake_patch(&value,&b->startup,on) && value.level==9 && value.hue==300);
    value.level=50;assert(wake_patch(&value,&b->startup,on) && value.level==50); // Repeated ON is idempotent.
    cJSON_Delete(on);on=cJSON_Parse("{\"on\":true,\"level\":70,\"saturation\":0}");value.on=false;assert(wake_patch(&value,&b->startup,on) && value.level==70 && value.saturation==0);
    cJSON_Delete(on);cJSON_Delete(config);
    config=cJSON_Parse("{\"wakeMode\":1,\"wakeMode\":0}");assert(!startup_patch(&b->startup,config));cJSON_Delete(config);
    config=cJSON_Parse("{\"powerMode\":4,\"power\":{\"level\":0}}");assert(!startup_patch(&b->startup,config));cJSON_Delete(config);
    config=startup_json(&b->startup);startup_config_t round=startup_default();assert(startup_patch(&round,config) && round.wake.level==9);cJSON_Delete(config);
    puts("Coordinator: native saturation, CT, transitions, removal/stale callbacks, 16-light frame capacity and atomic scene validation passed.");
    return 0;
}
