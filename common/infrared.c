#include "infrared.h"
#include <string.h>
#include <math.h>

// Common 24-key RGB remote wire codes; measured variants listed by the WLED project:
// https://github.com/wled/WLED/blob/main/wled00/ir_codes.h (IR24 and IR24_OLD).
const ir_key_t ir_keys[IR_KEYS]={
    {"Jas +",0x00F700FF,0x00FF906F,-1,0},{"Jas -",0x00F7807F,0x00FFB847,-1,0},
    {"OFF",0x00F740BF,0x00FFF807,-1,0},{"ON",0x00F7C03F,0x00FFB04F,-1,0},
    {"R",0x00F720DF,0x00FF9867,0,0},{"G",0x00F7A05F,0x00FFD827,120,0},
    {"B",0x00F7609F,0x00FF8877,240,0},{"W",0x00F7E01F,0x00FFA857,-1,0},
    {"R1",0x00F710EF,0x00FFE817,15,0},{"G1",0x00F7906F,0x00FF48B7,105,0},
    {"B1",0x00F750AF,0x00FF6897,225,0},{"FLASH",0x00F7D02F,0x00FFB24D,-1,IR_FLASH},
    {"R2",0x00F730CF,0x00FF02FD,30,0},{"G2",0x00F7B04F,0x00FF32CD,150,0},
    {"B2",0x00F7708F,0x00FF20DF,270,0},{"STROBE",0x00F7F00F,0x00FF00FF,-1,IR_STROBE},
    {"R3",0x00F708F7,0x00FF50AF,45,0},{"G3",0x00F78877,0x00FF7887,175,0},
    {"B3",0x00F748B7,0x00FF708F,300,0},{"FADE",0x00F7C837,0x00FF58A7,-1,IR_FADE},
    {"R4",0x00F728D7,0x00FF38C7,60,0},{"G4",0x00F7A857,0x00FF28D7,185,0},
    {"B4",0x00F76897,0x00FFF00F,330,0},{"SMOOTH",0x00F7E817,0x00FF30CF,-1,IR_SMOOTH},
};
static bool near(unsigned value,unsigned nominal,unsigned margin)
{ return value>=nominal-margin && value<=nominal+margin; }
ir_frame_t ir_decode(const ir_pulse_t *p,size_t count,uint32_t *code)
{
    if (!p || !code) return IR_INVALID;
    while (count && !p[count-1].us) count--;
    if (count && p[0].high) { p++; count--; } // Optional leading idle interval.
    if (count!=3 && count!=4 && count!=67 && count!=68) return IR_INVALID;
    for (size_t i=0;i<count;i++) if (p[i].high!=(bool)(i&1)) return IR_INVALID;
    if (!near(p[0].us,9000,1600)) return IR_INVALID;
    if (count<=4) return near(p[1].us,2250,400) && near(p[2].us,560,220)?IR_REPEAT:IR_INVALID;
    if (!near(p[1].us,4500,750) || !near(p[66].us,560,220)) return IR_INVALID;
    uint32_t raw=0;
    for (int i=0;i<32;i++) {
        if (!near(p[2+2*i].us,560,220)) return IR_INVALID;
        unsigned space=p[3+2*i].us;
        bool bit=near(space,1690,400);
        if (!bit && !near(space,560,220)) return IR_INVALID;
        raw=(raw<<1)|bit;
    }
    // Accept NEC and extended addresses; the command must still have its inverse byte.
    if (((raw^(raw>>8))&255)!=255) return IR_INVALID;
    *code=raw; return IR_FRAME;
}
int ir_match(const uint32_t codes[IR_KEYS],uint32_t code)
{
    if (!code) return -1;
    for (int i=0;i<IR_KEYS;i++) if (codes[i]==code) return i;
    for (int i=0;i<IR_KEYS;i++) if (codes[i]==ir_keys[i].code && code==ir_keys[i].old_code) return i;
    return -1;
}
bool ir_key_patch(int key,light_values_t current,const remote_config_t *remote,cJSON *patch)
{
    if (key<0 || key>=IR_KEYS || !patch || ir_keys[key].effect) return false;
    if (key<=1) {
        cJSON_AddNumberToObject(patch,"level",ir_brightness_step(current.level,key==0?1:-1,remote));
    } else if (key==2 || key==3) cJSON_AddBoolToObject(patch,"on",key==3);
    else {
        cJSON_AddBoolToObject(patch,"on",true);
        cJSON_AddNumberToObject(patch,"saturation",key==7?0:100);
        if (key==7) { cJSON_AddNumberToObject(patch,"kelvin",current.kelvin);cJSON_AddNumberToObject(patch,"level",current.level); }
        else cJSON_AddNumberToObject(patch,"hue",ir_keys[key].hue);
    }
    return true;
}
void ir_default_scale(uint8_t percent[IR_LEVELS],unsigned count)
{
    static const uint8_t normal[]={1,2,4,7,12,20,32,50,75,100};
    if(count<2 || count>IR_LEVELS)return;
    if(count==10) { memcpy(percent,normal,sizeof(normal));return; }
    int previous=0;
    for(unsigned i=0;i<count;i++) {
        int value=(int)lroundf(powf(100.0f,(float)i/(count-1)));
        if(value<=previous)value=previous+1;
        if(value>100-(int)(count-1-i))value=100-(int)(count-1-i);
        percent[i]=(uint8_t)value;previous=value;
    }
}
void remote_default(remote_config_t *r,unsigned count)
{
    memset(r,0,sizeof(*r));r->version=1;r->count=count>=2 && count<=IR_LEVELS?count:10;ir_default_scale(r->percent,r->count);
}
bool remote_valid(const remote_config_t *r)
{
    if(r->version!=1 || r->count<2 || r->count>IR_LEVELS || r->member_count>MAX_LIGHTS+1)return false;
    for(unsigned i=0;i<r->count;i++)if(r->percent[i]<1 || r->percent[i]>100 || (i && r->percent[i]<=r->percent[i-1]))return false;
    for(unsigned i=0;i<r->member_count;i++) {
        if(!memchr(r->members[i],0,ID_LEN) || (strcmp(r->members[i],"display") && !valid_light_id(r->members[i])))return false;
        for(unsigned j=0;j<i;j++)if(!strcmp(r->members[i],r->members[j]))return false;
    }
    return true;
}
int ir_brightness_step(int level,int direction,const remote_config_t *r)
{
    if(r->count<2 || r->count>IR_LEVELS)return level;
    int previous=(r->percent[0]*254+50)/100;
    for(unsigned i=0;i<r->count;i++) {
        int value=(r->percent[i]*254+50)/100;
        if(direction>0 && value>level)return value;
        if(direction<0 && value>=level)return previous;
        previous=value;
    }
    return previous;
}
