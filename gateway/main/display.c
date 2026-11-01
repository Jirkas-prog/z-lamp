#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "board.h"
#include "ble.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "font8x8_basic.h"

static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t transfer_done;
static uint16_t *pixels;
static bool transfer_complete(esp_lcd_panel_io_handle_t handle,esp_lcd_panel_io_event_data_t *event,void *ctx)
{
    BaseType_t wake=pdFALSE; xSemaphoreGiveFromISR(transfer_done,&wake); return wake==pdTRUE;
}
#define STRIP_ROWS 8
typedef struct { int x,y,w,h,scale;uint16_t color;char label[80]; } draw_command_t;
static draw_command_t frame[20];
static unsigned frame_count;
static bool capturing;
static int strip_y,strip_rows=BOARD_LCD_HEIGHT;
static draw_command_t *record(int x,int y,uint16_t color)
{
    assert(frame_count<sizeof(frame)/sizeof(frame[0]));
    draw_command_t *cmd=&frame[frame_count++];memset(cmd,0,sizeof(*cmd));
    cmd->x=x;cmd->y=y;cmd->color=color;return cmd;
}
static uint16_t rgb565(unsigned r,unsigned g,unsigned b)
{
    uint16_t n=((r>>3)<<11)|((g>>2)<<5)|(b>>3); return (uint16_t)((n<<8)|(n>>8));
}
static void fill(int x,int y,int w,int h,uint16_t color)
{
    if(capturing) { draw_command_t *cmd=record(x,y,color);cmd->w=w;cmd->h=h;return; }
    int top=y>strip_y?y:strip_y,bottom=y+h<strip_y+strip_rows?y+h:strip_y+strip_rows;
    int left=x>0?x:0,right=x+w<BOARD_LCD_WIDTH?x+w:BOARD_LCD_WIDTH;
    for(int row=top;row<bottom;row++)for(int col=left;col<right;col++)pixels[(row-strip_y)*BOARD_LCD_WIDTH+col]=color;
}
static unsigned ascii_char(const unsigned char **p)
{
    unsigned c=*(*p)++; if (c<128) return c;
    unsigned next=**p;
    // Czech names remain legible on the small ASCII bitmap font.
    if (c==0xc3 || c==0xc4 || c==0xc5) {
        if (next) (*p)++;
        const unsigned codes[]={0xc381,0xc3a1,0xc48c,0xc48d,0xc48e,0xc48f,0xc389,0xc3a9,0xc49a,0xc49b,
            0xc38d,0xc3ad,0xc587,0xc588,0xc393,0xc3b3,0xc598,0xc599,0xc5a0,0xc5a1,0xc5a4,0xc5a5,
            0xc39a,0xc3ba,0xc5ae,0xc5af,0xc39d,0xc3bd,0xc5bd,0xc5be};
        const char mapped[]="AaCcDdEeEeIiNnOoRrSsTtUuUuYyZz";
        for (unsigned i=0;i<sizeof(codes)/sizeof(codes[0]);i++) if (((c<<8)|next)==codes[i]) return mapped[i];
    } else while ((**p&0xc0)==0x80) (*p)++;
    return '?';
}
static void text(int x,int y,const char *s,uint16_t color,int scale)
{
    if(capturing) { draw_command_t *cmd=record(x,y,color);cmd->scale=scale;snprintf(cmd->label,sizeof(cmd->label),"%s",s);return; }
    if(y+8*scale<=strip_y || y>=strip_y+strip_rows)return;
    const unsigned char *p=(const unsigned char *)s;
    while (*p && x+8*scale<=BOARD_LCD_WIDTH) {
        unsigned c=ascii_char(&p); if (c<32 || c>126) c='?';
        for (int row=0;row<8;row++) for (int col=0;col<8;col++)
            if ((unsigned char)font8x8_basic[c][row]&(1u<<col)) fill(x+col*scale,y+row*scale,scale,scale,color);
        x+=8*scale;
    }
}
static int wrapped(int y,const char *prefix,const char *value,uint16_t color)
{
    char combined[110]; snprintf(combined,sizeof(combined),"%s%s",prefix,value);
    size_t n=strlen(combined);
    for (size_t offset=0;offset<n;offset+=29) {
        char line[30]; snprintf(line,sizeof(line),"%.29s",combined+offset); text(4,y,line,color,1); y+=11;
    }
    return y;
}
static void backlight(unsigned level)
{
    if (level>254) level=254;
    unsigned duty=level*level*1023/(254*254); if (level && !duty) duty=1;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0));
}
static unsigned render(void)
{
    uint16_t white=rgb565(225,230,241),muted=rgb565(125,142,165),accent=rgb565(117,222,184);
    frame_count=0;
    if(app.ble_help_until>app_now()) {
        text(4,8,"BLUETOOTH PAROVANI",accent,1);
        text(4,29,app.runtime_alias,white,1);
        text(4,53,"BEZ PINU",accent,2);
        text(4,89,"V aplikaci: Parovat",white,1);
        text(4,111,"Vyber tuto lampu.",muted,1);
        return app.config.status_brightness*254/100;
    }
    if (app.config.lamp_mode && app_now()>=app.status_until) {
        uint8_t rgb[3]; values_rgb(app.config.lamp,rgb);
        if (app.config.lamp.on) fill(0,0,BOARD_LCD_WIDTH,BOARD_LCD_HEIGHT,rgb565(rgb[0],rgb[1],rgb[2]));
        return app.config.lamp.on?app.config.lamp.level:0;
    }
    bool link=app.c6_last_ms && app_now()-app.c6_last_ms<6000 && app.c6_ready;
    unsigned total=0,on=0,unknown=0;
    for (int i=0;i<MAX_LIGHTS;i++) if (app.lights[i].known) {
        total++;
        if (!link || !app.lights[i].online || !app.lights[i].reported) unknown++;
        else if (app.lights[i].on) on++;
    }
    unsigned pages=1+(total+3)/4; if (app.page>=pages) app.page=0;
    char line[60];
    if (app.page==0) {
        int y=wrapped(3,"",app.runtime_alias,accent);
        snprintf(line,sizeof(line),"%s.local",app.hostname); y=wrapped(y,"",line,accent);
        if (app.ap_active) {
            y=wrapped(y,"AP: ",app.runtime_ssid,white);
            y=wrapped(y,"PW: ",app.runtime_password,white);
            text(4,y,"IP: 192.168.4.1",accent,1); y+=11;
        } else {
            text(4,y,"HOTSPOT: VYPNUTY",accent,1); y+=11;
            if (app.sta_connected) {
                y=wrapped(y,"WIFI: ",app.sta_ssid,white);
                y=wrapped(y,"IP: ",app.sta_ip,accent);
            } else {
                text(4,y,app.options.network_count?"HLEDAM ULOZENE WIFI":"ZADNE ULOZENE WIFI",muted,1); y+=11;
                text(4,y,"AP: PODRZ GPIO35 3s",muted,1); y+=11;
            }
        }
        snprintf(line,sizeof(line),"SVITI: %u/%u  NEZNAME: %u",on,total,unknown); text(4,y,line,white,1); y+=11;
        snprintf(line,sizeof(line),"ZIGBEE: %s CH:%d",link?"OK":"OFFLINE",app.channel); text(4,y,line,link?accent:rgb565(255,150,90),1);
    } else {
        snprintf(line,sizeof(line),"ZAROVKY %u-%u / %u",(app.page-1)*4+1,app.page*4<total?app.page*4:total,total);
        text(4,5,line,accent,1);
        unsigned index=0;
        for (int i=0;i<MAX_LIGHTS;i++) {
            light_t *l=&app.lights[i]; if (!l->known) continue;
            unsigned position=index++;
            if (position/4!=app.page-1) continue;
            int y=27+(position%4)*25; uint8_t rgb[3]={70,70,70};
            bool fresh=link && l->reported && l->online;
            if (fresh && l->on && l->color_known) {
                lamp_xy_t xy=l->mode==2?lamp_mix(l->mired?1000000.0f/l->mired:2700,0,0):(lamp_xy_t){l->x/65536.0f,l->y/65536.0f};
                lamp_xy_rgb(xy.x,xy.y,1,rgb);
                if (l->mode==0) lamp_hsv_rgb(l->hue/254.0f,l->saturation/254.0f,rgb);
            }
            fill(5,y,12,12,rgb565(rgb[0],rgb[1],rgb[2]));
            text(24,y,app_name(l->id),white,1);
            snprintf(line,sizeof(line),"%s  jas %d%%",fresh?(l->on?"SVITI":"VYPNUTO"):"NEZNAME",l->level*100/254);
            text(24,y+11,line,muted,1);
        }
    }
    return app.config.status_brightness*254/100;
}
static unsigned capture_frame(void)
{
    capturing=true;unsigned level=render();capturing=false;return level;
}
static void render_strip(int y,int rows)
{
    strip_y=y;strip_rows=rows;memset(pixels,0,BOARD_LCD_WIDTH*rows*sizeof(*pixels));
    for(unsigned i=0;i<frame_count;i++) {
        draw_command_t *cmd=&frame[i];
        if(cmd->scale)text(cmd->x,cmd->y,cmd->label,cmd->color,cmd->scale);
        else fill(cmd->x,cmd->y,cmd->w,cmd->h,cmd->color);
    }
}
static void display_task(void *unused)
{
    lamp_debounce_t screen_key={0},preset_key={0}; bool awake=false;
    button_hold_t hold={0},preset_hold={0};
    uint64_t last_draw=0,last_page=0;
    for (;;) {
        uint64_t now=app_now();
        bool key=lamp_debounce(&screen_key,gpio_get_level(BOARD_SCREEN_BUTTON)==0,(uint32_t)now);
        bool preset=lamp_debounce(&preset_key,gpio_get_level(BOARD_PRESET_BUTTON)==0,(uint32_t)now);
        button_action_t action=button_hold(&hold,key,now,3000);
        if (action==BUTTON_LONG) app_toggle_ap();
        button_action_t preset_action=button_hold(&preset_hold,preset,now,5000);
        if(preset_action==BUTTON_LONG)ble_pair(true);
        app_lock();
        if (action==BUTTON_SHORT) {
            screen_button(&app.screen,now);
            if (app.screen.awake) { app.page=0; last_page=now; app.status_until=now+15000; }
        }
        if (preset_action==BUTTON_SHORT && !app.updating) app.preset_pending=true;
        if (app.screen.awake && now>=app.status_until && now-last_page>=8000) { app.page++; last_page=now; }
        if(now>=app.ble_help_until)screen_timeout(&app.screen,now,app.config.timeout);
        bool enabled=app.screen.awake;
        app_unlock();
        if (enabled!=awake) {
            backlight(0);
            if (enabled) {
                ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io,LCD_CMD_SLPOUT,NULL,0));
                vTaskDelay(pdMS_TO_TICKS(120)); ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
            } else {
                ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,false));
                ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io,LCD_CMD_SLPIN,NULL,0));
            }
            awake=enabled; last_draw=0;
        }
        if (awake && (!last_draw || now-last_draw>=250)) {
            app_lock(); unsigned level=capture_frame(); app_unlock();
            for(int y=0;y<BOARD_LCD_HEIGHT;y+=STRIP_ROWS) {
                int rows=BOARD_LCD_HEIGHT-y;if(rows>STRIP_ROWS)rows=STRIP_ROWS;
                render_strip(y,rows);
                ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel,0,y,BOARD_LCD_WIDTH,y+rows,pixels));
                if (xSemaphoreTake(transfer_done,pdMS_TO_TICKS(1000))!=pdTRUE) { backlight(0); abort(); }
            }
            backlight(level); last_draw=app_now();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void display_start(void)
{
    transfer_done=xSemaphoreCreateBinary(); assert(transfer_done);
    pixels=heap_caps_malloc(BOARD_LCD_WIDTH*STRIP_ROWS*2,MALLOC_CAP_DMA); assert(pixels);
    ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.timer_num=LEDC_TIMER_0,.duty_resolution=LEDC_TIMER_10_BIT,.freq_hz=5000,.clk_cfg=LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t light={.gpio_num=BOARD_TFT_BACKLIGHT,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_0,.duty=0};
    ESP_ERROR_CHECK(ledc_channel_config(&light));
    gpio_config_t keys={.pin_bit_mask=(1ULL<<BOARD_SCREEN_BUTTON)|(1ULL<<BOARD_PRESET_BUTTON),.mode=GPIO_MODE_INPUT};
    ESP_ERROR_CHECK(gpio_config(&keys)); // GPIO35 has a board-level pull-up; ESP32 has no internal pull-up on it.
    gpio_pullup_en(BOARD_PRESET_BUTTON);
    spi_bus_config_t bus={.mosi_io_num=BOARD_TFT_MOSI,.sclk_io_num=BOARD_TFT_CLOCK,.miso_io_num=-1,.quadwp_io_num=-1,.quadhd_io_num=-1,
        .max_transfer_sz=BOARD_LCD_WIDTH*STRIP_ROWS*2+8};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t config={.cs_gpio_num=BOARD_TFT_CS,.dc_gpio_num=BOARD_TFT_DC,.spi_mode=0,
        .pclk_hz=20000000,.trans_queue_depth=1,.lcd_cmd_bits=8,.lcd_param_bits=8,.on_color_trans_done=transfer_complete};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&config,&io));
    esp_lcd_panel_dev_config_t dev={.reset_gpio_num=BOARD_TFT_RESET,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io,&dev,&panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel)); ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel,true)); ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel,true,false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel,40,53)); ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,false)); ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io,LCD_CMD_SLPIN,NULL,0));
    BaseType_t created=xTaskCreate(display_task,"display",4096,NULL,3,NULL); assert(created==pdPASS);
}
