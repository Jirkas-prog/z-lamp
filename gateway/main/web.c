#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_http_server.h"
#include "updates.h"
#include "api.h"
#include "ble.h"

extern const char index_start[] asm("_binary_index_html_start"),index_end[] asm("_binary_index_html_end");
extern const char js_start[] asm("_binary_app_js_start"),js_end[] asm("_binary_app_js_end");
extern const char css_start[] asm("_binary_style_css_start"),css_end[] asm("_binary_style_css_end");
static esp_err_t reply(httpd_req_t *r,const char *status,cJSON *json)
{
    char *body=cJSON_PrintUnformatted(json); cJSON_Delete(json);
    httpd_resp_set_status(r,status); httpd_resp_set_type(r,"application/json; charset=utf-8");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    esp_err_t result=body?httpd_resp_send(r,body,HTTPD_RESP_USE_STRLEN):httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"memory");
    free(body); return result;
}
static esp_err_t error(httpd_req_t *r,const char *status,const char *message)
{
    cJSON *o=cJSON_CreateObject(); cJSON_AddStringToObject(o,"error",message); return reply(r,status,o);
}
static esp_err_t success(httpd_req_t *r)
{
    cJSON *o=cJSON_CreateObject(); cJSON_AddBoolToObject(o,"ok",true); return reply(r,"200 OK",o);
}
static bool same_origin(httpd_req_t *r)
{
    if (!httpd_req_get_hdr_value_len(r,"Origin")) return true;
    char origin[150],host[128],expected[140];
    if (httpd_req_get_hdr_value_str(r,"Origin",origin,sizeof(origin))!=ESP_OK || httpd_req_get_hdr_value_str(r,"Host",host,sizeof(host))!=ESP_OK) return false;
    snprintf(expected,sizeof(expected),"http://%s",host); return !strcmp(origin,expected);
}
static bool keys(const cJSON *o,const char *const *allowed)
{
    if (!cJSON_IsObject(o)) return false;
    for (const cJSON *p=o->child;p;p=p->next) {
        bool found=false;
        for (int i=0;allowed[i];i++) if (!strcmp(allowed[i],p->string)) found=true;
        if (!found) return false;
        for (const cJSON *q=o->child;q!=p;q=q->next) if (!strcmp(q->string,p->string)) return false;
    }
    return true;
}
static esp_err_t assets(httpd_req_t *r)
{
    const char *start=index_start,*end=index_end,*type="text/html; charset=utf-8";
    if (!strcmp(r->uri,"/app.js")) { start=js_start; end=js_end; type="text/javascript; charset=utf-8"; }
    else if (!strcmp(r->uri,"/style.css")) { start=css_start; end=css_end; type="text/css; charset=utf-8"; }
    httpd_resp_set_type(r,type);
    httpd_resp_set_hdr(r,"Cache-Control","no-store"); httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    return httpd_resp_send(r,start,end-start-1);
}
static cJSON *groups_json(void)
{
    cJSON *array=cJSON_CreateArray();
    for (int i=0;i<MAX_GROUPS;i++) {
        group_t *g=&app.config.groups[i]; if (!g->used) continue;
        cJSON *o=cJSON_CreateObject(),*members=cJSON_AddArrayToObject(o,"members"); char id[4]; snprintf(id,sizeof(id),"g%d",i+1);
        cJSON_AddStringToObject(o,"id",id); cJSON_AddStringToObject(o,"name",g->name);
        for (unsigned j=0;j<g->count;j++) cJSON_AddItemToArray(members,cJSON_CreateString(g->members[j]));
        cJSON_AddItemToArray(array,o);
    }
    return array;
}
static cJSON *light_json(light_t *l,bool link,uint64_t now)
{
        cJSON *p=cJSON_CreateObject();
        cJSON_AddStringToObject(p,"id",l->id); cJSON_AddStringToObject(p,"name",app_name(l->id));
        cJSON_AddBoolToObject(p,"online",link && l->online); cJSON_AddBoolToObject(p,"reported",l->reported);
        cJSON_AddBoolToObject(p,"on",l->on); cJSON_AddNumberToObject(p,"level",l->level);
        cJSON_AddNumberToObject(p,"age",l->age+(app.c6_last_ms?(double)(now-app.c6_last_ms)/1000:0));
        cJSON_AddBoolToObject(p,"colorKnown",link && l->online && l->color_known);
        cJSON_AddBoolToObject(p,"requested",l->requested); cJSON_AddItemToObject(p,"wanted",values_json(l->wanted));
        cJSON_AddNumberToObject(p,"transition",l->transition); cJSON_AddBoolToObject(p,"legacyXy",l->legacy_xy);
        cJSON_AddBoolToObject(p,"removing",l->removing); cJSON_AddStringToObject(p,"removeError",l->remove_error);
        cJSON_AddStringToObject(p,"manufacturer",l->manufacturer); cJSON_AddStringToObject(p,"model",l->model);
        cJSON_AddNumberToObject(p,"capabilities",l->capabilities);
        cJSON_AddItemToObject(p,"startup",startup_json(&l->startup));
        cJSON_AddNumberToObject(p,"powerStatus",l->power_status);cJSON_AddStringToObject(p,"powerError",l->power_error);
        cJSON_AddNumberToObject(p,"minKelvin",l->max_mired?1000000/l->max_mired:2000);
        cJSON_AddNumberToObject(p,"maxKelvin",l->min_mired?1000000/l->min_mired:6500);
        cJSON_AddNumberToObject(p,"colorMode",l->mode);
        uint8_t rgb[3]={100,100,100};
        if (l->color_known) {
            lamp_xy_t xy=l->mode==2?lamp_mix(l->mired?1000000.0f/l->mired:2700,0,0):(lamp_xy_t){l->x/65536.0f,l->y/65536.0f};
            lamp_xy_rgb(xy.x,xy.y,1,rgb);
            if (l->mode==0) lamp_hsv_rgb(l->hue/254.0f,l->saturation/254.0f,rgb);
        }
        char color[8]; snprintf(color,sizeof(color),"#%02x%02x%02x",rgb[0],rgb[1],rgb[2]);
        cJSON_AddStringToObject(p,"color",color);
        return p;
}
static cJSON *screen_json(void)
{
    cJSON *screen=cJSON_CreateObject(); uint8_t rgb[3]; values_rgb(app.config.lamp,rgb);
    char color[8]; snprintf(color,sizeof(color),"#%02x%02x%02x",rgb[0],rgb[1],rgb[2]);
    cJSON_AddStringToObject(screen,"id","display"); cJSON_AddStringToObject(screen,"name",app_name("display"));
    cJSON_AddBoolToObject(screen,"online",true); cJSON_AddBoolToObject(screen,"reported",true);
    cJSON_AddBoolToObject(screen,"on",app.screen.awake && app.config.lamp_mode && app.config.lamp.on);
    cJSON_AddBoolToObject(screen,"colorKnown",true); cJSON_AddStringToObject(screen,"color",color);
    cJSON_AddNumberToObject(screen,"level",app.config.lamp.level); cJSON_AddNumberToObject(screen,"age",0);
    cJSON_AddItemToObject(screen,"wanted",values_json(app.config.lamp));
    return screen;
}
static cJSON *timers_json(uint64_t now)
{
    cJSON *items=cJSON_CreateArray();
    for(int i=0;i<MAX_LIGHTS;i++)if(app.arrival[i].id[0]) {
        arrival_timer_t *t=&app.arrival[i];cJSON *item=cJSON_CreateObject();
        cJSON_AddStringToObject(item,"id",t->id);
        cJSON_AddNumberToObject(item,"seconds",now>=t->due?0:(t->due-now+999)/1000);
        cJSON_AddBoolToObject(item,"explicit",t->explicit_timer);cJSON_AddItemToArray(items,item);
    }
    return items;
}
static esp_err_t state_get(httpd_req_t *r)
{
    cJSON *o=cJSON_CreateObject(),*array=cJSON_AddArrayToObject(o,"lights");
    app_lock();
    uint64_t now=app_now(); bool link=app.c6_last_ms && now-app.c6_last_ms<6000 && app.c6_ready;
    cJSON_AddBoolToObject(o,"connected",link); cJSON_AddNumberToObject(o,"channel",app.channel);
    cJSON_AddNumberToObject(o,"deviceApi",app.device_api);
    cJSON_AddStringToObject(o,"alias",app.runtime_alias); cJSON_AddStringToObject(o,"hostname",app.hostname);
    cJSON_AddBoolToObject(o,"aliasPending",strcmp(app.alias,app.runtime_alias)!=0);
    int joining=app.join_seconds-(app.c6_last_ms?(int)((now-app.c6_last_ms)/1000):0);
    cJSON_AddNumberToObject(o,"joinSeconds",link && joining>0?joining:0);
    cJSON_AddStringToObject(o,"apSsid",app.runtime_ssid); cJSON_AddStringToObject(o,"apIp","192.168.4.1");
    cJSON_AddBoolToObject(o,"apEnabled",app.options.ap_enabled); cJSON_AddBoolToObject(o,"apActive",app.ap_active);
    cJSON_AddStringToObject(o,"staSsid",app.sta_ssid); cJSON_AddStringToObject(o,"wifiError",app.wifi_error);
    cJSON_AddNumberToObject(o,"networkCount",app.options.network_count);
    cJSON_AddStringToObject(o,"staIp",app.sta_ip); cJSON_AddBoolToObject(o,"staConnected",app.sta_connected);
    cJSON_AddNumberToObject(o,"clients",app.ap_clients); cJSON_AddStringToObject(o,"lastError",app.error);
    cJSON_AddBoolToObject(o,"saving",app.dirty_ms!=0);
    for(int i=0;i<MAX_LIGHTS;i++) if(app.lights[i].known) cJSON_AddItemToArray(array,light_json(&app.lights[i],link,now));
    cJSON_AddItemToArray(array,screen_json());
    cJSON *display=cJSON_AddObjectToObject(o,"display");
    cJSON_AddBoolToObject(display,"awake",app.screen.awake); cJSON_AddBoolToObject(display,"bootOn",app.config.boot_on);
    cJSON_AddNumberToObject(display,"timeout",app.config.timeout); cJSON_AddStringToObject(display,"mode",app.config.lamp_mode?"lamp":"status");
    if (app.ir_learn_key>=0 && now>=app.ir_learn_until) app.ir_learn_key=-1;
    cJSON *ir=cJSON_AddObjectToObject(o,"ir");
    char code[9]; snprintf(code,sizeof(code),"%08lX",(unsigned long)app.ir_last_code);
    cJSON_AddStringToObject(ir,"lastCode",app.ir_last_code?code:"");
    cJSON_AddNumberToObject(ir,"lastKey",ir_match(app.ir.codes,app.ir_last_code));
    cJSON_AddNumberToObject(ir,"age",app.ir_last_ms?(double)(now-app.ir_last_ms)/1000:-1);
    cJSON_AddNumberToObject(ir,"learningKey",app.ir_learn_key);
    cJSON_AddNumberToObject(ir,"learningSeconds",app.ir_learn_key<0?0:(app.ir_learn_until-now+999)/1000);
    cJSON_AddNumberToObject(ir,"effect",app.ir_effect);
    cJSON_AddStringToObject(ir,"sceneName",app.ir_effect!=IR_STATIC?app.playing.name:"");
    cJSON_AddNumberToObject(ir,"sceneLevel",app.ir_effect!=IR_STATIC?app.playing.level:0);
    cJSON_AddNumberToObject(ir,"sceneIndex",app.ir_effect!=IR_STATIC?app.active_scene:-1);
    cJSON *codes=cJSON_AddArrayToObject(ir,"codes");
    for (int i=0;i<IR_KEYS;i++) {
        snprintf(code,sizeof(code),"%08lX",(unsigned long)app.ir.codes[i]);
        cJSON_AddItemToArray(codes,cJSON_CreateString(app.ir.codes[i]?code:""));
    }
    cJSON_AddItemToObject(o,"groups",groups_json());
    cJSON_AddItemToObject(o,"timers",timers_json(now));
    app_unlock(); return reply(r,"200 OK",o);
}
static esp_err_t settings_get(httpd_req_t *r)
{
    cJSON *o=cJSON_CreateObject(); app_lock(); settings_t *s=&app.config;
    cJSON_AddStringToObject(o,"alias",app.alias);
    cJSON_AddBoolToObject(o,"bootOn",s->boot_on); cJSON_AddNumberToObject(o,"timeout",s->timeout);
    cJSON_AddNumberToObject(o,"statusBrightness",s->status_brightness); cJSON_AddStringToObject(o,"mode",s->lamp_mode?"lamp":"status");
    cJSON_AddBoolToObject(o,"apEnabled",app.options.ap_enabled);
    cJSON *networks=cJSON_AddArrayToObject(o,"networks");
    for (unsigned i=0;i<app.options.network_count;i++) {
        cJSON *network=cJSON_CreateObject();
        cJSON_AddStringToObject(network,"ssid",app.options.networks[i].ssid);
        cJSON_AddStringToObject(network,"password",app.options.networks[i].password);
        cJSON_AddItemToArray(networks,network);
    }
    cJSON_AddStringToObject(o,"presetTarget",app.options.preset_target);
    cJSON_AddItemToObject(o,"preset",values_json(app.options.preset));
    cJSON_AddStringToObject(o,"apSsid",s->ap_ssid); cJSON_AddStringToObject(o,"apPassword",s->ap_password);
    cJSON_AddBoolToObject(o,"irEnabled",app.ir.enabled); cJSON_AddStringToObject(o,"irTarget",app.ir.target);
    cJSON *ir_names=cJSON_AddArrayToObject(o,"irKeys");
    for (int i=0;i<IR_KEYS;i++) cJSON_AddItemToArray(ir_names,cJSON_CreateString(ir_keys[i].name));
    app_unlock(); return reply(r,"200 OK",o);
}
static bool string_copy(const cJSON *o,const char *key,char *dest,size_t cap)
{
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(o,key); if (!p) return true;
    const char *s=cJSON_GetStringValue(p); if (!s || strlen(s)>=cap) return false;
    for (const unsigned char *c=(const unsigned char *)s;*c;c++) if (*c<32 || *c==127) return false;
    snprintf(dest,cap,"%s",s); return true;
}
static bool boolean_copy(const cJSON *o,const char *key,bool *value)
{
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(o,key); if (!p) return true;
    if (!cJSON_IsBool(p)) return false;
    *value=cJSON_IsTrue(p); return true;
}
static bool unsigned_copy(const cJSON *o,const char *key,unsigned *value,int min,int max)
{
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(o,key); if (!p) return true;
    int n; if (!json_integer(p,min,max,&n)) return false; *value=n; return true;
}
static esp_err_t scenes_get(httpd_req_t *r)
{
    cJSON *o=cJSON_CreateObject(),*items=cJSON_AddArrayToObject(o,"scenes"),*modes=cJSON_AddArrayToObject(o,"modes"),*buttons=cJSON_AddArrayToObject(o,"buttons");
    app_lock();
    for(int i=0;i<MAX_SCENES;i++)if(app.scenes.items[i].used)cJSON_AddItemToArray(items,scene_json(&app.scenes.items[i],i));
    for(int i=0;i<4;i++) {
        char id[16];snprintf(id,sizeof(id),"s%d",app.scenes.buttons[i]+1);
        cJSON_AddItemToArray(buttons,app.scenes.buttons[i]>=0?cJSON_CreateString(id):cJSON_CreateNull());
    }
    cJSON_AddNumberToObject(o,"whiteKelvin",app.scenes.white_kelvin);cJSON_AddNumberToObject(o,"whiteLevel",app.scenes.white_level);
    cJSON_AddNumberToObject(o,"brightnessSteps",app.remote.count);cJSON_AddBoolToObject(o,"customBrightness",app.remote.custom);
    cJSON *levels=cJSON_AddArrayToObject(o,"brightnessLevels"),*members=cJSON_AddArrayToObject(o,"irMembers");
    for(unsigned i=0;i<app.remote.count;i++)cJSON_AddItemToArray(levels,cJSON_CreateNumber(app.remote.percent[i]));
    for(unsigned i=0;i<app.remote.member_count;i++)cJSON_AddItemToArray(members,cJSON_CreateString(app.remote.members[i]));
    for(int i=0;i<SCENE_MODES;i++) {
        cJSON *m=cJSON_CreateObject();cJSON_AddStringToObject(m,"name",scene_modes[i].name);cJSON_AddStringToObject(m,"description",scene_modes[i].description);
        cJSON_AddNumberToObject(m,"hold",scene_modes[i].hold_ds);cJSON_AddNumberToObject(m,"fade",scene_modes[i].fade_ds);cJSON_AddItemToArray(modes,m);
    }
    app_unlock();return reply(r,"200 OK",o);
}
static const char *remote_set(const cJSON *o)
{
    const char *const allowed[]={"whiteKelvin","whiteLevel","brightnessSteps","brightnessLevels","customBrightness","irMembers","buttons",NULL};
    if(!keys(o,allowed))return "invalid_remote";
    app_lock();unsigned kelvin=app.scenes.white_kelvin,level=app.scenes.white_level,steps=app.remote.count;remote_config_t remote=app.remote;
    int assignments[4];memcpy(assignments,app.scenes.buttons,sizeof(assignments));
    bool valid=unsigned_copy(o,"whiteKelvin",&kelvin,2000,6500) && unsigned_copy(o,"whiteLevel",&level,1,254) && unsigned_copy(o,"brightnessSteps",&steps,2,IR_LEVELS) && boolean_copy(o,"customBrightness",&remote.custom);
    remote.count=steps;
    const cJSON *levels=cJSON_GetObjectItemCaseSensitive(o,"brightnessLevels"),*members=cJSON_GetObjectItemCaseSensitive(o,"irMembers");
    if(remote.custom) {
        if(!levels && remote.count!=app.remote.count)valid=false;
        if(levels) {
            if(!cJSON_IsArray(levels) || cJSON_GetArraySize(levels)!=(int)steps)valid=false;
            else for(unsigned i=0;i<steps;i++) { int value;if(!json_integer(cJSON_GetArrayItem(levels,i),1,100,&value))valid=false;else remote.percent[i]=(uint8_t)value; }
        }
    } else ir_default_scale(remote.percent,steps);
    if(members) {
        int count=cJSON_GetArraySize(members);
        if(!cJSON_IsArray(members) || count>MAX_LIGHTS+1)valid=false;
        else {
            remote.member_count=(unsigned)count;
            for(int i=0;i<count;i++) {
                const char *id=cJSON_GetStringValue(cJSON_GetArrayItem(members,i));
                if(!id || (strcmp(id,"display") && !app_light(id)))valid=false;
                else snprintf(remote.members[i],ID_LEN,"%s",id);
            }
        }
    }
    valid=valid && remote_valid(&remote);
    const cJSON *buttons=cJSON_GetObjectItemCaseSensitive(o,"buttons");
    if(buttons) {
        if(!cJSON_IsArray(buttons)||cJSON_GetArraySize(buttons)!=4)valid=false;
        else for(int i=0;i<4;i++) {
            const cJSON *b=cJSON_GetArrayItem(buttons,i);int index=scene_index(cJSON_GetStringValue(b));
            if(!cJSON_IsNull(b) && (index<0 || !app.scenes.items[index].used))valid=false;
            assignments[i]=index;
        }
    }
    if(valid) {
        if(members && (app.remote.member_count!=remote.member_count || memcmp(app.remote.members,remote.members,remote.member_count*ID_LEN))) {
            app.ir_effect=IR_STATIC;app.ir_generation++;
        }
        app.scenes.white_kelvin=kelvin;app.scenes.white_level=level;app.remote=remote;
        memcpy(app.scenes.buttons,assignments,sizeof(assignments));app.ir_revision++;app_changed();
    }
    app_unlock();return valid?NULL:"invalid_remote";
}
static const char *scene_set(const cJSON *o)
{
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id"));int index=scene_index(id);
    if(cJSON_HasObjectItem(o,"id") && index<0)return "invalid_scene";
    if(cJSON_HasObjectItem(o,"delete")) {
        const char *const allowed[]={"id","delete",NULL};
        if(!keys(o,allowed) || index<0 || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"delete")))return "invalid_scene";
        app_lock();
        if(!app.scenes.items[index].used) { app_unlock();return "unknown_scene"; }
        memset(&app.scenes.items[index],0,sizeof(scene_t));
        for(int i=0;i<4;i++)if(app.scenes.buttons[i]==index)app.scenes.buttons[i]=-1;
        if(app.active_scene==index) { app.ir_effect=IR_STATIC;app.ir_generation++; }
        app.ir_revision++;app_changed();app_unlock();return NULL;
    }
    scene_t s;if(!scene_parse(&s,o))return "invalid_scene";
    app_lock();
    for(unsigned i=0;i<s.member_count;i++)if(strcmp(s.members[i].id,"display") && !app_light(s.members[i].id)) { app_unlock();return "scene_missing_device"; }
    if(index<0)for(int i=0;i<MAX_SCENES;i++)if(!app.scenes.items[i].used) { index=i;break; }
    if(index<0) { app_unlock();return "scene_limit"; }
    app.scenes.items[index]=s;
    if(app.active_scene==index) { app.ir_effect=IR_STATIC;app.ir_generation++; }
    app.ir_revision++;app_changed();app_unlock();return NULL;
}
static const char *settings_set(const cJSON *o)
{
    const char *const allowed[]={"alias","bootOn","timeout","statusBrightness","mode","networks","presetTarget","preset","apSsid","apPassword","irEnabled","irTarget",NULL};
    if (!keys(o,allowed)) return "invalid_settings";
    app_lock(); settings_t s=app.config; options_t options=app.options; ir_settings_t ir=app.ir;
    char alias[NAME_LEN]; snprintf(alias,sizeof(alias),"%s",app.alias);
    if (!string_copy(o,"alias",alias,sizeof(alias)) || !valid_alias(alias)) { app_unlock(); return "invalid_alias"; }
    bool valid=boolean_copy(o,"bootOn",&s.boot_on) && boolean_copy(o,"irEnabled",&ir.enabled)
        && string_copy(o,"irTarget",ir.target,sizeof(ir.target))
        && unsigned_copy(o,"timeout",&s.timeout,0,3600)
        && unsigned_copy(o,"statusBrightness",&s.status_brightness,1,100)
        && string_copy(o,"apSsid",s.ap_ssid,sizeof(s.ap_ssid)) && string_copy(o,"apPassword",s.ap_password,sizeof(s.ap_password))
        && string_copy(o,"presetTarget",options.preset_target,sizeof(options.preset_target));
    const cJSON *networks=cJSON_GetObjectItemCaseSensitive(o,"networks");
    if (networks) {
        int count=cJSON_GetArraySize(networks);
        if (!cJSON_IsArray(networks) || count>MAX_NETWORKS) valid=false;
        else {
            memset(options.networks,0,sizeof(options.networks)); options.network_count=count;
            const char *const network_keys[]={"ssid","password",NULL};
            for (int i=0;i<count;i++) {
                const cJSON *item=cJSON_GetArrayItem(networks,i); network_t *network=&options.networks[i];
                if (!keys(item,network_keys) || !string_copy(item,"ssid",network->ssid,sizeof(network->ssid))
                    || !string_copy(item,"password",network->password,sizeof(network->password))
                    || !network->ssid[0] || strlen(network->password)<8) valid=false;
                for (int j=0;j<i;j++) if (!strcmp(options.networks[j].ssid,network->ssid)) valid=false;
            }
        }
    }
    const cJSON *preset=cJSON_GetObjectItemCaseSensitive(o,"preset");
    if (preset && !values_patch(&options.preset,preset)) valid=false;
    options.preset.on=true;
    if (preset || cJSON_HasObjectItem(o,"presetTarget")) {
        const char *t=options.preset_target;
        bool exists=!strcmp(t,"all") || !strcmp(t,"display") || app_light(t);
        if (t[0]=='g' && t[1]>='1' && t[1]<='8' && !t[2]) exists=app.config.groups[t[1]-'1'].used;
        if (!exists) valid=false;
    }
    bool ir_changed=cJSON_HasObjectItem(o,"irEnabled") || cJSON_HasObjectItem(o,"irTarget");
    if (ir_changed && ir.enabled) {
        const char *t=ir.target;
        bool exists=!strcmp(t,"remote") || !strcmp(t,"all") || !strcmp(t,"display") || app_light(t);
        if (t[0]=='g' && t[1]>='1' && t[1]<='8' && !t[2]) exists=app.config.groups[t[1]-'1'].used;
        if (!exists) valid=false;
    }
    const cJSON *mode=cJSON_GetObjectItemCaseSensitive(o,"mode");
    if (mode) {
        const char *m=cJSON_GetStringValue(mode);
        if (!m || (strcmp(m,"status") && strcmp(m,"lamp"))) valid=false;
        else s.lamp_mode=!strcmp(m,"lamp");
    }
    if (!s.ap_ssid[0] || strlen(s.ap_password)<8) valid=false;
    // The small LCD must reproduce the access password exactly using its ASCII font.
    for (const unsigned char *p=(const unsigned char *)s.ap_password;*p;p++) if (*p>126) valid=false;
    if (valid) {
        app.config=s; app.options=options; app.ir=ir;
        snprintf(app.alias,sizeof(app.alias),"%s",alias);
        if (ir_changed) app.ir_revision++;
        if (ir_changed || mode) { app.ir_effect=IR_STATIC; app.ir_generation++; app.ir_learn_key=-1; }
        if (networks) app.wifi_revision++;
        app_changed();
    }
    app_unlock(); return valid?NULL:"invalid_settings";
}
static const char *rename_light(const cJSON *o)
{
    const char *const allowed[]={"id","name",NULL}; if (!keys(o,allowed)) return "invalid_name";
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id"));
    const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"name"));
    if ((!valid_light_id(id) && (!id || strcmp(id,"display"))) || !valid_name(name)) return "invalid_name";
    if (!strcmp(id,"display")) {
        app_lock(); snprintf(app.display_name,sizeof(app.display_name),"%s",name); app_changed(); app_unlock(); return NULL;
    }
    app_lock(); if (!app_light(id)) { app_unlock(); return "unknown_light"; }
    named_light_t *slot=NULL;
    for (int i=0;i<MAX_LIGHTS;i++) if (!strcmp(app.config.names[i].id,id)) slot=&app.config.names[i];
    if (!slot) for (int i=0;i<MAX_LIGHTS;i++) if (!app.config.names[i].id[0] || !app_light(app.config.names[i].id)) { slot=&app.config.names[i]; break; }
    if (slot) { snprintf(slot->id,sizeof(slot->id),"%s",id); snprintf(slot->name,sizeof(slot->name),"%s",name); app_changed(); }
    app_unlock(); return slot?NULL:"name_limit";
}
static const char *group_set(const cJSON *o)
{
    const char *const allowed[]={"id","name","members","delete",NULL}; if (!keys(o,allowed)) return "invalid_group";
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id")); int index=-1;
    if (id) { if (id[0]!='g'||id[1]<'1'||id[1]>'8'||id[2]) return "invalid_group"; index=id[1]-'1'; }
    app_lock();
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"delete"))) {
        if (index<0 || !app.config.groups[index].used) { app_unlock(); return "unknown_group"; }
        memset(&app.config.groups[index],0,sizeof(group_t));
        if (!strcmp(app.options.preset_target,id)) app.options.preset_target[0]=0;
        if (!strcmp(app.ir.target,id)) { app.ir.enabled=false; app.ir.target[0]=0; app.ir_effect=IR_STATIC; app.ir_generation++; app.ir_revision++; }
        app_changed(); app_unlock(); return NULL;
    }
    if (index<0) for (int i=0;i<MAX_GROUPS;i++) if (!app.config.groups[i].used) { index=i; break; }
    if (index<0) { app_unlock(); return "group_limit"; }
    const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"name"));
    const cJSON *members=cJSON_GetObjectItemCaseSensitive(o,"members"); int count=cJSON_GetArraySize(members);
    bool valid=valid_name(name) && cJSON_IsArray(members) && count>0 && count<=MAX_LIGHTS+1;
    group_t group={.used=true};
    if (valid) { snprintf(group.name,sizeof(group.name),"%s",name); group.count=count; }
    for (int i=0;valid && i<count;i++) {
        const char *member=cJSON_GetStringValue(cJSON_GetArrayItem(members,i));
        if (!member || (strcmp(member,"display") && !app_light(member))) { valid=false; break; }
        for (int j=0;j<i;j++) if (!strcmp(group.members[j],member)) valid=false;
        snprintf(group.members[i],ID_LEN,"%s",member);
    }
    if (valid) { app.config.groups[index]=group; app_changed(); }
    app_unlock(); return valid?NULL:"invalid_group";
}
static const char *ir_configure(const char *uri,const cJSON *o)
{
    const char *const allowed[]={"key",NULL}; int key;
    bool learn=!strcmp(uri,"/api/ir/learn");
    if (learn) {
        if (!keys(o,allowed) || !json_integer(cJSON_GetObjectItemCaseSensitive(o,"key"),0,IR_KEYS-1,&key)) return "invalid_ir_key";
    } else if (!cJSON_IsObject(o) || o->child) return "invalid_ir_request";
    app_lock();
    app.ir_effect=IR_STATIC; app.ir_generation++; app.ir_revision++;
    app.ir_learn_key=learn?key:-1; app.ir_learn_until=learn?app_now()+20000:0;
    if (!strcmp(uri,"/api/ir/reset")) {
        for (int i=0;i<IR_KEYS;i++) app.ir.codes[i]=ir_keys[i].code;
        app_changed();
    }
    app_unlock(); return NULL;
}
static const char *configure(const char *(*apply)(const cJSON *),const cJSON *args)
{
    xSemaphoreTake(command_mutex,portMAX_DELAY);
    app_lock();bool busy=app.updating;app_unlock();
    const char *failure=busy?"update_busy":apply(args);
    xSemaphoreGive(command_mutex);return failure;
}
const char *app_api(const char *uri,const cJSON *o)
{
    app_lock();bool busy=app.updating;app_unlock();
    if(busy && strcmp(uri,"/api/update/cancel"))return "update_busy";
    const char *failure="unknown_operation";
    if(!strcmp(uri,"/api/update/url")) {
        const char *const allowed[]={"target","url",NULL};
        if(keys(o,allowed))failure=updates_url(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"target")),cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"url")));
    } else if(!strcmp(uri,"/api/update/cancel") && cJSON_IsObject(o) && !o->child)failure=updates_cancel();
    else if (!strcmp(uri,"/api/control")) {
        const char *const allowed[]={"target","patch",NULL};
        if (keys(o,allowed)) failure=app_set(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"target")),cJSON_GetObjectItemCaseSensitive(o,"patch"));
    } else if (!strcmp(uri,"/api/timer")) {
        const char *const allowed[]={"target","seconds","level",NULL};int seconds,level=0;
        failure="invalid_timer";
        const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"level");
        if(keys(o,allowed) && json_integer(cJSON_GetObjectItemCaseSensitive(o,"seconds"),0,86400,&seconds)
            && (!v || json_integer(v,1,254,&level)))
            failure=app_timer(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"target")),seconds,level);
    } else if (!strcmp(uri,"/api/arrival")) {
        const char *const allowed[]={"target","level","offSeconds","kelvin",NULL};int level,seconds,kelvin;
        failure="invalid_arrival";
        if(keys(o,allowed) && json_integer(cJSON_GetObjectItemCaseSensitive(o,"level"),1,254,&level)
            && json_integer(cJSON_GetObjectItemCaseSensitive(o,"offSeconds"),0,3600,&seconds)
            && json_integer(cJSON_GetObjectItemCaseSensitive(o,"kelvin"),0,6500,&kelvin))
            failure=app_arrival(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"target")),level,seconds,kelvin);
    } else if (!strcmp(uri,"/api/pair")) {
        const char *const allowed[]={"seconds",NULL}; int seconds;
        if (keys(o,allowed) && json_integer(cJSON_GetObjectItemCaseSensitive(o,"seconds"),0,180,&seconds)) {
            cJSON *request=cJSON_CreateObject(); cJSON_AddStringToObject(request,"op","pair"); cJSON_AddNumberToObject(request,"seconds",seconds);
            xSemaphoreTake(command_mutex,portMAX_DELAY);
            app_lock();bool busy=app.updating;app_unlock();
            char message[80]; bool ok=!busy && bridge_request(request,message); cJSON_Delete(request);
            xSemaphoreGive(command_mutex);
            failure=busy?"update_busy":ok?NULL:"coordinator_rejected_or_timeout";
        }
    } else if (!strcmp(uri,"/api/ir/learn") || !strcmp(uri,"/api/ir/cancel") || !strcmp(uri,"/api/ir/reset")) {
        xSemaphoreTake(command_mutex,portMAX_DELAY);app_lock();bool busy=app.updating;app_unlock();
        failure=busy?"update_busy":ir_configure(uri,o);xSemaphoreGive(command_mutex);
    } else if (!strcmp(uri,"/api/settings")) {
        failure=configure(settings_set,o);
    }
    else if (!strcmp(uri,"/api/rename")) failure=configure(rename_light,o);
    else if (!strcmp(uri,"/api/remove")) {
        const char *const allowed[]={"id","force",NULL};
        if (keys(o,allowed) && valid_light_id(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id"))) && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(o,"force"))) failure=app_device("remove",o);
    } else if (!strcmp(uri,"/api/device")) {
        const char *const allowed[]={"id","transition","legacyXy",NULL}; int n;
        if (keys(o,allowed) && valid_light_id(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id")))
            && json_integer(cJSON_GetObjectItemCaseSensitive(o,"transition"),0,100,&n) && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(o,"legacyXy"))) failure=app_device("device",o);
    } else if(!strcmp(uri,"/api/startup")) {
        startup_config_t config=startup_default();
        if(valid_light_id(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id"))) && startup_patch(&config,o))failure=app_device("startup",o);
    } else if (!strcmp(uri,"/api/identify")) {
        const char *const allowed[]={"id",NULL};
        if (keys(o,allowed) && valid_light_id(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id")))) failure=app_device("identify",o);
    }
    else if (!strcmp(uri,"/api/group")) {
        failure=configure(group_set,o);
    }
    else if(!strcmp(uri,"/api/scenes")) {
        failure=configure(scene_set,o);
    } else if(!strcmp(uri,"/api/remote")) {
        failure=configure(remote_set,o);
    } else if(!strcmp(uri,"/api/scene/run")) {
        const char *const allowed[]={"id",NULL};
        if(keys(o,allowed))failure=app_start_scene(scene_index(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"id"))),false,0);
    } else if(!strcmp(uri,"/api/scene/stop") && cJSON_IsObject(o) && !o->child) { failure=app_stop_scene(); }
    else if (!strcmp(uri,"/api/reboot") && cJSON_IsObject(o) && !o->child) {
        app_lock();failure=app.updating?"update_busy":NULL;if(!failure)app.restart_ms=app_now()+2000;app_unlock();
    }
    if(!strcmp(uri,"/api/ble"))failure=configure(ble_configure,o);
    return failure;
}
// Query one entity or at most four inventory entries. Never return stored Wi-Fi credentials.
cJSON *app_query(const char *op,const cJSON *args,const char **failure)
{
    cJSON *o=NULL;uint64_t now=app_now();
    const char *const page_keys[]={"offset",NULL},*const id_keys[]={"id",NULL},*const no_keys[]={NULL};
    int offset=0;
    bool list=!strncmp(op,"list_",5),single=!strcmp(op,"get_light") || !strcmp(op,"get_group") || !strcmp(op,"get_scene");
    if(!keys(args,list?page_keys:single?id_keys:no_keys)
        || (list && cJSON_HasObjectItem(args,"offset") && !json_integer(cJSON_GetObjectItemCaseSensitive(args,"offset"),0,MAX_LIGHTS+1,&offset))) {
        *failure="invalid_query";return NULL;
    }
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args,"id"));
    if(single && !id) { *failure="invalid_query";return NULL; }
    app_lock();bool link=app.c6_last_ms && now-app.c6_last_ms<6000 && app.c6_ready;
    if(list) {
        bool lights=!strcmp(op,"list_lights"),groups=!strcmp(op,"list_groups"),scenes=!strcmp(op,"list_scenes");
        if(!lights && !groups && !scenes) { *failure="unknown_operation";goto done; }
        o=cJSON_CreateObject();cJSON *items=cJSON_AddArrayToObject(o,"items");int seen=0,added=0;
        int count=lights?MAX_LIGHTS+1:groups?MAX_GROUPS:MAX_SCENES;
        for(int i=0;i<count;i++) {
            bool screen=lights && i==MAX_LIGHTS;
            if(lights?(!screen && !app.lights[i].known):groups?!app.config.groups[i].used:!app.scenes.items[i].used)continue;
            if(seen++<offset)continue;
            if(added==4) { cJSON_AddNumberToObject(o,"next",seen-1);break; }
            cJSON *p=cJSON_CreateObject();char key[8];
            if(!lights)snprintf(key,sizeof(key),"%c%d",groups?'g':'s',i+1);
            const char *identity=lights?(screen?"display":app.lights[i].id):key;
            cJSON_AddStringToObject(p,"id",identity);
            cJSON_AddStringToObject(p,"name",lights?app_name(identity):groups?app.config.groups[i].name:app.scenes.items[i].name);
            if(lights)cJSON_AddBoolToObject(p,"online",screen || (link && app.lights[i].online));
            cJSON_AddItemToArray(items,p);added++;
        }
        if(!cJSON_HasObjectItem(o,"next"))cJSON_AddNullToObject(o,"next");
    } else if(!strcmp(op,"get_light")) {
        light_t *l=app_light(id);
        if(!strcmp(id,"display"))o=screen_json();
        else if(l)o=light_json(l,link,now);else *failure="unknown_light";
    } else if(!strcmp(op,"get_group")) {
        int n=id[0]=='g' && id[1]>='1' && id[1]<='8' && !id[2]?id[1]-'1':-1;
        if(n<0 || !app.config.groups[n].used)*failure="unknown_group";
        else {
            group_t *g=&app.config.groups[n];o=cJSON_CreateObject();cJSON_AddStringToObject(o,"id",id);cJSON_AddStringToObject(o,"name",g->name);
            cJSON *members=cJSON_AddArrayToObject(o,"members");
            for(unsigned i=0;i<g->count;i++)cJSON_AddItemToArray(members,cJSON_CreateString(g->members[i]));
        }
    } else if(!strcmp(op,"get_scene")) {
        int n=scene_index(id);if(n<0 || !app.scenes.items[n].used)*failure="unknown_scene";else o=scene_json(&app.scenes.items[n],n);
    } else if(!strcmp(op,"get_timers")) {
        o=cJSON_CreateObject();cJSON_AddItemToObject(o,"items",timers_json(now));
    } else if(!strcmp(op,"get_network_status")) {
        o=cJSON_CreateObject();cJSON_AddBoolToObject(o,"staConnected",app.sta_connected);cJSON_AddStringToObject(o,"staIp",app.sta_ip);
        cJSON_AddStringToObject(o,"staSsid",app.sta_ssid);cJSON_AddBoolToObject(o,"apActive",app.ap_active);
        cJSON_AddStringToObject(o,"hostname",app.hostname);cJSON_AddStringToObject(o,"alias",app.runtime_alias);cJSON_AddBoolToObject(o,"coordinator",link);
        cJSON_AddBoolToObject(o,"updating",app.updating);
    } else if(!strcmp(op,"get_remote")) {
        o=cJSON_CreateObject();cJSON_AddBoolToObject(o,"irEnabled",app.ir.enabled);cJSON_AddStringToObject(o,"irTarget",app.ir.target);
        cJSON_AddNumberToObject(o,"whiteKelvin",app.scenes.white_kelvin);cJSON_AddNumberToObject(o,"whiteLevel",app.scenes.white_level);
        cJSON_AddNumberToObject(o,"brightnessSteps",app.remote.count);cJSON_AddBoolToObject(o,"customBrightness",app.remote.custom);
        cJSON *levels=cJSON_AddArrayToObject(o,"brightnessLevels"),*members=cJSON_AddArrayToObject(o,"irMembers"),*buttons=cJSON_AddArrayToObject(o,"buttons");
        for(unsigned i=0;i<app.remote.count;i++)cJSON_AddItemToArray(levels,cJSON_CreateNumber(app.remote.percent[i]));
        for(unsigned i=0;i<app.remote.member_count;i++)cJSON_AddItemToArray(members,cJSON_CreateString(app.remote.members[i]));
        for(int i=0;i<4;i++) { char key[16];snprintf(key,sizeof(key),"s%d",app.scenes.buttons[i]+1);cJSON_AddItemToArray(buttons,app.scenes.buttons[i]<0?cJSON_CreateNull():cJSON_CreateString(key)); }
    } else *failure="unknown_operation";
done:
    if(o)cJSON_AddNumberToObject(o,"rev",app.state_revision);
    app_unlock();return o;
}
static esp_err_t ble_get(httpd_req_t *r) { return reply(r,"200 OK",ble_status()); }
static esp_err_t post(httpd_req_t *r)
{
    if (!same_origin(r)) return error(r,"403 Forbidden","origin_rejected");
    if(!strcmp(r->uri,"/api/update/gateway") || !strcmp(r->uri,"/api/update/coordinator")) {
        char type[80];if(httpd_req_get_hdr_value_str(r,"Content-Type",type,sizeof(type))!=ESP_OK || strcmp(type,"application/octet-stream"))return error(r,"415 Unsupported Media Type","update_binary_required");
        return updates_upload(r);
    }
    app_lock();bool updating=app.updating;app_unlock();
    if(updating && strcmp(r->uri,"/api/update/cancel"))return error(r,"409 Conflict","update_busy");
    char content_type[80];
    if (httpd_req_get_hdr_value_str(r,"Content-Type",content_type,sizeof(content_type))!=ESP_OK || strncmp(content_type,"application/json",16))
        return error(r,"415 Unsupported Media Type","json_required");
    if (r->content_len<=0 || r->content_len>4096) return error(r,"413 Payload Too Large","invalid_body_size");
    char *body=calloc(1,r->content_len+1); if (!body) return error(r,"500 Internal Server Error","memory");
    int read=0;
    while (read<r->content_len) {
        int n=httpd_req_recv(r,body+read,r->content_len-read);
        if (n<=0) { free(body); return error(r,"400 Bad Request","incomplete_body"); } read+=n;
    }
    const char *end=NULL;
    cJSON *o=memchr(body,0,r->content_len)?NULL:cJSON_ParseWithOpts(body,&end,true); free(body);
    if (!o) return error(r,"400 Bad Request","invalid_json");
    const char *failure=app_api(r->uri,o);
    cJSON_Delete(o);
    return failure?error(r,"400 Bad Request",failure):success(r);
}
void web_start(void)
{
    httpd_config_t config=HTTPD_DEFAULT_CONFIG(); config.stack_size=16384; config.max_uri_handlers=10;
    config.uri_match_fn=httpd_uri_match_wildcard; config.lru_purge_enable=true;
    config.recv_wait_timeout=3; config.send_wait_timeout=3;
    httpd_handle_t server=NULL; ESP_ERROR_CHECK(httpd_start(&server,&config));
    httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=assets}, {.uri="/app.js",.method=HTTP_GET,.handler=assets},
        {.uri="/style.css",.method=HTTP_GET,.handler=assets}, {.uri="/api/state",.method=HTTP_GET,.handler=state_get},
        {.uri="/api/settings",.method=HTTP_GET,.handler=settings_get}, {.uri="/api/scenes",.method=HTTP_GET,.handler=scenes_get},
        {.uri="/api/ble",.method=HTTP_GET,.handler=ble_get},
        {.uri="/api/update/status",.method=HTTP_GET,.handler=updates_status},
        {.uri="/api/*",.method=HTTP_POST,.handler=post},
    };
    for (unsigned i=0;i<sizeof(routes)/sizeof(routes[0]);i++) ESP_ERROR_CHECK(httpd_register_uri_handler(server,&routes[i]));
}
