#include "scenes.h"
#include <stdio.h>
#include <string.h>

const scene_mode_t scene_modes[SCENE_MODES]={
    {"Duha plynule","Celý barevný kruh, bez bílé.",0,20},
    {"Barevné kroky","Šest sytých barev se střídá bez prolínání.",10,0},
    {"Barevný dech","Barva zeslábne, nadechne se a přejde na další odstín.",0,10},
    {"Barevný flash","Střídá syté barvy a zhasnutí.",10,0},
    {"Disko","Přeskakuje mezi kontrastními barvami.",7,0},
    {"Party rytmus","Barevné záblesky s proměnlivým jasem a pauzami.",5,0},
    {"Modrá a růžová","Plynulé přelévání dvou sytých odstínů.",5,25},
    {"Západ slunce","Červená, oranžová a purpurová.",10,30},
    {"Polární záře","Zelená, tyrkysová a fialová.",5,30},
    {"Oceán","Klidné proměny modré a tyrkysové.",10,30},
    {"Oheň","Teplé barevné odstíny s nepravidelným jasem.",5,2},
    {"Barevný běžec","Jasné světlo putuje mezi žárovkami v zadaném pořadí.",2,8},
    {"Vlastní paleta","Dvě až osm vlastních barev v opakující se smyčce.",5,20},
};
scene_t scene_default(void)
{
    scene_t s={.used=true,.use_ir=true,.mode=0,.level=127,.hold_ds=0,.fade_ds=20,.color_count=2,.hues={240,320}};
    snprintf(s.name,sizeof(s.name),"Moje scéna");return s;
}
void scenes_default(scene_settings_t *s)
{
    memset(s,0,sizeof(*s));s->version=1;s->white_kelvin=2700;s->white_level=127;s->brightness_steps=10;
    const unsigned modes[]={3,5,6,0};
    for(int i=0;i<4;i++) {
        s->buttons[i]=i;s->items[i]=scene_default();scene_t *p=&s->items[i];p->mode=modes[i];
        snprintf(p->name,sizeof(p->name),"%s",scene_modes[p->mode].name);
        p->hold_ds=scene_modes[p->mode].hold_ds;p->fade_ds=scene_modes[p->mode].fade_ds;
    }
}
unsigned scene_period_ms(const scene_t *s) { return 100*(s->hold_ds+s->fade_ds); }
bool scene_valid(const scene_t *s)
{
    if(!memchr(s->name,0,sizeof(s->name)) || !valid_name(s->name) || s->mode>=SCENE_MODES || s->level<1 || s->level>254
        || s->hold_ds>600 || s->fade_ds>100 || s->hold_ds+s->fade_ds<5
        || s->color_count<2 || s->color_count>SCENE_COLORS || s->member_count>MAX_LIGHTS+1)return false;
    for(unsigned i=0;i<s->color_count;i++)if(s->hues[i]>=360)return false;
    for(unsigned i=0;i<s->member_count;i++) {
        if(!memchr(s->members[i].id,0,ID_LEN) || (strcmp(s->members[i].id,"display") && !valid_light_id(s->members[i].id)) || s->members[i].offset>31)return false;
        for(unsigned j=0;j<i;j++)if(!strcmp(s->members[i].id,s->members[j].id))return false;
    }
    return true;
}
bool scenes_valid(const scene_settings_t *s)
{
    if(s->version!=1 || s->white_kelvin<2000 || s->white_kelvin>6500 || s->white_level<1 || s->white_level>254
        || s->brightness_steps<4 || s->brightness_steps>32)return false;
    for(int i=0;i<MAX_SCENES;i++)if(s->items[i].used && !scene_valid(&s->items[i]))return false;
    for(int i=0;i<4;i++)if(s->buttons[i]<-1 || s->buttons[i]>=MAX_SCENES || (s->buttons[i]>=0 && !s->items[s->buttons[i]].used))return false;
    return true;
}
light_values_t scene_value(const scene_t *s,uint64_t step,unsigned member,unsigned members,unsigned offset)
{
    static const unsigned rainbow[]={0,30,60,90,120,150,180,210,240,270,300,330};
    static const unsigned rgb[]={0,60,120,180,240,300},disco[]={0,240,60,300,120,180};
    static const unsigned pink[]={240,320},sunset[]={350,10,30,330},aurora[]={100,140,175,250},ocean[]={180,205,230,200},fire[]={5,25,15,35};
    const unsigned *palette=rainbow;unsigned count=12;
    switch(s->mode) {
    case 1:case 2:case 3:case 5:palette=rgb;count=6;break;
    case 4:palette=disco;count=6;break;
    case 6:palette=pink;count=2;break;
    case 7:palette=sunset;count=4;break;
    case 8:palette=aurora;count=4;break;
    case 9:palette=ocean;count=4;break;
    case 10:palette=fire;count=4;break;
    case SCENE_CUSTOM:palette=s->hues;count=s->color_count;break;
    }
    uint64_t phase=step+offset,index=phase;unsigned gain=100;bool on=true;
    if(s->mode==2) { static const unsigned breath[]={100,65,30,8,8,30,65,100};gain=breath[phase%8];index=phase/8; }
    if(s->mode==3) { on=phase%2==0;index=phase/2; }
    if(s->mode==5) { static const unsigned beat[]={100,0,100,30,0,80};gain=beat[phase%6];on=gain!=0;index=phase/2; }
    if(s->mode==10) { static const unsigned flicker[]={95,60,85,70,100,55,80,65};gain=flicker[phase%8]; }
    if(s->mode==11 && members)gain=step%members==member?100:8;
    int level=(int)(s->level*gain/100);if(level<1)level=1;
    return (light_values_t){on,level,2700,(int)palette[index%count],100};
}
int scene_index(const char *id) { return id && id[0]=='s' && id[1]>='1' && id[1]<='8' && !id[2]?id[1]-'1':-1; }
bool scene_parse(scene_t *out,const cJSON *o)
{
    static const char *const names[]={"id","name","mode","level","hold","fade","useIr","stagger","hues","members",NULL};
    if(!cJSON_IsObject(o))return false;
    for(const cJSON *p=o->child;p;p=p->next) {
        bool found=false;for(int i=0;names[i];i++)if(!strcmp(names[i],p->string))found=true;
        if(!found)return false;
        for(const cJSON *q=o->child;q!=p;q=q->next)if(!strcmp(q->string,p->string))return false;
    }
    scene_t s={.used=true};int n;
    const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"name"));if(!valid_name(name))return false;
    snprintf(s.name,sizeof(s.name),"%s",name);
    const char *fields[]={"mode","level","hold","fade"};unsigned *dest[]={&s.mode,&s.level,&s.hold_ds,&s.fade_ds};
    const int max[]={SCENE_MODES-1,254,600,100};
    for(int i=0;i<4;i++){if(!json_integer(cJSON_GetObjectItemCaseSensitive(o,fields[i]),0,max[i],&n))return false;*dest[i]=(unsigned)n;}
    const cJSON *use=cJSON_GetObjectItemCaseSensitive(o,"useIr"),*stagger=cJSON_GetObjectItemCaseSensitive(o,"stagger");
    if(!cJSON_IsBool(use)||!cJSON_IsBool(stagger))return false;
    s.use_ir=cJSON_IsTrue(use);s.stagger=cJSON_IsTrue(stagger);
    const cJSON *hues=cJSON_GetObjectItemCaseSensitive(o,"hues"),*members=cJSON_GetObjectItemCaseSensitive(o,"members");
    if(!cJSON_IsArray(hues)||cJSON_GetArraySize(hues)>SCENE_COLORS || !cJSON_IsArray(members)||cJSON_GetArraySize(members)>MAX_LIGHTS+1)return false;
    s.color_count=(unsigned)cJSON_GetArraySize(hues);s.member_count=(unsigned)cJSON_GetArraySize(members);
    for(unsigned i=0;i<s.color_count;i++){if(!json_integer(cJSON_GetArrayItem(hues,i),0,359,&n))return false;s.hues[i]=(unsigned)n;}
    for(unsigned i=0;i<s.member_count;i++) {
        const cJSON *m=cJSON_GetArrayItem(members,i);if(!cJSON_IsArray(m)||cJSON_GetArraySize(m)!=2)return false;
        const char *id=cJSON_GetStringValue(cJSON_GetArrayItem(m,0));if(!id || strlen(id)>=ID_LEN)return false;
        snprintf(s.members[i].id,ID_LEN,"%s",id);if(!json_integer(cJSON_GetArrayItem(m,1),0,31,&n))return false;s.members[i].offset=(unsigned)n;
    }
    if(!scene_valid(&s) || (!s.use_ir && !s.member_count))return false;
    *out=s;return true;
}
cJSON *scene_json(const scene_t *s,int index)
{
    cJSON *o=cJSON_CreateObject();char id[16];snprintf(id,sizeof(id),"s%d",index+1);
    cJSON_AddStringToObject(o,"id",id);cJSON_AddStringToObject(o,"name",s->name);
    cJSON_AddNumberToObject(o,"mode",s->mode);cJSON_AddNumberToObject(o,"level",s->level);
    cJSON_AddNumberToObject(o,"hold",s->hold_ds);cJSON_AddNumberToObject(o,"fade",s->fade_ds);
    cJSON_AddBoolToObject(o,"useIr",s->use_ir);cJSON_AddBoolToObject(o,"stagger",s->stagger);
    cJSON *hues=cJSON_AddArrayToObject(o,"hues"),*members=cJSON_AddArrayToObject(o,"members");
    for(unsigned i=0;i<s->color_count;i++)cJSON_AddItemToArray(hues,cJSON_CreateNumber(s->hues[i]));
    for(unsigned i=0;i<s->member_count;i++) {
        cJSON *m=cJSON_CreateArray();cJSON_AddItemToArray(m,cJSON_CreateString(s->members[i].id));cJSON_AddItemToArray(m,cJSON_CreateNumber(s->members[i].offset));cJSON_AddItemToArray(members,m);
    }
    return o;
}
