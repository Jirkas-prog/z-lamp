#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "infrared.h"
static void frame(ir_pulse_t p[68],uint32_t code)
{
    p[0]=(ir_pulse_t){9000,false};p[1]=(ir_pulse_t){4500,true};
    for (int i=0;i<32;i++) {
        p[2+2*i]=(ir_pulse_t){560,false};
        p[3+2*i]=(ir_pulse_t){(code&(1u<<(31-i)))?1690:560,true};
    }
    p[66]=(ir_pulse_t){560,false};p[67]=(ir_pulse_t){12000,true};
}
int main(void)
{
    uint32_t codes[IR_KEYS],decoded=0;ir_pulse_t p[69];
    for (int i=0;i<IR_KEYS;i++) codes[i]=ir_keys[i].code;
    for (int i=0;i<IR_KEYS;i++) {
        assert(ir_match(codes,ir_keys[i].code)==i && ir_match(codes,ir_keys[i].old_code)==i);
        frame(p,codes[i]);assert(ir_decode(p,68,&decoded)==IR_FRAME && decoded==codes[i]);
        assert(ir_decode(p,67,&decoded)==IR_FRAME); // Final idle may be omitted by RMT.
        for(int j=0;j<67;j++)p[j].us=(uint16_t)(p[j].us*11/10);
        assert(ir_decode(p,68,&decoded)==IR_FRAME && decoded==codes[i]); // Receiver pulse distortion.
        frame(p,ir_keys[i].old_code);assert(ir_decode(p,68,&decoded)==IR_FRAME && decoded==ir_keys[i].old_code);
    }
    frame(p,ir_keys[3].code);memmove(p+1,p,68*sizeof(*p));p[0]=(ir_pulse_t){1000,true};
    assert(ir_decode(p,69,&decoded)==IR_FRAME && decoded==ir_keys[3].code);
    frame(p,ir_keys[3].code^1);assert(ir_decode(p,68,&decoded)==IR_INVALID); // Complement catches corrupted command.
    frame(p,ir_keys[3].code);p[10].high=true;assert(ir_decode(p,68,&decoded)==IR_INVALID);
    frame(p,ir_keys[3].code);p[5].us=1000;assert(ir_decode(p,68,&decoded)==IR_INVALID);
    frame(p,ir_keys[3].code);p[66].us=100;assert(ir_decode(p,68,&decoded)==IR_INVALID);
    assert(ir_decode(p,20,&decoded)==IR_INVALID && ir_decode(p,0,&decoded)==IR_INVALID);
    ir_pulse_t repeat[]={{9000,false},{2250,true},{560,false},{12000,true}};
    assert(ir_decode(repeat,4,&decoded)==IR_REPEAT && ir_decode(repeat,3,&decoded)==IR_REPEAT);
    repeat[1].us=4500;assert(ir_decode(repeat,4,&decoded)==IR_INVALID);
    codes[0]=0x12AB34CB;assert(ir_match(codes,0x12AB34CB)==0 && ir_match(codes,ir_keys[0].old_code)==-1);
    assert(ir_match(codes,0)==-1 && ir_match(codes,0x12345678)==-1);
    remote_config_t remote;remote_default(&remote,10);
    light_values_t v={true,250,3000,123,42};
    cJSON *patch=cJSON_CreateObject();assert(ir_key_patch(0,v,&remote,patch) && values_patch(&v,patch) && v.level==254);cJSON_Delete(patch);
    v.level=5;patch=cJSON_CreateObject();assert(ir_key_patch(1,v,&remote,patch) && values_patch(&v,patch) && v.level==3);cJSON_Delete(patch);
    patch=cJSON_CreateObject();assert(ir_key_patch(7,v,&remote,patch) && values_patch(&v,patch) && v.saturation==0 && v.kelvin==3000 && v.level==3);cJSON_Delete(patch);
    patch=cJSON_CreateObject();assert(ir_key_patch(6,v,&remote,patch) && values_patch(&v,patch) && v.hue==240 && v.saturation==100);cJSON_Delete(patch);
    patch=cJSON_CreateObject();assert(ir_key_patch(2,v,&remote,patch) && cJSON_GetArraySize(patch)==1 && values_patch(&v,patch) && !v.on);cJSON_Delete(patch);
    const uint8_t expected[]={1,2,4,7,12,20,32,50,75,100};assert(!memcmp(remote.percent,expected,10));
    for(unsigned steps=2;steps<=32;steps++) {
        remote_default(&remote,steps);assert(remote_valid(&remote));int level=3,count=1;
        while(level<254) { int next=ir_brightness_step(level,1,&remote);assert(next>level && next<=254);assert(ir_brightness_step(next,-1,&remote)==level);level=next;count++; }
        assert(count==(int)steps && ir_brightness_step(3,-1,&remote)==3 && ir_brightness_step(254,1,&remote)==254);
        for(int l=4;l<254;l++)assert(ir_brightness_step(l,-1,&remote)<l && ir_brightness_step(l,1,&remote)>l);
    }
    remote_default(&remote,4);remote.custom=true;remote.percent[0]=1;remote.percent[1]=2;remote.percent[2]=4;remote.percent[3]=20;
    assert(remote_valid(&remote));assert(ir_brightness_step(5,1,&remote)==10 && ir_brightness_step(51,1,&remote)==51);
    remote.percent[2]=2;assert(!remote_valid(&remote));remote.percent[2]=101;assert(!remote_valid(&remote));
    puts("IR: 48 remote codes, NEC timings/polarity/integrity, repeats, learned mapping, brightness limits, white, colors and effects passed.");
    return 0;
}
