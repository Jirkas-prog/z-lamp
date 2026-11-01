#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "api.h"
#include "ble.h"
#include "ble_rpc.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// Keep the host's compile-time policy aligned with sm_mitm=0 / Just Works.
// Level 3/4 rejects a peer's no-MITM request before our GAP callback can admit it.
#if CONFIG_BT_NIMBLE_SM_LVL != 2 || !CONFIG_BT_NIMBLE_SM_SC || defined(CONFIG_BT_NIMBLE_SM_LEGACY)
#error "Z-lamp requires encrypted Just Works with Secure Connections and no legacy pairing"
#endif
// IDF 5.3.1's SC_ONLY branch accepts a valid request without sending a response.
// SC is already enforced by disabling legacy pairing; keep the working path.
#if MYNEWT_VAL(BLE_SM_SC_ONLY)
#error "SC_ONLY stalls pairing in this NimBLE version; disable legacy pairing instead"
#endif

// No UART, application actions or application NVS writes run in host callbacks.
static SemaphoreHandle_t mutex,host_done,indication_done;
static nvs_handle_t prefs;
static bool storage_ready,worker_available;
static bool enabled=true,running,starting,ready,paused,stopping,subscribed,authenticated,trusted_peer,busy,forget;
static uint16_t connection=BLE_HS_CONN_HANDLE_NONE,tx_handle;
static uint32_t generation;
static unsigned bonds,worker_stack,host_stack;
static unsigned connect_events;
static int connect_status=-1,disconnect_reason=-1,security_status=-1,passkey_action=-1;
static const char *security_stage="idle";
static const char *termination_cause="";
static int pairing_status=-1,link_encrypted=-1,link_bonded=-1,link_key_size=-1;
static uint32_t disconnected_after_ms;
static struct { uint32_t ms; const char *event; int code; } history[8];
static unsigned history_count,repeat_pairings;
static bool known_on_connect;
static int last_store_status;
static int (*persist_write)(int,const union ble_store_value *);
static char local_address[18];
static uint8_t own_addr_type;
static uint64_t help_until,connected_at;
static bool physical_pair,previous_awake;
static uint64_t previous_touch;
static char device_name[33],info[320],last_error[40];
static ble_message_t incoming;
static ble_rpc_t rpc;
static int indication_result;
void ble_store_config_init(void);
#define LOCK() xSemaphoreTake(mutex,portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mutex)
#define UUID(last) BLE_UUID128_INIT(last,0xa0,0xa2,0xe2,0xdb,0x31,0x6c,0x9d,0x7b,0x4d,0x98,0xe3,0xd0,0x3a,0xf8,0xbc)
static const ble_uuid128_t service_uuid=UUID(1),info_uuid=UUID(2),rx_uuid=UUID(3),tx_uuid=UUID(4);
static int gap_event(struct ble_gap_event *event,void *arg);
static void close_pair_help(void);

static void set_error(const char *error)
{ LOCK();snprintf(last_error,sizeof(last_error),"%s",error?error:"");UNLOCK(); }
static void trace(const char *event,int code)
{
    LOCK();unsigned i=history_count++%8;history[i].ms=(uint32_t)app_now();history[i].event=event;history[i].code=code;UNLOCK();
}
static int store_write(int type,const union ble_store_value *value)
{
    int rc=persist_write(type,value);
    if(rc || type==BLE_STORE_OBJ_TYPE_OUR_SEC || type==BLE_STORE_OBJ_TYPE_PEER_SEC) {
        LOCK();last_store_status=rc;UNLOCK();trace(rc?"key_store_error":"keys_saved",rc);
    }
    return rc;
}
static int store_status(struct ble_store_status_event *event,void *arg)
{ trace("store_capacity",event->event_code);return ble_store_util_status_rr(event,arg); }
static void terminate_connection(uint16_t conn,const char *cause)
{
    // ENOTCONN can arrive while tearing down and must not erase the original cause.
    LOCK();if(conn==connection && !termination_cause[0])termination_cause=cause;UNLOCK();
    ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);
}
static bool peer_bonded(const ble_addr_t *address)
{
    ble_addr_t peers[4];int count=0;
    if(ble_store_util_bonded_peers(peers,&count,4))return false;
    LOCK();bonds=count;UNLOCK();
    for(int i=0;i<count;i++)if(!ble_addr_cmp(address,&peers[i]))return true;
    return false;
}
static int access_gatt(uint16_t handle,uint16_t attribute,struct ble_gatt_access_ctxt *ctx,void *arg)
{
    if(arg==(void *)1)return os_mbuf_append(ctx->om,info,strlen(info))?BLE_ATT_ERR_INSUFFICIENT_RES:0;
    if(arg!=(void *)2 || ctx->op!=BLE_GATT_ACCESS_OP_WRITE_CHR)return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    struct ble_gap_conn_desc desc;
    if(ble_gap_conn_find(handle,&desc) || !desc.sec_state.encrypted || !desc.sec_state.bonded || desc.sec_state.key_size!=16)return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t bytes[247];uint16_t size=0;
    if(OS_MBUF_PKTLEN(ctx->om)>sizeof(bytes) || ble_hs_mbuf_to_flat(ctx->om,bytes,sizeof(bytes),&size))return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    LOCK();
    int result=BLE_ATT_ERR_INSUFFICIENT_RES;
    if(handle==connection && authenticated && !paused && !stopping && !busy && subscribed) {
        int state=ble_message_feed(&incoming,bytes,size,app_now());
        result=state<0?BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN:0;
        if(state==1)busy=true;
    }
    UNLOCK();return result;
}
static const struct ble_gatt_svc_def services[]={
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service_uuid.u,.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=&info_uuid.u,.access_cb=access_gatt,.arg=(void *)1,.flags=BLE_GATT_CHR_F_READ},
        {.uuid=&rx_uuid.u,.access_cb=access_gatt,.arg=(void *)2,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_ENC,.min_key_size=16},
        {.uuid=&tx_uuid.u,.access_cb=access_gatt,.flags=BLE_GATT_CHR_F_INDICATE,.val_handle=&tx_handle},
        {0}}},
    {0}
};
static void advertise(void)
{
    LOCK();bool allowed=ready && !stopping && !paused && !forget && enabled && connection==BLE_HS_CONN_HANDLE_NONE;
    UNLOCK();
    if(!allowed || ble_gap_adv_active())return;
    struct ble_hs_adv_fields fields={0},scan={0};
    fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128=(ble_uuid128_t *)&service_uuid;fields.num_uuids128=1;fields.uuids128_is_complete=1;
    scan.name=(uint8_t *)device_name;scan.name_len=strlen(device_name);if(scan.name_len>29)scan.name_len=29;
    scan.name_is_complete=scan.name_len==strlen(device_name);
    struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN,.itvl_min=320,.itvl_max=480};
    if(ble_gap_adv_set_fields(&fields) || ble_gap_adv_rsp_set_fields(&scan) || ble_gap_adv_start(own_addr_type,NULL,BLE_HS_FOREVER,&params,gap_event,NULL))set_error("ble_advertising");
}
static void on_sync(void)
{
    if(ble_hs_util_ensure_addr(0) || ble_hs_id_infer_auto(0,&own_addr_type)) { set_error("ble_address");return; }
    ble_addr_t peers[4];int count=0;ble_store_util_bonded_peers(peers,&count,4);
    LOCK();ready=true;bonds=count;UNLOCK();
    trace("bonds_loaded",count);
}
static void on_reset(int reason)
{ LOCK();ready=false;authenticated=false;subscribed=false;connection=BLE_HS_CONN_HANDLE_NONE;generation++;UNLOCK();trace("host_reset",reason);set_error("ble_host_reset"); }
static int gap_event(struct ble_gap_event *event,void *arg)
{
    if(!strcmp(pcTaskGetName(NULL),"ble_host")) { LOCK();host_stack=uxTaskGetStackHighWaterMark(NULL);UNLOCK(); }
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        LOCK();connect_events++;connect_status=event->connect.status;security_status=-1;passkey_action=-1;security_stage="connecting";UNLOCK();
        if(!event->connect.status) {
            struct ble_gap_conn_desc desc;
            if(ble_gap_conn_find(event->connect.conn_handle,&desc))return 0;
            bool known=peer_bonded(&desc.peer_id_addr);
            LOCK();known_on_connect=known;UNLOCK();trace("known_peer",known);
            LOCK();bool allowed=enabled && !paused && !stopping && !forget;
            connection=event->connect.conn_handle;generation++;incoming.received=0;busy=false;subscribed=false;authenticated=false;trusted_peer=known;connected_at=app_now();
            termination_cause="";pairing_status=link_encrypted=link_bonded=link_key_size=-1;disconnected_after_ms=0;UNLOCK();
            if(!allowed) {
                LOCK();security_stage="ble_unavailable";UNLOCK();
                terminate_connection(event->connect.conn_handle,"ble_unavailable");
            } else {
                int rc=ble_gap_security_initiate(event->connect.conn_handle);
                LOCK();security_stage="security_request";security_status=rc;UNLOCK();
            }
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        trace("disconnect",event->disconnect.reason);
        LOCK();disconnect_reason=event->disconnect.reason;disconnected_after_ms=app_now()-connected_at;connection=BLE_HS_CONN_HANDLE_NONE;generation++;incoming.received=0;incoming.total=0;busy=false;subscribed=false;authenticated=false;UNLOCK();
        xSemaphoreGive(indication_done);return 0;
    case BLE_GAP_EVENT_PARING_COMPLETE:
        LOCK();pairing_status=event->pairing_complete.status;UNLOCK();return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        trace("encryption",event->enc_change.status);
        struct ble_gap_conn_desc desc;bool found=!ble_gap_conn_find(event->enc_change.conn_handle,&desc);
        LOCK();if(found) { link_encrypted=desc.sec_state.encrypted;link_bonded=desc.sec_state.bonded;link_key_size=desc.sec_state.key_size; }UNLOCK();
        bool valid=!event->enc_change.status && found
            && desc.sec_state.encrypted && desc.sec_state.bonded && desc.sec_state.key_size==16;
        LOCK();bool admitted=enabled && !paused && !stopping && !forget;UNLOCK();
        valid=valid && admitted;
        LOCK();authenticated=valid;security_stage=valid?"authenticated":"encryption_failed";security_status=event->enc_change.status;UNLOCK();
        if(valid)peer_bonded(&desc.peer_id_addr);
        else if(found)terminate_connection(event->enc_change.conn_handle,event->enc_change.status?"security_error":"security_requirements");
        return 0;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if(event->subscribe.attr_handle==tx_handle) { LOCK();subscribed=event->subscribe.cur_indicate;UNLOCK(); }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if(event->notify_tx.indication && event->notify_tx.attr_handle==tx_handle && event->notify_tx.status) {
            LOCK();indication_result=event->notify_tx.status;UNLOCK();xSemaphoreGive(indication_done);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        LOCK();repeat_pairings++;UNLOCK();trace("repeat_pairing",0);
        LOCK();bool allow=enabled && !paused && !stopping && !forget;UNLOCK();
        if(!allow)return BLE_GAP_REPEAT_PAIRING_IGNORE;
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle,&desc))return BLE_GAP_REPEAT_PAIRING_IGNORE;
        if(ble_store_util_delete_peer(&desc.peer_id_addr))return BLE_GAP_REPEAT_PAIRING_IGNORE;
        LOCK();trusted_peer=false;UNLOCK();return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        // Just Works has no PIN input/output action. Reject unexpected downgrade paths.
        LOCK();passkey_action=event->passkey.params.action;UNLOCK();
        terminate_connection(event->passkey.conn_handle,"unexpected_pin_action");return 0;
    }
    default:return 0;
    }
}
static void host(void *arg)
{ nimble_port_run();xSemaphoreGive(host_done);vTaskDelete(NULL); }
static bool start_host(void)
{
    if(nimble_port_init()!=ESP_OK) { set_error("ble_memory");return false; }
    ble_hs_cfg.sync_cb=on_sync;ble_hs_cfg.reset_cb=on_reset;
    ble_hs_cfg.sm_io_cap=BLE_HS_IO_NO_INPUT_OUTPUT;ble_hs_cfg.sm_bonding=1;ble_hs_cfg.sm_mitm=0;ble_hs_cfg.sm_sc=1;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();ble_svc_gatt_init();ble_store_config_init();
    persist_write=ble_hs_cfg.store_write_cb;ble_hs_cfg.store_write_cb=store_write;
    // New phones can always enroll; rotate the oldest bond when the four slots fill.
    ble_hs_cfg.store_status_cb=store_status;
    if(ble_svc_gap_device_name_set(device_name) || ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) {
        nimble_port_deinit();set_error("ble_services");return false;
    }
    xSemaphoreTake(host_done,0);
    LOCK();stopping=false;running=true;UNLOCK();
    if(xTaskCreatePinnedToCore(host,"ble_host",5120,NULL,configMAX_PRIORITIES-4,NULL,1)!=pdPASS) {
        nimble_port_deinit();LOCK();running=false;UNLOCK();set_error("ble_memory");return false;
    }
    set_error(NULL);return true;
}
static void disconnect_session(uint32_t session)
{
    LOCK();uint16_t conn=generation==session?connection:BLE_HS_CONN_HANDLE_NONE;UNLOCK();
    if(conn!=BLE_HS_CONN_HANDLE_NONE)terminate_connection(conn,"command_transport");
}
static bool stop_host(void)
{
    LOCK();stopping=true;ready=false;UNLOCK();
    int rc=nimble_port_stop();
    if(rc || xSemaphoreTake(host_done,pdMS_TO_TICKS(2000))!=pdTRUE || nimble_port_deinit()!=ESP_OK) {
        set_error("ble_stop_failed");return false;
    }
    LOCK();running=false;stopping=false;connection=BLE_HS_CONN_HANDLE_NONE;generation++;busy=false;authenticated=false;subscribed=false;incoming.received=0;UNLOCK();
    return true;
}
static bool send_message(const char *message,uint32_t session)
{
    size_t total=strlen(message),offset=0;
    while(offset<total) {
        LOCK();uint16_t conn=connection;bool valid=generation==session && authenticated && subscribed && !paused && !stopping;UNLOCK();
        if(!valid)return false;
        uint8_t frame[247];size_t n=ble_message_frame(message,total,offset,ble_att_mtu(conn),frame,sizeof(frame));
        if(!n)return false;
        xSemaphoreTake(indication_done,0);LOCK();indication_result=0;UNLOCK();
        struct os_mbuf *mbuf=ble_hs_mbuf_from_flat(frame,n);
        if(!mbuf || ble_gatts_indicate_custom(conn,tx_handle,mbuf))return false;
        bool ack=false;uint64_t until=app_now()+5000;
        while(app_now()<until) {
            if(xSemaphoreTake(indication_done,pdMS_TO_TICKS(100))==pdTRUE) {
                LOCK();ack=generation==session && indication_result==BLE_HS_EDONE;UNLOCK();break;
            }
            LOCK();valid=generation==session && !paused;UNLOCK();if(!valid)break;
        }
        if(!ack)return false;
        offset+=n-4;
    }
    return true;
}
static cJSON *dispatch(const char *op,const cJSON *args,const char **error,void *unused)
{
    if(!strcmp(op,"get_info")) {
        if(args->child) { *error="invalid_query";return NULL; }
        LOCK();bool enrolled=authenticated && !trusted_peer;uint16_t conn=connection;UNLOCK();
        if(enrolled) {
            // NimBLE delivers ENC_CHANGE before persisting the keys; refresh the count after that step.
            struct ble_gap_conn_desc desc;
            if(ble_gap_conn_find(conn,&desc) || !peer_bonded(&desc.peer_id_addr)) { *error="ble_storage";return NULL; }
            LOCK();if(conn==connection)trusted_peer=true;UNLOCK();close_pair_help();
        }
        return cJSON_Parse(info);
    }
    if(!strncmp(op,"get_",4) || !strncmp(op,"list_",5))return app_query(op,args,error);
    const char *const allowed[]={"timer","arrival","control","group","scenes","scene/run","scene/stop","rename","device","startup","pair","remove","identify","remote",NULL};
    bool found=false;for(int i=0;allowed[i];i++)if(!strcmp(op,allowed[i]))found=true;
    if(!found) { *error="unknown_operation";return NULL; }
    char uri[48];snprintf(uri,sizeof(uri),"/api/%s",op);*error=app_api(uri,args);
    cJSON *out=cJSON_CreateObject();app_lock();cJSON_AddNumberToObject(out,"rev",app.state_revision);app_unlock();return out;
}
static void close_pair_help(void)
{
    // If both are needed, take the BLE mutex before app_mutex; callbacks never wait for application work.
    LOCK();
    help_until=0;
    bool physical=physical_pair,awake=previous_awake;uint64_t touched=previous_touch;physical_pair=false;
    app_lock();app.ble_help_until=0;
    if(physical) { app.screen.awake=awake;app.screen.touched_ms=touched; }
    app_unlock();UNLOCK();
}
static void worker(void *unused)
{
    char request[BLE_MESSAGE_MAX+1],scratch[BLE_RPC_ERROR_MAX];uint32_t cache_generation=0,last_revision=0;
    uint64_t retry=0,last_event=0,last_command=0;
    for(;;) {
        LOCK();worker_stack=uxTaskGetStackHighWaterMark(NULL);
        bool active=running,want=enabled && !paused,clear=forget,help_done=help_until && app_now()>=help_until;
        uint16_t conn=connection;bool expired=conn!=BLE_HS_CONN_HANDLE_NONE && !authenticated && app_now()-connected_at>30000;UNLOCK();
        if(help_done || !want)close_pair_help();
        if(active && !want) { if(stop_host())retry=0;else vTaskDelay(pdMS_TO_TICKS(1000)); }
        else if(!active && want && app_now()>=retry) {
            LOCK();bool start=enabled && !paused;if(start)starting=true;UNLOCK();
            if(start && !start_host())retry=app_now()+30000;
            LOCK();starting=false;UNLOCK();
        }
        else if(active && want) {
            LOCK();bool synced=ready;UNLOCK();
            if(synced && clear) {
                // Explicit "forget all" stops connections before removing every bond.
                ble_gap_adv_stop();
                if(conn!=BLE_HS_CONN_HANDLE_NONE)terminate_connection(conn,"forget_bonds");
                else if(!ble_store_clear()) { LOCK();forget=false;bonds=0;UNLOCK(); }
                else set_error("ble_forget_failed");
            }
            if(expired) {
                LOCK();security_stage="authentication_timeout";UNLOCK();
                terminate_connection(conn,"pairing_timeout");
            }
            if(synced && !clear)advertise();
            LOCK();uint32_t session=generation;bool command=busy && incoming.total && incoming.received==incoming.total;
            if(command) { memcpy(request,incoming.data,incoming.total+1);incoming.received=incoming.total=0; }
            bool can_send=authenticated && subscribed;UNLOCK();
            if(cache_generation!=session) { memset(&rpc,0,sizeof(rpc));cache_generation=session;last_revision=0; }
            if(command && can_send) {
                uint64_t elapsed=app_now()-last_command;
                if(elapsed<100)vTaskDelay(pdMS_TO_TICKS(100-elapsed)+1);
                LOCK();bool valid=generation==session && authenticated && enabled && !paused && !forget;UNLOCK();
                if(valid) {
                    const char *reply=ble_rpc_execute(&rpc,request,scratch,dispatch,NULL);last_command=app_now();
                    // The peer can send its next request as soon as it receives the
                    // reply, before our indication ACK callback runs. Free the inbox
                    // now; the worker still serializes execution and reply delivery.
                    LOCK();if(generation==session)busy=false;UNLOCK();
                    if(!send_message(reply,session))disconnect_session(session);
                }
            } else if(command) {
                disconnect_session(session);
            } else if(can_send && rpc.last_id && app_now()-last_event>=250) {
                // A restored bonded CCCD can precede the app's notification handler.
                // Wait for its first request so it cannot join an event mid-fragment.
                app_lock();uint32_t revision=app.state_revision;app_unlock();
                if(revision!=last_revision) {
                    snprintf(scratch,sizeof(scratch),"{\"v\":1,\"event\":\"changed\",\"rev\":%lu}",(unsigned long)revision);
                    if(send_message(scratch,session))last_revision=revision;else disconnect_session(session);
                }
                last_event=app_now();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
const char *ble_pair(bool physical)
{
    app_lock();bool updating=app.updating;app_unlock();if(updating)return "update_busy";
    LOCK();
    if(!storage_ready) { UNLOCK();return "ble_storage"; }
    if(!worker_available) { UNLOCK();return "ble_memory_restart_required"; }
    if(paused || stopping) { UNLOCK();return "ble_paused"; }
    if(nvs_set_u8(prefs,"enabled",1)!=ESP_OK || nvs_commit(prefs)!=ESP_OK) { UNLOCK();return "ble_storage"; }
    enabled=true;
    app_lock();
    if(physical && !physical_pair) {
        previous_awake=app.screen.awake;previous_touch=app.screen.touched_ms;physical_pair=true;
        app.screen.awake=true;app.screen.touched_ms=app_now();
    }
    if(physical) { help_until=app_now()+30000;app.ble_help_until=help_until; }
    app_unlock();UNLOCK();
    return NULL;
}
bool ble_pause(bool pause)
{
    LOCK();paused=pause;UNLOCK();
    if(!pause)return true;
    uint64_t until=app_now()+20000;
    do { LOCK();bool stopped=!running && !starting;UNLOCK();if(stopped)return true;vTaskDelay(pdMS_TO_TICKS(50)); }while(app_now()<until);
    return false;
}
const char *ble_configure(const cJSON *o)
{
    if(!cJSON_IsObject(o) || !o->child || o->child->next)return "invalid_ble";
    const cJSON *p=o->child;
    if(!strcmp(p->string,"pair") && cJSON_IsTrue(p))return ble_pair(false);
    // Compatibility with an already-open older web page: this only hides LCD help.
    if(!strcmp(p->string,"pair") && cJSON_IsFalse(p)) { close_pair_help();return NULL; }
    if(!strcmp(p->string,"forgetAll") && cJSON_IsTrue(p)) {
        LOCK();bool available=running && ready && !paused;if(available)forget=true;UNLOCK();
        return available?NULL:"ble_disabled";
    }
    if(strcmp(p->string,"enabled") || !cJSON_IsBool(p))return "invalid_ble";
    bool value=cJSON_IsTrue(p);LOCK();
    if(!storage_ready) { UNLOCK();return "ble_storage"; }
    if(value && !worker_available) { UNLOCK();return "ble_memory_restart_required"; }
    esp_err_t err=nvs_set_u8(prefs,"enabled",value);if(err==ESP_OK)err=nvs_commit(prefs);
    if(err==ESP_OK)enabled=value;
    UNLOCK();return err==ESP_OK?NULL:"ble_storage";
}
cJSON *ble_status(void)
{
    cJSON *o=cJSON_CreateObject();LOCK();
    cJSON_AddBoolToObject(o,"enabled",enabled);cJSON_AddBoolToObject(o,"running",running && ready);cJSON_AddBoolToObject(o,"paused",paused);
    cJSON_AddBoolToObject(o,"connected",connection!=BLE_HS_CONN_HANDLE_NONE);cJSON_AddBoolToObject(o,"authenticated",authenticated);
    cJSON_AddNumberToObject(o,"bonds",bonds);cJSON_AddStringToObject(o,"error",last_error);
    cJSON_AddStringToObject(o,"address",local_address);cJSON_AddBoolToObject(o,"knownOnConnect",known_on_connect);
    cJSON_AddNumberToObject(o,"repeatPairings",repeat_pairings);cJSON_AddNumberToObject(o,"keyStoreStatus",last_store_status);
    cJSON *events=cJSON_AddArrayToObject(o,"history");
    for(unsigned n=history_count>8?history_count-8:0;n<history_count;n++) {
        unsigned i=n%8;cJSON *e=cJSON_CreateArray();cJSON_AddItemToArray(events,e);
        cJSON_AddItemToArray(e,cJSON_CreateNumber(history[i].ms));cJSON_AddItemToArray(e,cJSON_CreateString(history[i].event));cJSON_AddItemToArray(e,cJSON_CreateNumber(history[i].code));
    }
    cJSON_AddNumberToObject(o,"connectEvents",connect_events);cJSON_AddNumberToObject(o,"connectStatus",connect_status);
    cJSON_AddNumberToObject(o,"disconnectReason",disconnect_reason);cJSON_AddStringToObject(o,"securityStage",security_stage);
    cJSON_AddNumberToObject(o,"securityStatus",security_status);cJSON_AddNumberToObject(o,"passkeyAction",passkey_action);
    cJSON *security=cJSON_AddObjectToObject(o,"security");
    cJSON_AddNumberToObject(security,"pairingStatus",pairing_status);cJSON_AddStringToObject(security,"termination",termination_cause);
    cJSON_AddNumberToObject(security,"encrypted",link_encrypted);cJSON_AddNumberToObject(security,"bonded",link_bonded);
    cJSON_AddNumberToObject(security,"keySize",link_key_size);cJSON_AddNumberToObject(security,"disconnectedAfterMs",disconnected_after_ms);
    cJSON_AddNumberToObject(o,"workerStackFreeBytes",worker_stack);cJSON_AddNumberToObject(o,"hostStackFreeBytes",host_stack);
    cJSON_AddStringToObject(o,"name",device_name);cJSON_AddBoolToObject(o,"forgetPending",forget);
    cJSON_AddStringToObject(o,"pairingMode","always");
    UNLOCK();
    cJSON_AddNumberToObject(o,"freeHeap",heap_caps_get_free_size(MALLOC_CAP_8BIT));cJSON_AddNumberToObject(o,"largestBlock",heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(o,"minimumHeap",heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));return o;
}
void ble_start(void)
{
    mutex=xSemaphoreCreateMutex();host_done=xSemaphoreCreateBinary();indication_done=xSemaphoreCreateBinary();assert(mutex && host_done && indication_done);
    uint8_t value=1;esp_err_t err=nvs_open("lamp_ble",NVS_READWRITE,&prefs);
    if(err==ESP_OK) { err=nvs_get_u8(prefs,"enabled",&value);storage_ready=(err==ESP_OK || err==ESP_ERR_NVS_NOT_FOUND) && value<=1; }
    enabled=storage_ready && value!=0;
    if(!storage_ready)set_error("ble_storage");
    app_lock();snprintf(device_name,sizeof(device_name),"%s",app.runtime_alias);app_unlock();
    uint8_t mac[6];ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_BT));
    snprintf(local_address,sizeof(local_address),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(info,sizeof(info),"{\"v\":1,\"alias\":\"%s\",\"version\":\"%s\",\"deviceId\":\"%02x%02x%02x%02x%02x%02x\",\"maxMessage\":1536,\"pageSize\":4,\"ota\":\"wifi\",\"arrival\":1,\"timers\":1}",
        device_name,esp_app_get_description()->version,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    worker_available=xTaskCreate(worker,"ble_commands",9216,NULL,2,NULL)==pdPASS;
    if(!worker_available) { enabled=false;set_error("ble_memory_restart_required"); }
}
