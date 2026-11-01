#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "scenes.h"
int main(void)
{
    scene_settings_t settings;scenes_default(&settings);assert(settings.brightness_steps==10 && settings.white_kelvin==2700);
    for(int i=0;i<4;i++)assert(settings.buttons[i]==i && scene_valid(&settings.items[i]));
    assert(scenes_valid(&settings));settings.brightness_steps=0;assert(!scenes_valid(&settings));settings.brightness_steps=10;
    settings.items[0].fade_ds=settings.items[0].hold_ds=0;assert(!scenes_valid(&settings));scenes_default(&settings);
    scene_t s=scene_default();
    for(unsigned mode=0;mode<SCENE_MODES;mode++) {
        s.mode=mode;
        for(unsigned step=0;step<1000;step++)for(unsigned member=0;member<17;member++) {
            light_values_t v=scene_value(&s,step,member,17,member);
            assert(v.saturation==100 && v.hue>=0 && v.hue<360 && v.level>=1 && v.level<=254);
            uint8_t h,sat;lamp_native_hs(v.kelvin,v.hue/360.0f,v.saturation/100.0f,&h,&sat);assert(sat==254);
        }
    }
    s.mode=SCENE_CUSTOM;s.hues[0]=240;s.hues[1]=320;
    assert(scene_value(&s,0,0,2,0).hue==240 && scene_value(&s,0,1,2,1).hue==320);
    assert(scene_value(&s,1,0,2,0).hue==320 && scene_value(&s,1,1,2,1).hue==240);
    assert(scene_value(&s,2,0,2,0).hue==240);
    s.use_ir=false;s.member_count=2;strcpy(s.members[0].id,"001788010203abcd");strcpy(s.members[1].id,"display");s.members[1].offset=1;
    cJSON *o=scene_json(&s,3);scene_t parsed={0};assert(scene_parse(&parsed,o));assert(parsed.mode==12 && parsed.members[1].offset==1 && parsed.hues[1]==320);
    cJSON_AddNumberToObject(o,"unrecognized",1);assert(!scene_parse(&parsed,o));cJSON_DeleteItemFromObject(o,"unrecognized");
    cJSON_AddNumberToObject(o,"level",10);assert(!scene_parse(&parsed,o));cJSON_Delete(o);
    s.hold_ds=0;s.fade_ds=4;assert(!scene_valid(&s));s.fade_ds=5;assert(scene_valid(&s));
    s.color_count=1;assert(!scene_valid(&s));s.color_count=2;s.hues[1]=360;assert(!scene_valid(&s));
    s.hues[1]=320;strcpy(s.members[1].id,s.members[0].id);assert(!scene_valid(&s));
    assert(scene_index("s1")==0 && scene_index("s8")==7 && scene_index("s9")==-1 && scene_index(NULL)==-1 && scene_index("s11")==-1);
    puts("Scenes: 13 modes x 1000 steps x 17 members, no white, custom phases/wrap, JSON roundtrip and invalid data passed.");
    return 0;
}
