#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wire.h"
#include "model.h"

static unsigned feed(wire_rx_t *rx,const char *bytes,size_t size)
{
    unsigned count=0;
    for (size_t i=0;i<size;i++) if (wire_feed(rx,(uint8_t)bytes[i])) count++;
    return count;
}
int main(void)
{
    assert(wire_crc("123456789",9)==0xcbf43926u); // Standard CRC-32 reference vector.
    wire_rx_t rx={0}; char frame[WIRE_CAP];
    const char *message="{\"op\":\"set\",\"patch\":{\"on\":true}}";
    size_t length=wire_encode(message,frame,sizeof(frame));
    assert(length>0);
    assert(feed(&rx,frame,5)==0); // Arbitrarily fragmented UART reads.
    assert(feed(&rx,frame+5,length-5)==1 && !strcmp(rx.data,message));
    frame[4]^=1; assert(feed(&rx,frame,length)==0); frame[4]^=1;
    assert(feed(&rx,frame,length)==1); // Recovery after corruption.
    for (unsigned i=0;i<WIRE_CAP*3;i++) assert(!wire_feed(&rx,'x'));
    assert(feed(&rx,frame,length)==0); // Oversized partial frame consumes through newline.
    assert(feed(&rx,frame,length)==1);
    assert(feed(&rx,"bad\0bytes\n",10)==0);
    assert(feed(&rx,frame,length)==1);
    assert(!wire_encode("{}\n{}",frame,sizeof(frame)));
    assert(!wire_encode(message,frame,10));

    light_values_t values=values_default();
    cJSON *patch=cJSON_Parse("{\"hue\":240,\"saturation\":40}");
    assert(values_patch(&values,patch)); cJSON_Delete(patch);
    assert(values.hue==240 && values.saturation==40 && values.kelvin==2700 && !values.on);
    const char *bad[]={"{}","[]","{\"on\":1}","{\"level\":0}","{\"hue\":360.1}","{\"saturation\":101}",
        "{\"on\":true,\"kelvin\":1999}","{\"level\":1,\"level\":254}","{\"on\":true,\"unknown\":0}"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        patch=cJSON_Parse(bad[i]); light_values_t before=values;
        assert(!values_patch(&values,patch)); cJSON_Delete(patch);
        assert(values.on==before.on && values.level==before.level && values.hue==before.hue && values.kelvin==before.kelvin && values.saturation==before.saturation);
    }
    patch=values_json(values); light_values_t restored=values_default();
    assert(values_patch(&restored,patch) && restored.hue==240 && restored.saturation==40); cJSON_Delete(patch);
    assert(valid_light_id("001788010203abcd") && !valid_light_id("001788010203ABCd"));
    assert(!valid_light_id(NULL) && !valid_name("line\nbreak"));
    assert(valid_alias("Z-lamp") && valid_alias("A") && valid_alias("2-lamp") && valid_alias("a--b"));
    assert(valid_alias("01234567890123456789012345678901"));
    const char *bad_alias[]={NULL,"","-lamp","lamp-","lamp.local","lamp name","lampa_1","\xc5\xbe-lamp","http://lamp","012345678901234567890123456789012"};
    for (unsigned i=0;i<sizeof(bad_alias)/sizeof(bad_alias[0]);i++) assert(!valid_alias(bad_alias[i]));
    assert(cJSON_Parse("[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]")==NULL);

    screen_gate_t screen={false,0};
    screen_timeout(&screen,1000000,120); assert(!screen.awake);
    screen_button(&screen,1000000); assert(screen.awake);
    screen_timeout(&screen,1119999,120); assert(screen.awake);
    screen_timeout(&screen,1120000,120); assert(!screen.awake);
    screen_timeout(&screen,1120001,0); assert(!screen.awake); // Disabling timeout must not wake it.
    screen_button(&screen,1120001); screen_timeout(&screen,9999999999ULL,0); assert(screen.awake);
    screen_button(&screen,10000000000ULL); assert(!screen.awake);
    button_hold_t button={0};
    assert(button_hold(&button,false,0,3000)==BUTTON_NONE);
    assert(button_hold(&button,true,100,3000)==BUTTON_NONE);
    assert(button_hold(&button,false,200,3000)==BUTTON_SHORT);
    assert(button_hold(&button,false,300,3000)==BUTTON_NONE);
    assert(button_hold(&button,true,500,3000)==BUTTON_NONE);
    assert(button_hold(&button,true,3499,3000)==BUTTON_NONE);
    assert(button_hold(&button,true,3500,3000)==BUTTON_LONG);
    assert(button_hold(&button,true,9500,3000)==BUTTON_NONE); // Never repeat during a hold.
    assert(button_hold(&button,false,9600,3000)==BUTTON_NONE); // No short action after a hold.
    assert(button_hold(&button,true,10000000000ULL,3000)==BUTTON_NONE);
    assert(button_hold(&button,false,10000000100ULL,3000)==BUTTON_SHORT);
    assert(button_hold(&button,true,10000000200ULL,3000)==BUTTON_NONE);
    assert(button_hold(&button,false,10000003200ULL,3000)==BUTTON_LONG);
    uint8_t rgb[3]; values=(light_values_t){true,254,2700,0,100}; values_rgb(values,rgb);
    assert(rgb[0]>250 && rgb[1]<5 && rgb[2]<5);
    values.saturation=0; assert(values_command(values).white);
    values.saturation=1; assert(!values_command(values).white);
    puts("CRC recovery, atomic command validation and display sleep checks passed.");
    return 0;
}
