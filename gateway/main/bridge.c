#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "board.h"
#include "wire.h"
#include "driver/uart.h"
#include "esp_random.h"
#define LINK UART_NUM_1
static SemaphoreHandle_t request_mutex,ack_sem;
static portMUX_TYPE ack_lock=portMUX_INITIALIZER_UNLOCKED;
static int session,sequence,pending;
static bool ack_ok;
static char ack_error[80];
static void receive(void *unused)
{
    static wire_rx_t rx;
    uint8_t bytes[128];
    for (;;) {
        int n=uart_read_bytes(LINK,bytes,sizeof(bytes),pdMS_TO_TICKS(50));
        for (int i=0;i<n;i++) if (wire_feed(&rx,bytes[i])) {
            cJSON *o=cJSON_Parse(rx.data); if (!o) continue;
            const char *type=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"type"));
            int v,sid,seq;
            if (type && !strcmp(type,"ack") && json_integer(cJSON_GetObjectItemCaseSensitive(o,"v"),1,1,&v)
                && json_integer(cJSON_GetObjectItemCaseSensitive(o,"sid"),1,INT_MAX,&sid)
                && json_integer(cJSON_GetObjectItemCaseSensitive(o,"seq"),1,INT_MAX,&seq)) {
                bool accepted=false;
                portENTER_CRITICAL(&ack_lock);
                if (sid==session && seq==pending) {
                    ack_ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"ok"));
                    const char *error=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"error"));
                    snprintf(ack_error,sizeof(ack_error),"%s",error?error:"invalid_ack"); accepted=true;
                }
                portEXIT_CRITICAL(&ack_lock);
                if (accepted) xSemaphoreGive(ack_sem);
            } else app_receive(o);
            cJSON_Delete(o);
        }
    }
}
bool bridge_request(cJSON *o,char error[80])
{
    if (xSemaphoreTake(request_mutex,pdMS_TO_TICKS(2000))!=pdTRUE) { snprintf(error,80,"bridge_busy"); return false; }
    if (++sequence<=0 || sequence>=INT_MAX) sequence=1;
    cJSON_AddNumberToObject(o,"v",1); cJSON_AddNumberToObject(o,"sid",session); cJSON_AddNumberToObject(o,"seq",sequence);
    char json[WIRE_CAP],frame[WIRE_CAP]; size_t length=0;
    if (cJSON_PrintPreallocated(o,json,sizeof(json),false)) length=wire_encode(json,frame,sizeof(frame));
    bool ok=false,received=false;
    while (xSemaphoreTake(ack_sem,0)==pdTRUE) {}
    portENTER_CRITICAL(&ack_lock); pending=sequence; ack_ok=false; ack_error[0]=0; portEXIT_CRITICAL(&ack_lock);
    const char *op=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"op"));
    unsigned timeout=op && !strncmp(op,"ota_",4)?2500:600;
    if (length) for (int attempt=0;attempt<3 && !received;attempt++) {
        uart_write_bytes(LINK,frame,length);
        received=xSemaphoreTake(ack_sem,pdMS_TO_TICKS(timeout))==pdTRUE;
    }
    portENTER_CRITICAL(&ack_lock);
    if (received) { ok=ack_ok; snprintf(error,80,"%s",ack_error); }
    else snprintf(error,80,"%s",length?"UART_timeout":"frame_too_large");
    pending=0;
    portEXIT_CRITICAL(&ack_lock);
    xSemaphoreGive(request_mutex); return ok;
}
void bridge_start(void)
{
    request_mutex=xSemaphoreCreateMutex(); ack_sem=xSemaphoreCreateBinary(); assert(request_mutex && ack_sem);
    session=(int)(esp_random()&INT_MAX); if (!session) session=1;
    uart_config_t config={.baud_rate=115200,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_param_config(LINK,&config));
    ESP_ERROR_CHECK(uart_driver_install(LINK,8192,2048,0,NULL,0));
    ESP_ERROR_CHECK(uart_set_pin(LINK,BOARD_UART_TX,BOARD_UART_RX,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    BaseType_t created=xTaskCreate(receive,"link_rx",6144,NULL,6,NULL); assert(created==pdPASS);
}
