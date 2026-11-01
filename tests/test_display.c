// Render the actual LCD status page; catch clipped text and save synthetic previews.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "scenes.h"
#include "infrared.h"
#include "../gateway/main/board.h"
#include "../gateway/main/font8x8_basic.h"
typedef int SemaphoreHandle_t;
typedef int esp_err_t;
#define portMAX_DELAY 0
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_CHANNEL_0 0
#define ESP_ERROR_CHECK(x) assert((x)==0)
static int xSemaphoreTake(int s,int t) { (void)s;(void)t;return 1; }
static void xSemaphoreGive(int s) { (void)s; }
static int ledc_set_duty(int m,int c,unsigned d) { (void)m;(void)c;(void)d;return 0; }
static int ledc_update_duty(int m,int c) { (void)m;(void)c;return 0; }
static uint16_t *pixels;
#include "display-under-test.inc"
app_state_t app;
SemaphoreHandle_t app_mutex,command_mutex;
uint64_t app_now(void) { return 1000; }
const char *app_name(const char *id) { (void)id;return "Zarovka"; }
static void snapshot(const char *prefix,const char *name)
{
    assert(capture_frame()==40*254/100);
    render_strip(0,BOARD_LCD_HEIGHT);
    uint16_t *full=pixels;
    uint16_t strip[BOARD_LCD_WIDTH*STRIP_ROWS+2];pixels=strip+1;
    for(int y=0;y<BOARD_LCD_HEIGHT;y+=STRIP_ROWS) {
        int rows=BOARD_LCD_HEIGHT-y;if(rows>STRIP_ROWS)rows=STRIP_ROWS;
        strip[0]=strip[BOARD_LCD_WIDTH*rows+1]=0xa55a;
        render_strip(y,rows);
        assert(strip[0]==0xa55a && strip[BOARD_LCD_WIDTH*rows+1]==0xa55a);
        assert(!memcmp(pixels,full+y*BOARD_LCD_WIDTH,BOARD_LCD_WIDTH*rows*sizeof(*pixels)));
    }
    pixels=full;
    char path[1024];snprintf(path,sizeof(path),"%s-%s.ppm",prefix,name);
    FILE *f=fopen(path,"wb");assert(f);
    fprintf(f,"P6\n%d %d\n255\n",BOARD_LCD_WIDTH,BOARD_LCD_HEIGHT);
    for (int i=0;i<BOARD_LCD_WIDTH*BOARD_LCD_HEIGHT;i++) {
        uint16_t c=(uint16_t)((pixels[i]<<8)|(pixels[i]>>8));
        unsigned char rgb[]={(unsigned char)(((c>>11)&31)*255/31),(unsigned char)(((c>>5)&63)*255/63),(unsigned char)((c&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    fclose(f);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    pixels=calloc(BOARD_LCD_WIDTH*BOARD_LCD_HEIGHT,sizeof(*pixels));assert(pixels);
    app.config.status_brightness=40;app.ap_active=true;
    strcpy(app.runtime_alias,"Z-lamp");strcpy(app.hostname,"z-lamp");
    strcpy(app.runtime_ssid,"HueLamp-DEMO");strcpy(app.runtime_password,"demo-pass-123");
    snapshot(argv[1],"default");
    memset(app.runtime_alias,'W',32);memset(app.hostname,'w',32);
    memset(app.runtime_ssid,'W',32);memset(app.runtime_password,'W',63);
    for (int i=0;i<MAX_LIGHTS;i++) app.lights[i].known=true;
    snapshot(argv[1],"long"); // Maximum alias, hostname, SSID and password must all fit.
    app.ap_active=false;app.sta_connected=true;
    strcpy(app.runtime_alias,"Nocni-lampa");strcpy(app.hostname,"nocni-lampa");
    strcpy(app.sta_ssid,"Domaci Wi-Fi");strcpy(app.sta_ip,"192.0.2.123");
    snapshot(argv[1],"station");
    app.sta_connected=false;snapshot(argv[1],"offline");
    for (app.page=1;app.page<=4;app.page++) snapshot(argv[1],"bulbs");
    app.ble_help_until=120000;snapshot(argv[1],"pairing");app.ble_help_until=0;
    app.config.lamp_mode=true;app.config.lamp=(light_values_t){.on=true,.level=254,.hue=240,.saturation=100,.kelvin=2700};
    assert(capture_frame()==254);render_strip(0,STRIP_ROWS);
    free(pixels);puts("Production LCD render: alias/address, AP/STA/offline, maximum text lengths and bulb pages fit 240x135.");
    return 0;
}
