#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include "ble_rpc.h"

// Deterministic platform shims; parser, inbox checks and worker are production code.
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_ATT_ERR_WRITE_NOT_PERMITTED 3
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 5
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
struct os_mbuf { const uint8_t *data; uint16_t size; };
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
struct ble_gap_conn_desc { struct { bool encrypted,bonded; int key_size; } sec_state; };
#define OS_MBUF_PKTLEN(m) ((m)->size)
static int os_mbuf_append(struct os_mbuf *m,const void *data,size_t size) { return 0; }
static int ble_hs_mbuf_to_flat(struct os_mbuf *m,void *data,uint16_t capacity,uint16_t *size)
{ if(m->size>capacity)return -1;memcpy(data,m->data,m->size);*size=m->size;return 0; }
static int ble_gap_conn_find(uint16_t conn,struct ble_gap_conn_desc *desc)
{ desc->sec_state.encrypted=desc->sec_state.bonded=true;desc->sec_state.key_size=16;return 0; }
static bool running=true,enabled=true,ready=true,authenticated=true,subscribed=true;
static bool paused,forget,starting,stopping,busy;
static unsigned worker_stack;
static uint16_t connection=1;
static uint32_t generation=1;
static uint64_t help_until,connected_at;
static const char *security_stage,*info="{}";
static unsigned bonds,sends;
static ble_message_t incoming;
static ble_rpc_t rpc;
static struct { uint32_t state_revision; } app={42};
static jmp_buf done;
static int scenario;
static uint64_t app_now(void) { return 1000; }
static unsigned uxTaskGetStackHighWaterMark(void *task) { return 4096; }
static void app_lock(void) {}
static void app_unlock(void) {}
static void close_pair_help(void) {}
static bool stop_host(void) { return true; }
static bool start_host(void) { return true; }
static void ble_gap_adv_stop(void) {}
static int ble_store_clear(void) { return 0; }
static void set_error(const char *text) { assert(0); }
static void terminate_connection(uint16_t conn,const char *cause) { assert(0); }
static void disconnect_session(uint32_t session) { assert(0); }
static void advertise(void) {}
static void vTaskDelay(unsigned ms) { assert(ms==20);longjmp(done,1); }
static cJSON *dispatch(const char *op,const cJSON *args,const char **error,void *unused)
{ return cJSON_CreateObject(); }
static bool send_message(const char *message,uint32_t session);
#include "ble-transport-under-test.inc"

static int write_request(const char *text)
{
    uint8_t bytes[247];size_t size=ble_message_frame(text,strlen(text),0,247,bytes,sizeof(bytes));
    assert(size==strlen(text)+4);
    struct os_mbuf mbuf={bytes,(uint16_t)size};
    struct ble_gatt_access_ctxt context={BLE_GATT_ACCESS_OP_WRITE_CHR,&mbuf};
    return access_gatt(connection,1,&context,(void *)2);
}
static bool send_message(const char *message,uint32_t session)
{
    sends++;
    if(scenario==2) {
        // Peer sees the final reply and sends its next request before the worker
        // receives the indication ACK. The worker must already have freed the inbox.
        assert(strstr(message,"\"id\":1"));
        assert(write_request("{\"v\":1,\"id\":2,\"op\":\"get_info\",\"args\":{}}") == 0);
        assert(busy && incoming.received==incoming.total);
    }
    return true;
}
int main(int argc,char **argv)
{
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc==2);
    scenario=!strcmp(argv[1],"reply-race")?2:1;
    if(scenario==2)assert(write_request("{\"v\":1,\"id\":1,\"op\":\"get_info\",\"args\":{}}") == 0);
    if(!setjmp(done))worker(NULL);
    if(scenario==1)assert(sends==0); // CCCD restoration alone must not start indications.
    else {
        assert(sends==1 && busy && incoming.received==incoming.total);
        assert(strstr(incoming.data,"\"id\":2")); // Reply cleanup must not erase the next request.
    }
    puts("BLE transport scheduling passed.");return 0;
}
