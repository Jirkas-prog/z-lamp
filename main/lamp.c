#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_private/uart_share_hw_ctrl.h"
#include "hal/uart_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs_flash.h"
#include "lamp_config.h"
#include "model.h"
#include "wire.h"
#include "update.h"
#include "esp_system.h"

#if !CONFIG_IDF_TARGET_ESP32C6 || !CONFIG_ZB_ZCZR
#error "Select ESP32-C6 and the Zigbee coordinator/router library"
#endif
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "Keep the XIAO console on native USB Serial/JTAG; UART1 is the inter-board link"
#endif
#define UART_LINK UART_NUM_1
static const char *TAG="coordinator";
typedef struct { esp_zb_ieee_addr_t ieee; uint8_t endpoint; } identity_t;
typedef struct { uint16_t transition; bool xy; } light_config_t;
typedef struct { uint16_t cluster,attr,value; uint8_t size,type,write_tsn,read_tsn,stage; bool hue; } power_attr_t;
typedef struct {
    identity_t identity;
    char id[ID_LEN];
    uint16_t address;
    bool addressed, matching, match_busy, resolving, requested;
    unsigned attempts, dirty, retry_dirty;
    uint64_t match_at, resolved_at, retry_at, on_at, level_at, color_at;
    light_values_t wanted;
    bool actual_on;
    uint8_t level, mode, hue, saturation;
    uint16_t x,y,mired;
    unsigned color_fields;
    light_config_t config;
    bool scene_override;
    uint16_t scene_transition;
    uint32_t token,leave_ticket;
    uint64_t leave_until,info_at;
    char remove_error[24],manufacturer[33],model[33];
    uint16_t capabilities,min_mired,max_mired;
    bool caps_known;
    startup_config_t startup;
    power_attr_t power_attrs[5];
    unsigned power_count,power_status;
    uint64_t power_until;
    char power_error[40];
} bulb_t;
// All bulb/network state is protected by the Zigbee lock (callbacks already hold it).
static bulb_t bulbs[MAX_LIGHTS];
static nvs_handle_t settings;
static bool ready;
static update_t firmware_update;
static int update_sid;
static uint64_t update_last,update_reboot;
static uint64_t join_until;
static int boot_id;
static uint32_t next_token;
static char removed[MAX_LIGHTS][ID_LEN];
static bulb_t *callback_bulb(void *ctx)
{
    uint32_t token=(uint32_t)(uintptr_t)ctx;
    for (int i=0;token && i<MAX_LIGHTS;i++) if (bulbs[i].token==token) return &bulbs[i];
    return NULL;
}
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time()/1000; }
static void id_string(bulb_t *b)
{
    for (int i=0;i<8;i++) snprintf(b->id+2*i,3,"%02x",b->identity.ieee[7-i]);
}
static bulb_t *find_id(const char *id)
{
    if (!id) return NULL;
    for (int i=0;i<MAX_LIGHTS;i++) if (bulbs[i].identity.endpoint && !strcmp(bulbs[i].id,id)) return &bulbs[i];
    return NULL;
}
static bulb_t *find_source(esp_zb_zcl_addr_t source,uint8_t endpoint)
{
    for (int i=0;i<MAX_LIGHTS;i++) if (bulbs[i].identity.endpoint==endpoint && bulbs[i].addressed
        && source.addr_type==ESP_ZB_ZCL_ADDR_TYPE_SHORT && bulbs[i].address==source.u.short_addr) return &bulbs[i];
    return NULL;
}
static void save_identities(void)
{
    identity_t data[MAX_LIGHTS]={0};
    for (int i=0;i<MAX_LIGHTS;i++) data[i]=bulbs[i].identity;
    ESP_ERROR_CHECK(nvs_set_blob(settings,"lights_v2",data,sizeof(data)));
    light_config_t configs[MAX_LIGHTS];
    for (int i=0;i<MAX_LIGHTS;i++) configs[i]=bulbs[i].config;
    ESP_ERROR_CHECK(nvs_set_blob(settings,"device_cfg1",configs,sizeof(configs)));
    startup_config_t starts[MAX_LIGHTS];
    for(int i=0;i<MAX_LIGHTS;i++)starts[i]=bulbs[i].startup;
    ESP_ERROR_CHECK(nvs_set_blob(settings,"startup_v1",starts,sizeof(starts)));
    ESP_ERROR_CHECK(nvs_set_blob(settings,"removed",removed,sizeof(removed)));
    ESP_ERROR_CHECK(nvs_commit(settings));
}
static void forget(bulb_t *b)
{
    memmove(removed+1,removed,sizeof(removed)-sizeof(removed[0]));
    snprintf(removed[0],ID_LEN,"%s",b->id);
    // Tokens invalidate outstanding discovery/resolve/leave callbacks before slot reuse.
    memset(b,0,sizeof(*b)); save_identities();
}
static void leave_done(esp_zb_zdp_status_t status,void *ctx)
{
    uint32_t ticket=(uint32_t)(uintptr_t)ctx;
    for (int i=0;i<MAX_LIGHTS;i++) {
        bulb_t *b=&bulbs[i]; if (!ticket || b->leave_ticket!=ticket) continue;
        b->leave_ticket=0; b->leave_until=0;
        if (status==ESP_ZB_ZDP_STATUS_SUCCESS) forget(b);
        else snprintf(b->remove_error,sizeof(b->remove_error),"%s",status==ESP_ZB_ZDP_STATUS_TIMEOUT?"leave_timeout":"leave_rejected");
        break;
    }
}
static esp_zb_zcl_basic_cmd_t destination(bulb_t *b)
{
    esp_zb_zcl_basic_cmd_t d={.src_endpoint=LAMP_ENDPOINT,.dst_endpoint=b->identity.endpoint};
    d.dst_addr_u.addr_short=b->address; return d;
}
static void send_values(bulb_t *b)
{
    lamp_command_t v=values_command(b->wanted);
    uint16_t transition=b->scene_override?b->scene_transition:b->config.transition;
    if ((b->dirty&1) && !v.on) {
        esp_zb_zcl_on_off_cmd_t c={.zcl_basic_cmd=destination(b),.address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .on_off_cmd_id=v.on?ESP_ZB_ZCL_CMD_ON_OFF_ON_ID:ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID};
        esp_zb_zcl_on_off_cmd_req(&c);
    }
    if (v.on && (b->dirty&3)) {
        esp_zb_zcl_move_to_level_cmd_t c={.zcl_basic_cmd=destination(b),.address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .level=v.level,.transition_time=transition};
        // One level+ON command avoids briefly recalling the previous (possibly 100%) level.
        if(b->dirty&1) { c.transition_time=0;esp_zb_zcl_level_move_to_level_with_onoff_cmd_req(&c); }
        else esp_zb_zcl_level_move_to_level_cmd_req(&c);
    }
    if (v.on && (b->dirty&4)) {
        if (v.white) {
            if (b->min_mired && v.mired<b->min_mired) v.mired=b->min_mired;
            if (b->max_mired && v.mired>b->max_mired) v.mired=b->max_mired;
            esp_zb_zcl_color_move_to_color_temperature_cmd_t c={.zcl_basic_cmd=destination(b),
                .address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,.color_temperature=v.mired,.transition_time=transition};
            esp_zb_zcl_color_move_to_color_temperature_cmd_req(&c);
        } else if (!b->config.xy && (!b->caps_known || (b->capabilities&1))) {
            esp_zb_color_move_to_hue_saturation_cmd_t c={.zcl_basic_cmd=destination(b),
                .address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,.transition_time=transition};
            lamp_native_hs(b->wanted.kelvin,b->wanted.hue/360.0f,b->wanted.saturation/100.0f,&c.hue,&c.saturation);
            esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req(&c);
        } else {
            esp_zb_zcl_color_move_to_color_cmd_t c={.zcl_basic_cmd=destination(b),.address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .color_x=v.x,.color_y=v.y,.transition_time=transition};
            esp_zb_zcl_color_move_to_color_cmd_req(&c);
        }
    }
    b->dirty=0;
}
static const char *scene_frame(const cJSON *o)
{
    const cJSON *rows=cJSON_GetObjectItemCaseSensitive(o,"lights"),*start=cJSON_GetObjectItemCaseSensitive(o,"start");
    int count=cJSON_GetArraySize(rows);
    if(!cJSON_IsArray(rows) || count<1 || count>MAX_LIGHTS || !cJSON_IsBool(start))return "invalid_scene_frame";
    bulb_t *selected[MAX_LIGHTS];light_values_t next[MAX_LIGHTS];int transition[MAX_LIGHTS];
    for(int i=0;i<count;i++) {
        const cJSON *row=cJSON_GetArrayItem(rows,i);
        if(!cJSON_IsArray(row) || cJSON_GetArraySize(row)!=5)return "invalid_scene_frame";
        selected[i]=find_id(cJSON_GetStringValue(cJSON_GetArrayItem(row,0)));
        if(!selected[i])return "unknown_light";
        if(selected[i]->leave_ticket)return "remove_pending";
        for(int j=0;j<i;j++)if(selected[i]==selected[j])return "duplicate_light";
        next[i]=selected[i]->wanted;
        const cJSON *on=cJSON_GetArrayItem(row,1);if(!cJSON_IsBool(on))return "invalid_scene_frame";
        next[i].on=cJSON_IsTrue(on);next[i].saturation=100;
        if(!json_integer(cJSON_GetArrayItem(row,2),1,254,&next[i].level)
            || !json_integer(cJSON_GetArrayItem(row,3),0,359,&next[i].hue)
            || !json_integer(cJSON_GetArrayItem(row,4),0,100,&transition[i]))return "invalid_scene_frame";
    }
    // Validate the whole frame before changing any bulb. Sixteen independent colours fit one UART frame.
    for(int i=0;i<count;i++) {
        bulb_t *b=selected[i];light_values_t old=b->wanted;unsigned dirty=0;
        if(cJSON_IsTrue(start) || next[i].on!=old.on)dirty|=next[i].on?7:1;
        if(next[i].level!=old.level)dirty|=2;
        if(next[i].hue!=old.hue || old.saturation!=100)dirty|=4;
        b->wanted=next[i];b->requested=true;b->scene_override=true;b->scene_transition=(uint16_t)transition[i];
        b->dirty|=dirty;b->retry_at=0;b->retry_dirty=0;
    }
    return NULL;
}
static bool hue_device(const bulb_t *b)
{
    return strstr(b->manufacturer,"Philips") || strstr(b->manufacturer,"Signify");
}
static void power_add(bulb_t *b,uint16_t cluster,uint16_t attr,uint16_t value,uint8_t type,bool hue)
{
    assert(b->power_count<5);
    b->power_attrs[b->power_count++]=(power_attr_t){.cluster=cluster,.attr=attr,.value=value,.size=type==0x21?2:1,.type=type,.hue=hue};
}
static const char *power_prepare(bulb_t *b,const startup_config_t *s)
{
    if(s->power_mode>1 && !b->caps_known)return "startup_info_missing";
    if(s->power_mode==4 && s->power.saturation && (!hue_device(b) || !(b->capabilities&8)))return "startup_color_unsupported";
    b->power_count=0;
    if(!s->power_mode)return NULL;
    if(s->power_mode>1) {
        bool preset=s->power_mode==4;lamp_command_t v=values_command(s->power);
        power_add(b,0x0008,0x4000,preset?(uint16_t)s->power.level:0xff,0x20,false);
        if(b->capabilities&16) {
            uint16_t ct=preset?(v.white?v.mired:366):0xffff;
            if(ct!=0xffff && b->min_mired && ct<b->min_mired)ct=b->min_mired;
            if(ct!=0xffff && b->max_mired && ct>b->max_mired)ct=b->max_mired;
            power_add(b,0x0300,0x4010,ct,0x21,false);
        }
        // Hue power-on colour is a manufacturer-specific XY pair, not ordinary CurrentX/Y.
        if(hue_device(b) && (b->capabilities&8)) {
            power_add(b,0x0300,3,preset && !v.white?v.x:0xffff,0x21,true);
            power_add(b,0x0300,4,preset && !v.white?v.y:0xffff,0x21,true);
        }
    }
    power_add(b,0x0006,0x4003,s->power_mode==1?0:s->power_mode==2?0xff:1,0x30,false);
    return NULL;
}
static void power_read(bulb_t *b,power_attr_t *a)
{
    esp_zb_zcl_read_attr_cmd_t r={.zcl_basic_cmd=destination(b),.address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID=a->cluster,.manuf_specific=a->hue,.manuf_code=a->hue?0x100b:0,.attr_number=1,.attr_field=&a->attr};
    a->stage=2;a->read_tsn=esp_zb_zcl_read_attr_cmd_req(&r);
}
static void power_begin(bulb_t *b,bool write)
{
    b->power_status=1;b->power_until=now_ms()+15000;b->power_error[0]=0;
    for(unsigned i=0;i<b->power_count;i++) {
        power_attr_t *a=&b->power_attrs[i];
        if(!write) { power_read(b,a);continue; }
        esp_zb_zcl_attribute_t attr={.id=a->attr,.data={.type=a->type,.size=a->size,.value=&a->value}};
        esp_zb_zcl_write_attr_cmd_t r={.zcl_basic_cmd=destination(b),.address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .clusterID=a->cluster,.manuf_specific=a->hue,.manuf_code=a->hue?0x100b:0,.attr_number=1,.attr_field=&attr};
        a->stage=1;a->write_tsn=esp_zb_zcl_write_attr_cmd_req(&r);
    }
}
static void power_fail(bulb_t *b,const char *error)
{
    b->power_status=3;b->power_until=0;snprintf(b->power_error,sizeof(b->power_error),"%s",error);
}
static power_attr_t *power_response(bulb_t *b,const esp_zb_zcl_cmd_info_t *info,unsigned stage)
{
    if(b->power_status!=1)return NULL;
    for(unsigned i=0;i<b->power_count;i++) {
        power_attr_t *a=&b->power_attrs[i];
        if(a->stage==stage && a->cluster==info->cluster && (stage==1?a->write_tsn:a->read_tsn)==info->header.tsn
            && a->hue==((info->header.fc&4)!=0) && (!a->hue || info->header.manuf_code==0x100b))return a;
    }
    return NULL;
}
static void power_verify(bulb_t *b,power_attr_t *a,const esp_zb_zcl_read_attr_resp_variable_t *v)
{
    if(!v || v->status!=ESP_ZB_ZCL_STATUS_SUCCESS) { power_fail(b,"startup_attribute_unsupported");return; }
    uint16_t actual=0;
    if(v->attribute.data.size!=a->size || !v->attribute.data.value) { power_fail(b,"startup_readback_invalid");return; }
    memcpy(&actual,v->attribute.data.value,a->size);
    if(actual!=a->value) { power_fail(b,"startup_readback_mismatch");return; }
    a->stage=3;
    for(unsigned i=0;i<b->power_count;i++)if(b->power_attrs[i].stage!=3)return;
    b->power_status=2;b->power_until=0;
}
static const char *startup_set(bulb_t *b,const cJSON *o)
{
    startup_config_t next=b->startup;
    if(!startup_patch(&next,o))return "invalid_startup";
    bool power=cJSON_HasObjectItem(o,"powerMode") || cJSON_HasObjectItem(o,"power");
    if(power) {
        if(b->power_status==1)return "startup_busy";
        if(next.power_mode && (!b->addressed || !b->on_at || now_ms()-b->on_at>=20000))return "light_offline";
        const char *failure=power_prepare(b,&next);if(failure)return failure;
    }
    b->startup=next;save_identities();
    if(power) {
        b->power_status=0;b->power_error[0]=0;
        if(next.power_mode)power_begin(b,true);
    }
    return NULL;
}
static void read_cluster(bulb_t *b,uint16_t cluster,uint16_t *attrs,uint8_t count)
{
    esp_zb_zcl_read_attr_cmd_t r={.zcl_basic_cmd=destination(b),.address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID=cluster,.attr_number=count,.attr_field=attrs};
    esp_zb_zcl_read_attr_cmd_req(&r);
}
static void read_state(bulb_t *b)
{
    static uint16_t base[]={0};
    static uint16_t color[]={0,1,3,4,7,8};
    read_cluster(b,ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,base,1);
    read_cluster(b,ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,base,1);
    read_cluster(b,ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,color,6);
    if (!b->info_at || now_ms()-b->info_at>=60000) {
        static uint16_t basic[]={4,5},details[]={0x400a,0x400b,0x400c};
        read_cluster(b,ESP_ZB_ZCL_CLUSTER_ID_BASIC,basic,2);
        read_cluster(b,ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,details,3);
        b->info_at=now_ms();
    }
}
static void attribute(bulb_t *b,uint16_t cluster,const esp_zb_zcl_attribute_t *a)
{
    if (!a || !a->data.value) return;
    uint8_t u8=0; uint16_t u16=0;
    if (a->data.size==1) memcpy(&u8,a->data.value,1);
    if (a->data.size==2) memcpy(&u16,a->data.value,2);
    uint64_t now=now_ms();
    if (cluster==ESP_ZB_ZCL_CLUSTER_ID_BASIC && (a->id==4 || a->id==5) && a->data.size>1) {
        const uint8_t *str=a->data.value;
        size_t len=str[0]; if (len>32 || len+1>a->data.size) return;
        char *out=a->id==4?b->manufacturer:b->model;
        memcpy(out,str+1,len); out[len]=0; return;
    }
    if (cluster==ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL && a->data.size==2) {
        if (a->id==0x400a) { b->capabilities=u16; b->caps_known=true; return; }
        if (a->id==0x400b) { b->min_mired=u16; return; }
        if (a->id==0x400c) { b->max_mired=u16; return; }
    }
    if (cluster==ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && a->id==0 && a->data.size==1) {
        b->actual_on=u8!=0; b->on_at=now;
        if (!b->requested) b->wanted.on=b->actual_on;
    } else if (cluster==ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL && a->id==0 && a->data.size==1 && u8!=255) {
        b->level=u8; b->level_at=now;
        if (!b->requested) b->wanted.level=u8?u8:1;
    } else if (cluster==ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
        if (a->id==0 && a->data.size==1 && u8<=254) { b->hue=u8; b->color_fields|=16; }
        if (a->id==1 && a->data.size==1 && u8<=254) { b->saturation=u8; b->color_fields|=32; }
        if (a->id==3 && a->data.size==2) { b->x=u16; b->color_fields|=1; }
        if (a->id==4 && a->data.size==2) { b->y=u16; b->color_fields|=2; }
        if (a->id==7 && a->data.size==2 && u16 && u16<0xfeff) { b->mired=u16; b->color_fields|=4; }
        if (a->id==8 && a->data.size==1 && u8<=2) { b->mode=u8; b->color_fields|=8; }
        b->color_at=now;
        if (!b->requested) {
            b->wanted.hue=(int)b->hue*360/254;
            b->wanted.saturation=b->mode==2?0:(int)b->saturation*100/254;
            if (b->mired) {
                int k=1000000/b->mired;
                b->wanted.kelvin=k<2000?2000:k>6500?6500:k;
            }
        }
    }
}
static esp_err_t action_handler(esp_zb_core_action_callback_id_t id,const void *message)
{
    if (!message) return ESP_OK;
    if (id==ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID) {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *r=message;
        bulb_t *b=find_source(r->info.src_address,r->info.src_endpoint);
        power_attr_t *power=b?power_response(b,&r->info,2):NULL;
        if(power) {
            const esp_zb_zcl_read_attr_resp_variable_t *v=r->variables;
            while(v && v->attribute.id!=power->attr)v=v->next;
            power_verify(b,power,v);return ESP_OK;
        }
        // Manufacturer-specific startup XY must never overwrite the live colour.
        if(r->info.header.fc&4)return ESP_OK;
        if (b && r->info.status==ESP_ZB_ZCL_STATUS_SUCCESS)
            for (const esp_zb_zcl_read_attr_resp_variable_t *a=r->variables;a;a=a->next)
                if (a->status==ESP_ZB_ZCL_STATUS_SUCCESS) attribute(b,r->info.cluster,&a->attribute);
    } else if(id==ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID) {
        const esp_zb_zcl_cmd_write_attr_resp_message_t *r=message;
        bulb_t *b=find_source(r->info.src_address,r->info.src_endpoint);
        power_attr_t *a=b?power_response(b,&r->info,1):NULL;
        if(a) {
            bool ok=r->info.status==ESP_ZB_ZCL_STATUS_SUCCESS;
            for(const esp_zb_zcl_write_attr_resp_variable_t *v=r->variables;v;v=v->next)if(v->status!=ESP_ZB_ZCL_STATUS_SUCCESS)ok=false;
            if(ok)power_read(b,a);else power_fail(b,"startup_write_rejected");
        }
    } else if (id==ESP_ZB_CORE_REPORT_ATTR_CB_ID) {
        const esp_zb_zcl_report_attr_message_t *r=message;
        bulb_t *b=find_source(r->src_address,r->src_endpoint);
        if (b && r->status==ESP_ZB_ZCL_STATUS_SUCCESS) attribute(b,r->cluster,&r->attribute);
    } else if (id==ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID) {
        const esp_zb_zcl_cmd_default_resp_message_t *r=message;
        if (r->status_code!=ESP_ZB_ZCL_STATUS_SUCCESS) ESP_LOGW(TAG,"ZCL 0x%04x command 0x%02x rejected: 0x%02x",
            r->info.cluster,r->resp_to_cmd,r->status_code);
    }
    return ESP_OK;
}
static void found(esp_zb_zdp_status_t status,uint16_t address,uint8_t endpoint,void *ctx)
{
    bulb_t *b=callback_bulb(ctx); if (!b) return; b->match_busy=false;
    if (status==ESP_ZB_ZDP_STATUS_SUCCESS && address==b->address && endpoint && endpoint<=240) {
        bool changed=b->identity.endpoint!=endpoint;
        b->identity.endpoint=endpoint; b->addressed=true; b->matching=false;
        if (changed) save_identities();
        ESP_LOGI(TAG,"Light %s endpoint %u",b->id,endpoint);
        read_state(b);
    } else if (++b->attempts>=3) {
        b->matching=false;
        ESP_LOGW(TAG,"No colour-light endpoint for %s; reopen pairing and power-cycle bulb",b->id);
    } else b->match_at=now_ms()+2000;
}
static void match(bulb_t *b)
{
    static uint16_t clusters[]={ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL};
    esp_zb_zdo_match_desc_req_param_t r={.dst_nwk_addr=b->address,.addr_of_interest=b->address,
        .profile_id=ESP_ZB_AF_HA_PROFILE_ID,.num_in_clusters=3,.cluster_list=clusters};
    b->match_busy=true;
    void *ctx=(void *)(uintptr_t)b->token;
    if (esp_zb_zdo_match_cluster(&r,found,ctx)!=ESP_OK) found(ESP_ZB_ZDP_STATUS_TIMEOUT,0xffff,0xff,ctx);
}
static void resolved(esp_zb_zdp_status_t status,esp_zb_zdo_nwk_addr_rsp_t *r,void *ctx)
{
    bulb_t *b=callback_bulb(ctx); if (!b) return; b->resolving=false;
    if (status==ESP_ZB_ZDP_STATUS_SUCCESS && r && r->nwk_addr<0xfff8 && !memcmp(r->ieee_addr,b->identity.ieee,8)) {
        bool changed=!b->addressed || b->address!=r->nwk_addr;
        b->address=r->nwk_addr; b->addressed=true;
        if (changed) { b->requested=false;read_state(b); }
    } else b->addressed=false;
}
static void resolve(bulb_t *b)
{
    esp_zb_zdo_nwk_addr_req_param_t r={.dst_nwk_addr=0xfffd};
    memcpy(r.ieee_addr_of_interest,b->identity.ieee,8);
    b->resolving=true; b->resolved_at=now_ms();
    esp_zb_zdo_nwk_addr_req(&r,resolved,(void *)(uintptr_t)b->token);
}
static void commission(uint8_t mode)
{
    if (esp_zb_bdb_start_top_level_commissioning(mode)!=ESP_OK) esp_zb_scheduler_alarm(commission,mode,2000);
}
static esp_err_t permit_join(unsigned seconds)
{
    esp_err_t err=seconds?esp_zb_bdb_open_network(seconds):esp_zb_bdb_close_network();
    if (err==ESP_OK) {
        if (seconds) { memset(removed,0,sizeof(removed)); save_identities(); }
        join_until=seconds?now_ms()+seconds*1000:0;
        ESP_LOGI(TAG,"Pairing %s (%u s)",seconds?"open":"closed",seconds);
    } else ESP_LOGW(TAG,"Pairing request failed: %s",esp_err_to_name(err));
    return err;
}
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal)
{
    esp_zb_app_signal_type_t type=*signal->p_app_signal;
    esp_err_t status=signal->esp_err_status;
    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP: commission(ESP_ZB_BDB_MODE_INITIALIZATION); break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status!=ESP_OK) {
            ESP_LOGW(TAG,"Zigbee initialization failed: %s; retrying",esp_err_to_name(status));
            esp_zb_scheduler_alarm(commission,ESP_ZB_BDB_MODE_INITIALIZATION,2000);
        }
        else if (esp_zb_bdb_is_factory_new()) commission(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        else {
            ready=true; permit_join(0);
            ESP_LOGI(TAG,"Zigbee network restored; channel %u",esp_zb_get_current_channel());
        }
        break;
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (status==ESP_OK) {
            ready=true; permit_join(0);
            ESP_LOGI(TAG,"Zigbee network formed; channel %u",esp_zb_get_current_channel());
        } else {
            ESP_LOGW(TAG,"Zigbee formation failed: %s; retrying",esp_err_to_name(status));
            esp_zb_scheduler_alarm(commission,ESP_ZB_BDB_MODE_NETWORK_FORMATION,2000);
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        if (status!=ESP_OK) break;
        const esp_zb_zdo_signal_device_annce_params_t *a=esp_zb_app_signal_get_params(signal->p_app_signal);
        bulb_t probe={0}; memcpy(probe.identity.ieee,a->ieee_addr,8); id_string(&probe);
        for (int i=0;i<MAX_LIGHTS;i++) if (!strcmp(removed[i],probe.id)) return;
        bulb_t *b=NULL;
        for (int i=0;i<MAX_LIGHTS;i++) if ((bulbs[i].identity.endpoint||bulbs[i].matching)
            && !memcmp(bulbs[i].identity.ieee,a->ieee_addr,8)) { b=&bulbs[i]; break; }
        if (!b && join_until>now_ms()) for (int i=0;i<MAX_LIGHTS;i++)
            if (!bulbs[i].identity.endpoint && !bulbs[i].matching) {
                b=&bulbs[i]; memset(b,0,sizeof(*b)); b->wanted=values_default();
                b->token=++next_token; b->config.transition=LAMP_TRANSITION_DS;b->startup=startup_default();break;
            }
        if (!b) break;
        memcpy(b->identity.ieee,a->ieee_addr,8); id_string(b);
        b->address=a->device_short_addr; b->addressed=true;
        if (b->identity.endpoint) {
            // Honour the bulb's own power-on setting; never replay an old ON after reboot.
            b->requested=false;b->dirty=b->retry_dirty=0;b->retry_at=0;b->scene_override=false;read_state(b);
        }
        else { b->matching=true; b->attempts=0; b->match_at=now_ms()+1000; }
        break;
    }
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (status==ESP_OK) { unsigned s=*(uint8_t *)esp_zb_app_signal_get_params(signal->p_app_signal); join_until=s?now_ms()+1000*s:0; }
        break;
    default: break;
    }
}
static void send_json(const cJSON *o)
{
    char json[WIRE_CAP],frame[WIRE_CAP];
    if (!cJSON_PrintPreallocated((cJSON *)o,json,sizeof(json),false)) return;
    size_t n=wire_encode(json,frame,sizeof(frame));
    if (n) uart_write_bytes(UART_LINK,frame,n);
}
static void snapshots(void)
{
    esp_zb_lock_acquire(portMAX_DELAY);
    uint64_t now=now_ms();
    cJSON *hello=cJSON_CreateObject(),*ids=cJSON_AddArrayToObject(hello,"ids");
    cJSON_AddNumberToObject(hello,"v",1); cJSON_AddStringToObject(hello,"type","hello");
    cJSON_AddNumberToObject(hello,"boot",boot_id); cJSON_AddBoolToObject(hello,"ready",ready);
    cJSON_AddNumberToObject(hello,"deviceApi",4);cJSON_AddNumberToObject(hello,"otaApi",1);
    cJSON_AddStringToObject(hello,"version",esp_app_get_description()->version);
    char fingerprint[65];update_hex(esp_app_get_description()->app_elf_sha256,32,fingerprint);
    cJSON_AddStringToObject(hello,"firmwareId",fingerprint);cJSON_AddBoolToObject(hello,"confirmed",update_confirmed());
    cJSON_AddNumberToObject(hello,"otaState",firmware_update.state);
    cJSON_AddNumberToObject(hello,"join",join_until>now?(join_until-now+999)/1000:0);
    cJSON_AddNumberToObject(hello,"channel",ready?esp_zb_get_current_channel():0);
    for (int i=0;i<MAX_LIGHTS;i++) if (bulbs[i].identity.endpoint) cJSON_AddItemToArray(ids,cJSON_CreateString(bulbs[i].id));
    esp_zb_lock_release();
    send_json(hello); cJSON_Delete(hello);
    for (int i=0;i<MAX_LIGHTS;i++) {
        esp_zb_lock_acquire(portMAX_DELAY);
        bulb_t copy=bulbs[i];
        esp_zb_lock_release();
        bulb_t *b=&copy; if (!b->identity.endpoint) continue;
        now=now_ms();
        cJSON *o=cJSON_CreateObject();
        cJSON_AddNumberToObject(o,"v",1); cJSON_AddStringToObject(o,"type","light"); cJSON_AddStringToObject(o,"id",b->id);
        cJSON_AddBoolToObject(o,"online",b->on_at && now-b->on_at<20000);
        cJSON_AddNumberToObject(o,"age",b->on_at?(double)(now-b->on_at)/1000:-1);
        cJSON_AddBoolToObject(o,"reported",b->on_at!=0); cJSON_AddBoolToObject(o,"on",b->actual_on);
        cJSON_AddNumberToObject(o,"level",b->level); cJSON_AddNumberToObject(o,"mode",b->mode);
        cJSON_AddNumberToObject(o,"mired",b->mired); cJSON_AddNumberToObject(o,"x",b->x); cJSON_AddNumberToObject(o,"y",b->y);
        cJSON_AddNumberToObject(o,"hue",b->hue); cJSON_AddNumberToObject(o,"saturation",b->saturation);
        cJSON_AddNumberToObject(o,"transition",b->config.transition); cJSON_AddBoolToObject(o,"legacyXy",b->config.xy);
        cJSON_AddStringToObject(o,"manufacturer",b->manufacturer); cJSON_AddStringToObject(o,"model",b->model);
        cJSON_AddNumberToObject(o,"capabilities",b->caps_known?b->capabilities:-1);
        cJSON_AddNumberToObject(o,"minMired",b->min_mired); cJSON_AddNumberToObject(o,"maxMired",b->max_mired);
        cJSON_AddItemToObject(o,"startup",startup_json(&b->startup));
        cJSON_AddNumberToObject(o,"powerStatus",b->power_status);cJSON_AddStringToObject(o,"powerError",b->power_error);
        cJSON_AddBoolToObject(o,"removing",b->leave_ticket!=0); cJSON_AddStringToObject(o,"removeError",b->remove_error);
        bool color_ok=(b->color_fields&8) && (b->mode==2?(b->color_fields&4):b->mode==0?(b->color_fields&48)==48:(b->color_fields&3)==3);
        cJSON_AddBoolToObject(o,"colorKnown",color_ok && b->color_at && now-b->color_at<20000);
        cJSON_AddBoolToObject(o,"requested",b->requested); cJSON_AddItemToObject(o,"wanted",values_json(b->wanted));
        send_json(o); cJSON_Delete(o);
    }
}
static const char *update_command(const cJSON *o,const char *op,int sid)
{
    if(!strcmp(op,"ota_begin")) {
        uint8_t bytes[UPDATE_HEADER];update_meta_t meta;
        if(update_unhex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"header")),bytes,sizeof(bytes))!=UPDATE_HEADER || !update_header(bytes,&meta))return "update_header";
        const char *error=update_begin(&firmware_update,&meta,2);
        if(!error) { update_sid=sid;update_last=now_ms(); }
        return error;
    }
    if(sid!=update_sid)return "update_session";
    update_last=now_ms();
    if(!strcmp(op,"ota_abort")) {
        if(firmware_update.state==3)return "update_restarting";
        update_abort(&firmware_update);return NULL;
    }
    if(!strcmp(op,"ota_data")) {
        int offset;uint8_t bytes[UPDATE_CHUNK];
        int n=update_unhex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"data")),bytes,sizeof(bytes));
        if(n<1 || !json_integer(cJSON_GetObjectItemCaseSensitive(o,"offset"),0,0x180000,&offset))return "update_chunk";
        return update_write(&firmware_update,(uint32_t)offset,bytes,(size_t)n);
    }
    if(!strcmp(op,"ota_finish"))return update_finish(&firmware_update);
    if(!strcmp(op,"ota_commit")) {
        const char *error=update_commit(&firmware_update);if(!error)update_reboot=now_ms()+4000;return error;
    }
    return "update_operation";
}
static void command(const char *json)
{
    static int previous_sid,previous_seq;
    static uint32_t previous_crc;
    static bool previous_ok;
    static char previous_error[64];
    cJSON *o=cJSON_Parse(json); if (!o) return;
    int v,sid,seq;
    if (!json_integer(cJSON_GetObjectItemCaseSensitive(o,"v"),1,1,&v)
        || !json_integer(cJSON_GetObjectItemCaseSensitive(o,"sid"),1,INT_MAX,&sid)
        || !json_integer(cJSON_GetObjectItemCaseSensitive(o,"seq"),1,INT_MAX,&seq)) { cJSON_Delete(o); return; }
    uint32_t crc=wire_crc(json,strlen(json));
    bool ok=false; const char *error="invalid_command";
    if (sid==previous_sid && seq==previous_seq) {
        if (crc==previous_crc) { ok=previous_ok; error=previous_error; }
        else error="sequence_conflict";
    } else {
        const char *op=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"op"));
        if(op && !strncmp(op,"ota_",4)) { const char *failure=update_command(o,op,sid);ok=!failure;error=failure?failure:""; }
        else if(firmware_update.state)error="update_busy";
        else if (!ready) error="network_starting";
        else if (op && !strcmp(op,"pair")) {
            int seconds;
            if (json_integer(cJSON_GetObjectItemCaseSensitive(o,"seconds"),0,180,&seconds)) {
                ok=permit_join(seconds)==ESP_OK; error=ok?"":"pair_failed";
            }
        } else if (op && !strcmp(op,"scene")) {
            const char *failure=scene_frame(o);ok=failure==NULL;error=failure?failure:"";
        } else if (op && (!strcmp(op,"remove") || !strcmp(op,"device") || !strcmp(op,"identify") || !strcmp(op,"startup"))) {
            bulb_t *b=find_id(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id")));
            if (!b) error="unknown_light";
            else if (!strcmp(op,"remove")) {
                const cJSON *force=cJSON_GetObjectItemCaseSensitive(o,"force");
                if (!cJSON_IsBool(force)) error="invalid_remove";
                else if (cJSON_IsTrue(force)) { forget(b); ok=true; }
                else if (b->leave_ticket) error="remove_pending";
                else if (!b->addressed) error="light_offline";
                else {
                    esp_zb_zdo_mgmt_leave_req_param_t req={.dst_nwk_addr=b->address,.rejoin=0,.remove_children=0};
                    memcpy(req.device_address,b->identity.ieee,8);
                    b->leave_ticket=++next_token; b->leave_until=now_ms()+15000; b->remove_error[0]=0;
                    b->dirty=b->retry_dirty=0; b->retry_at=0;
                    esp_zb_zdo_device_leave_req(&req,leave_done,(void *)(uintptr_t)b->leave_ticket); ok=true;
                }
            } else if (b->leave_ticket) error="remove_pending";
            else if(!strcmp(op,"startup")) {
                cJSON *params=cJSON_Duplicate(o,true);
                const char *transport[]={"v","sid","seq","op"};
                for(int i=0;i<4;i++)cJSON_DeleteItemFromObjectCaseSensitive(params,transport[i]);
                const char *failure=startup_set(b,params);ok=failure==NULL;error=failure?failure:"";cJSON_Delete(params);
            }
            else if (!strcmp(op,"device")) {
                int transition; const cJSON *xy=cJSON_GetObjectItemCaseSensitive(o,"legacyXy");
                if (json_integer(cJSON_GetObjectItemCaseSensitive(o,"transition"),0,100,&transition) && cJSON_IsBool(xy)) {
                    b->config.transition=transition; b->config.xy=cJSON_IsTrue(xy);b->scene_override=false; save_identities(); ok=true;
                    if (b->requested) b->dirty|=4;
                } else error="invalid_device_settings";
            } else if (!b->addressed) error="light_offline";
            else {
                esp_zb_zcl_identify_cmd_t req={.zcl_basic_cmd=destination(b),.address_mode=ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,.identify_time=3};
                esp_zb_zcl_identify_cmd_req(&req); ok=true;
            }
        } else if (op && !strcmp(op,"set")) {
            cJSON *targets=cJSON_GetObjectItemCaseSensitive(o,"targets"),*patch=cJSON_GetObjectItemCaseSensitive(o,"patch");
            int n=cJSON_GetArraySize(targets);
            bulb_t *selected[MAX_LIGHTS]; light_values_t next[MAX_LIGHTS];
            ok=cJSON_IsArray(targets) && n>0 && n<=MAX_LIGHTS;
            for (int i=0;ok && i<n;i++) {
                selected[i]=find_id(cJSON_GetStringValue(cJSON_GetArrayItem(targets,i)));
                if (!selected[i]) { ok=false; error="unknown_light"; break; }
                if (selected[i]->leave_ticket) { ok=false; error="remove_pending"; break; }
                for (int j=0;j<i;j++) if (selected[i]==selected[j]) ok=false;
                next[i]=selected[i]->wanted;
                if (!wake_patch(&next[i],&selected[i]->startup,patch)) { ok=false; error="invalid_patch"; }
            }
            if (ok) for (int i=0;i<n;i++) {
                bulb_t *b=selected[i]; b->wanted=next[i]; b->requested=true;b->scene_override=false;
                unsigned dirty=0;
                if (cJSON_HasObjectItem(patch,"on")) dirty|=next[i].on?7:1;
                if (cJSON_HasObjectItem(patch,"level")) dirty|=2;
                if (cJSON_HasObjectItem(patch,"kelvin")||cJSON_HasObjectItem(patch,"hue")||cJSON_HasObjectItem(patch,"saturation")) dirty|=4;
                b->dirty|=dirty; b->retry_dirty|=dirty;
                b->retry_at=now_ms()+2000;
            }
        }
        previous_sid=sid; previous_seq=seq; previous_crc=crc; previous_ok=ok;
        snprintf(previous_error,sizeof(previous_error),"%s",ok?"":error);
    }
    cJSON *ack=cJSON_CreateObject();
    cJSON_AddNumberToObject(ack,"v",1); cJSON_AddStringToObject(ack,"type","ack");
    cJSON_AddNumberToObject(ack,"sid",sid); cJSON_AddNumberToObject(ack,"seq",seq);
    cJSON_AddBoolToObject(ack,"ok",ok); cJSON_AddStringToObject(ack,"error",ok?"":error);
    send_json(ack); cJSON_Delete(ack); cJSON_Delete(o);
}
static void service_task(void *unused)
{
    static wire_rx_t rx;
    uint8_t bytes[128];
    uint64_t last_snapshot=0,last_poll=0,pair_start=0;
    unsigned transmit_index=0,poll_index=0;
    lamp_debounce_t key={0}; bool down=false;
    for (;;) {
        int length=uart_read_bytes(UART_LINK,bytes,sizeof(bytes),pdMS_TO_TICKS(20));
        esp_zb_lock_acquire(portMAX_DELAY);
        for (int i=0;i<length;i++) if (wire_feed(&rx,bytes[i])) command(rx.data);
        uint64_t now=now_ms();
        bool pressed=lamp_debounce(&key,gpio_get_level(LAMP_PAIR_GPIO)==0,(uint32_t)now);
        if (pressed && !down) pair_start=now;
        if (!pressed && down) {
            if (now-pair_start>=8000) {
                ESP_ERROR_CHECK(nvs_erase_all(settings)); ESP_ERROR_CHECK(nvs_commit(settings)); esp_zb_factory_reset();
            } else if (ready) permit_join(LAMP_JOIN_SECONDS);
        }
        down=pressed;
        if(firmware_update.state && firmware_update.state!=3 && now-update_last>60000)update_abort(&firmware_update);
        if (ready && !firmware_update.state) {
            for (int i=0;i<MAX_LIGHTS;i++) {
                bulb_t *b=&bulbs[i];
                if (b->leave_ticket && now>=b->leave_until) {
                    b->leave_ticket=0; b->leave_until=0; snprintf(b->remove_error,sizeof(b->remove_error),"leave_timeout");
                }
                if (b->leave_ticket) continue;
                if(b->power_status==1 && now>=b->power_until)power_fail(b,"startup_timeout");
                if(b->power_status==4 && b->addressed && b->on_at && now-b->on_at<20000 && (b->caps_known || b->startup.power_mode==1)) {
                    const char *failure=power_prepare(b,&b->startup);
                    if(failure)power_fail(b,failure);else power_begin(b,false);
                }
                if (b->matching && !b->match_busy && now>=b->match_at) match(b);
                if (b->identity.endpoint && !b->resolving && now-b->resolved_at>=30000) resolve(b);
                if (b->retry_at && now>=b->retry_at) { b->dirty|=b->retry_dirty; b->retry_dirty=0; b->retry_at=0; }
            }
            for (int i=0;i<MAX_LIGHTS;i++) {
                bulb_t *b=&bulbs[(transmit_index+i)%MAX_LIGHTS];
                if (b->identity.endpoint && !b->leave_ticket && b->addressed && b->dirty) { send_values(b); transmit_index=(transmit_index+i+1)%MAX_LIGHTS; break; }
            }
            if (now-last_poll>=500) {
                for (int i=0;i<MAX_LIGHTS;i++) {
                    bulb_t *b=&bulbs[(poll_index+i)%MAX_LIGHTS];
                    if (b->identity.endpoint && !b->leave_ticket) {
                        if (b->addressed) read_state(b);
                        else if (!b->resolving) resolve(b);
                        poll_index=(poll_index+i+1)%MAX_LIGHTS; break;
                    }
                }
                last_poll=now;
            }
        }
        esp_zb_lock_release();
        update_health_tick(ready);
        if(update_reboot && now>=update_reboot)esp_restart();
        if (now-last_snapshot>=2000) { snapshots(); last_snapshot=now; }
    }
}
static void zigbee_task(void *unused)
{
    esp_zb_cfg_t config={.esp_zb_role=ESP_ZB_DEVICE_TYPE_COORDINATOR,.install_code_policy=false,.nwk_cfg.zczr_cfg.max_children=MAX_LIGHTS};
    esp_zb_init(&config);
    esp_zb_color_dimmable_switch_cfg_t cfg=ESP_ZB_DEFAULT_COLOR_DIMMABLE_SWITCH_CONFIG();
    esp_zb_ep_list_t *ep=esp_zb_color_dimmable_switch_ep_create(LAMP_ENDPOINT,&cfg); assert(ep);
    ESP_ERROR_CHECK(esp_zb_device_register(ep)); esp_zb_core_action_handler_register(action_handler);
    ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(1UL<<LAMP_ZIGBEE_CHANNEL));
    ESP_ERROR_CHECK(esp_zb_start(false));
    BaseType_t result=xTaskCreate(service_task,"uart_service",10240,NULL,4,NULL);
    assert(result==pdPASS);
    esp_zb_stack_main_loop();
}
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init()); ESP_ERROR_CHECK(nvs_open("lamp",NVS_READWRITE,&settings));
    identity_t ids[MAX_LIGHTS]={0}; size_t n=sizeof(ids);
    esp_err_t err=nvs_get_blob(settings,"lights_v2",ids,&n);
    if (err==ESP_ERR_NVS_NOT_FOUND) {
        n=sizeof(identity_t); err=nvs_get_blob(settings,"light",ids,&n); // Migrate the previous single-bulb firmware.
    }
    if (err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND) ESP_ERROR_CHECK(err);
    for (int i=0;i<MAX_LIGHTS;i++) {
        bulbs[i].wanted=values_default();
        bulbs[i].startup=startup_default();
        bulbs[i].token=++next_token; bulbs[i].config.transition=LAMP_TRANSITION_DS;
        if (err==ESP_OK && ids[i].endpoint>0 && ids[i].endpoint<=240) { bulbs[i].identity=ids[i]; id_string(&bulbs[i]); }
    }
    light_config_t configs[MAX_LIGHTS]; n=sizeof(configs);
    if (nvs_get_blob(settings,"device_cfg1",configs,&n)==ESP_OK && n==sizeof(configs))
        for (int i=0;i<MAX_LIGHTS;i++) if (configs[i].transition<=100) bulbs[i].config=configs[i];
    startup_config_t starts[MAX_LIGHTS];n=sizeof(starts);
    if(nvs_get_blob(settings,"startup_v1",starts,&n)==ESP_OK && n==sizeof(starts))
        for(int i=0;i<MAX_LIGHTS;i++)if(startup_valid(&starts[i])) {
            bulbs[i].startup=starts[i];bulbs[i].power_status=starts[i].power_mode?4:0;
        }
    n=sizeof(removed);
    if (nvs_get_blob(settings,"removed",removed,&n)!=ESP_OK || n!=sizeof(removed)) memset(removed,0,sizeof(removed));
    for (int i=0;i<MAX_LIGHTS;i++) removed[i][ID_LEN-1]=0;
    gpio_config_t rf={.pin_bit_mask=(1ULL<<LAMP_RF_POWER_GPIO)|(1ULL<<LAMP_RF_SELECT_GPIO),.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&rf));
    ESP_ERROR_CHECK(gpio_set_level(LAMP_RF_POWER_GPIO,0));
    ESP_ERROR_CHECK(gpio_set_level(LAMP_RF_SELECT_GPIO,LAMP_EXTERNAL_ANTENNA));
    ESP_LOGI(TAG,"XIAO coordinator; UART TX=%d RX=%d; %s antenna",LAMP_UART_TX_GPIO,LAMP_UART_RX_GPIO,
        LAMP_EXTERNAL_ANTENNA?"external":"internal");
    gpio_config_t key={.pin_bit_mask=1ULL<<LAMP_PAIR_GPIO,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&key));
    uart_config_t uart={.baud_rate=115200,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    // IDF 5.3.1 enables UART1's bus clock but leaves its core clock gated on this C6.
    // Enable it before either API can wait for a UART register synchronization.
    HP_UART_SRC_CLK_ATOMIC() {
        uart_ll_set_sclk(UART_LL_GET_HW(UART_LINK),UART_SCLK_DEFAULT);
        uart_ll_sclk_enable(UART_LL_GET_HW(UART_LINK));
    }
    ESP_ERROR_CHECK(uart_param_config(UART_LINK,&uart));
    ESP_ERROR_CHECK(uart_driver_install(UART_LINK,4096,4096,0,NULL,0));
    ESP_ERROR_CHECK(uart_set_pin(UART_LINK,LAMP_UART_TX_GPIO,LAMP_UART_RX_GPIO,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    boot_id=(int)(esp_random()&INT_MAX); if (!boot_id) boot_id=1;
    esp_zb_platform_config_t platform={.radio_config.radio_mode=ZB_RADIO_MODE_NATIVE,.host_config.host_connection_mode=ZB_HOST_CONNECTION_MODE_NONE};
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform));
    BaseType_t result=xTaskCreate(zigbee_task,"zigbee",6144,NULL,5,NULL);
    assert(result==pdPASS);
}
