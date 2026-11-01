#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "mdns.h"

static EventGroupHandle_t events;
static bool started,accept_ip,failed;
#define RADIO_STOPPED BIT0
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    if (base==WIFI_EVENT && (id==WIFI_EVENT_STA_STOP || id==WIFI_EVENT_AP_STOP))
        xEventGroupSetBits(events,RADIO_STOPPED);
    app_lock();
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        app.sta_connected=false; app.sta_ip[0]=0; failed=true;
    }
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP && accept_ip) {
        const ip_event_got_ip_t *event=data;
        app.sta_connected=true; app.wifi_error[0]=0;
        snprintf(app.sta_ip,sizeof(app.sta_ip),IPSTR,IP2STR(&event->ip_info.ip));
        ESP_LOGI("network","Connected; web=http://%s",app.sta_ip);
    }
    if (base==WIFI_EVENT && (id==WIFI_EVENT_AP_STACONNECTED || id==WIFI_EVENT_AP_STADISCONNECTED)) {
        wifi_sta_list_t stations;
        if (esp_wifi_ap_get_sta_list(&stations)==ESP_OK) app.ap_clients=stations.num;
    }
    app_unlock();
}
static void radio_stop(void)
{
    app_lock(); accept_ip=false; app.sta_connected=false; app.sta_ip[0]=0; app.ap_active=false; app.ap_clients=0; app_unlock();
    if (!started) return;
    xEventGroupClearBits(events,RADIO_STOPPED);
    ESP_ERROR_CHECK(esp_wifi_stop());
    // Drain the previous interface's disconnect events before accepting the next DHCP event.
    xEventGroupWaitBits(events,RADIO_STOPPED,pdTRUE,pdFALSE,pdMS_TO_TICKS(1500));
    started=false;
}
static esp_err_t start_ap(void)
{
    wifi_config_t cfg={.ap={.channel=1,.max_connection=4,.authmode=WIFI_AUTH_WPA2_PSK}};
    memcpy(cfg.ap.ssid,app.runtime_ssid,strlen(app.runtime_ssid)); cfg.ap.ssid_len=strlen(app.runtime_ssid);
    snprintf((char *)cfg.ap.password,sizeof(cfg.ap.password),"%s",app.runtime_password);
    esp_err_t err=esp_wifi_set_mode(WIFI_MODE_AP);
    if (err==ESP_OK) err=esp_wifi_set_config(WIFI_IF_AP,&cfg);
    if (err==ESP_OK) { err=esp_wifi_start(); started=err==ESP_OK; }
    return err;
}
static esp_err_t start_station(const network_t *network)
{
    wifi_config_t cfg={0};
    memcpy(cfg.sta.ssid,network->ssid,strlen(network->ssid));
    memcpy(cfg.sta.password,network->password,strlen(network->password));
    cfg.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    cfg.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
    esp_err_t err=esp_wifi_set_mode(WIFI_MODE_STA);
    if (err==ESP_OK) err=esp_wifi_set_config(WIFI_IF_STA,&cfg);
    if (err==ESP_OK) { err=esp_wifi_start(); started=err==ESP_OK; }
    if (err==ESP_OK) {
        app_lock(); accept_ip=true; failed=false; app_unlock();
        err=esp_wifi_connect();
    }
    return err;
}
static void wifi_task(void *unused)
{
    unsigned revision=UINT_MAX,index=0;
    uint64_t due=0,began=0;
    bool attempting=false,was_connected=false;
    for (;;) {
        uint64_t now=app_now();
        app_lock();
        unsigned wanted_revision=app.wifi_revision,count=app.options.network_count;
        bool ap=app.options.ap_enabled,active=app.ap_active,connected=app.sta_connected,disconnected=failed;
        network_t network=app.options.networks[index< count?index:0];
        app_unlock();
        if (revision!=wanted_revision) {
            revision=wanted_revision; index=0; attempting=false; was_connected=false; due=0;
            // Editing station priorities over the hotspot must not disconnect its web client.
            if (ap && active && started) continue;
            radio_stop();
            app_lock(); app.sta_ssid[0]=0; app.wifi_error[0]=0; app_unlock();
            ESP_LOGI("network","Mode: %s; saved networks: %u",ap?"manual AP":"station only (no AP fallback)",count);
            continue;
        }
        if (ap) {
            if (!started && now>=due) {
                esp_err_t err=start_ap();
                app_lock(); app.ap_active=err==ESP_OK;
                if (err!=ESP_OK) snprintf(app.wifi_error,sizeof(app.wifi_error),"AP: %s",esp_err_to_name(err));
                app_unlock(); due=now+5000;
            }
        } else if (connected) was_connected=true;
        else if (was_connected) {
            radio_stop(); was_connected=false; attempting=false; index=0; due=now+250;
        } else if (attempting && ((disconnected && now-began>=500) || now-began>=15000)) {
            radio_stop(); attempting=false; index=(index+1)%count; due=now+(index?250:5000);
            app_lock(); snprintf(app.wifi_error,sizeof(app.wifi_error),"Sit nedostupna; zkousim dalsi"); app_unlock();
        } else if (!attempting && count && now>=due) {
            radio_stop();
            app_lock(); snprintf(app.sta_ssid,sizeof(app.sta_ssid),"%s",network.ssid); app_unlock();
            ESP_LOGI("network","Trying saved network %u/%u",index+1,count);
            esp_err_t err=start_station(&network);
            attempting=true; began=app_now();
            if (err!=ESP_OK) {
                app_lock(); failed=true; snprintf(app.wifi_error,sizeof(app.wifi_error),"Wi-Fi: %s",esp_err_to_name(err)); app_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
void wifi_start(void)
{
    snprintf(app.runtime_alias,sizeof(app.runtime_alias),"%s",app.alias);
    snprintf(app.hostname,sizeof(app.hostname),"%s",app.alias);
    for (char *p=app.hostname;*p;p++) if (*p>='A' && *p<='Z') *p+='a'-'A';
    snprintf(app.runtime_ssid,sizeof(app.runtime_ssid),"%s",app.config.ap_ssid);
    snprintf(app.runtime_password,sizeof(app.runtime_password),"%s",app.config.ap_password);
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap=esp_netif_create_default_wifi_ap(),*sta=esp_netif_create_default_wifi_sta();
    assert(ap && sta);
    ESP_ERROR_CHECK(esp_netif_set_hostname(ap,app.hostname));
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta,app.hostname));
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // No AP is started, even briefly, until the physical switch has enabled it.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    events=xEventGroupCreate(); assert(events);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(app.hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(app.runtime_alias));
    ESP_ERROR_CHECK(mdns_service_add(NULL,"_http","_tcp",80,NULL,0));
    ESP_LOGI("network","Alias=%s; hostname=%s; web=http://%s.local",app.runtime_alias,app.hostname,app.hostname);
    assert(xTaskCreate(wifi_task,"networks",4096,NULL,4,NULL)==pdPASS);
}
