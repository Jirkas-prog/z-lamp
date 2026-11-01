#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "freertos/queue.h"

static QueueHandle_t rx_queue,key_queue;
static rmt_channel_handle_t channel;
typedef struct { int key; uint64_t at; uint32_t revision; } key_event_t;
static bool received(rmt_channel_handle_t handle,const rmt_rx_done_event_data_t *event,void *ctx)
{
    BaseType_t wake=pdFALSE;
    xQueueSendFromISR(rx_queue,event,&wake);
    return wake==pdTRUE;
}
static void receiver(void *unused)
{
    rmt_symbol_word_t symbols[64]; ir_pulse_t pulses[128];
    rmt_rx_done_event_data_t event;
    const rmt_receive_config_t config={.signal_range_min_ns=1250,.signal_range_max_ns=12000000};
    uint32_t previous=0,suppressed=0;
    uint64_t last=0,last_action=0,suppress_until=0;
    ESP_ERROR_CHECK(rmt_receive(channel,symbols,sizeof(symbols),&config));
    for (;;) {
        if (xQueueReceive(rx_queue,&event,portMAX_DELAY)!=pdTRUE) continue;
        size_t count=event.num_symbols;
        if (count>64) count=64;
        for (size_t i=0;i<count;i++) {
            pulses[2*i]=(ir_pulse_t){event.received_symbols[i].duration0,event.received_symbols[i].level0};
            pulses[2*i+1]=(ir_pulse_t){event.received_symbols[i].duration1,event.received_symbols[i].level1};
        }
        uint32_t code=0; ir_frame_t frame=ir_decode(pulses,count*2,&code);
        // Re-arm before doing any work; light commands run in a separate task and can wait for UART.
        ESP_ERROR_CHECK(rmt_receive(channel,symbols,sizeof(symbols),&config));
        uint64_t now=app_now();
        if (frame==IR_INVALID) { previous=0; continue; }
        if (frame==IR_REPEAT) { if (!previous || now-last>250) continue; code=previous; }
        bool repeat=code==previous && now-last<=250;
        previous=code; last=now;
        app_lock();
        app.ir_last_code=code; app.ir_last_ms=now;
        if (app.ir_learn_key>=0 && now>=app.ir_learn_until) app.ir_learn_key=-1;
        if (app.ir_learn_key>=0) {
            if (!repeat && frame==IR_FRAME) {
                int key=app.ir_learn_key;
                for (int i=0;i<IR_KEYS;i++) if (app.ir.codes[i]==code) app.ir.codes[i]=0;
                app.ir.codes[key]=code; app.ir_learn_key=-1; app_changed();
                suppressed=code; suppress_until=now+500;
                ESP_LOGI("ir","Learned %s = %08lX",ir_keys[key].name,(unsigned long)code);
            }
            app_unlock(); continue; // Learning never operates the lights.
        }
        int key=ir_match(app.ir.codes,code); bool enabled=app.ir.enabled && !app.updating; uint32_t revision=app.ir_revision;
        app_unlock();
        if (code==suppressed && now<suppress_until) { suppress_until=now+500; continue; }
        if (!repeat) ESP_LOGI("ir","NEC %08lX -> %s",(unsigned long)code,key>=0?ir_keys[key].name:"unmapped");
        if (!enabled || key<0 || (repeat && (key>1 || now-last_action<200))) continue;
        key_event_t press={key,now,revision}; xQueueOverwrite(key_queue,&press); last_action=now;
    }
}
static void actions(void *unused)
{
    char target[ID_LEN]={0};
    for (;;) {
        key_event_t press;
        if (xQueueReceive(key_queue,&press,pdMS_TO_TICKS(50))==pdTRUE && app_now()-press.at<1500) {
            app_lock(); bool enabled=app.ir.enabled && !app.updating && app.ir_learn_key<0 && press.revision==app.ir_revision;
            snprintf(target,sizeof(target),"%s",app.ir.target); app_unlock();
            if (!enabled) continue;
            light_values_t base=app_target_values(target);ir_effect_t effect=ir_keys[press.key].effect;
            if (effect) {
                app_lock();int scene=app.scenes.buttons[effect-1];app_unlock();
                if(scene>=0)app_start_scene(scene,true,press.revision);
            } else if(!app_scene_remote(press.key,press.revision)) {
                cJSON *patch=cJSON_CreateObject();
                app_lock();
                if(press.key==7) { base.kelvin=(int)app.scenes.white_kelvin;base.level=(int)app.scenes.white_level; }
                remote_config_t remote=app.remote;app_unlock();
                if (ir_key_patch(press.key,base,&remote,patch)) {
                    const char *failure=app_set_ir(target,patch,press.revision);
                    if (failure) { app_lock(); snprintf(app.error,sizeof(app.error),"%s",failure); app_unlock(); }
                }
                cJSON_Delete(patch);
            }
        }
    }
}
void ir_start(void)
{
    // Establish the receiver return before applying power. Both latches start LOW.
    ESP_ERROR_CHECK(gpio_set_level(BOARD_IR_GND,0));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_IR_POWER,0));
    const gpio_config_t supply={
        .pin_bit_mask=(1ULL<<BOARD_IR_GND)|(1ULL<<BOARD_IR_POWER),
        .mode=GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&supply));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_IR_POWER,1));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI("ir","Receiver supply configured: GPIO%d=HIGH, GPIO%d=LOW",BOARD_IR_POWER,BOARD_IR_GND);
    rx_queue=xQueueCreate(1,sizeof(rmt_rx_done_event_data_t)); key_queue=xQueueCreate(1,sizeof(key_event_t));
    assert(rx_queue && key_queue);
    rmt_rx_channel_config_t config={.gpio_num=BOARD_IR_RX,.clk_src=RMT_CLK_SRC_DEFAULT,.resolution_hz=1000000,.mem_block_symbols=64};
    ESP_ERROR_CHECK(rmt_new_rx_channel(&config,&channel));
    // Idle HIGH even when the receiver is unplugged; enable only after receiver power.
    ESP_ERROR_CHECK(gpio_set_pull_mode(BOARD_IR_RX,GPIO_PULLUP_ONLY));
    rmt_rx_event_callbacks_t callbacks={.on_recv_done=received};
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(channel,&callbacks,NULL));
    ESP_ERROR_CHECK(rmt_enable(channel));
    BaseType_t rx=xTaskCreate(receiver,"ir_rx",4096,NULL,5,NULL);
    BaseType_t control=xTaskCreate(actions,"ir_actions",10240,NULL,3,NULL);
    assert(rx==pdPASS && control==pdPASS);
    ESP_LOGI("ir","NEC receiver ready on GPIO%d; idle HIGH, active LOW",BOARD_IR_RX);
}
