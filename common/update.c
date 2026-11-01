#include "update.h"
#include <string.h>
#include <stdio.h>
#include "esp_app_format.h"
#include "esp_timer.h"
#include "esp_log.h"

bool update_header(const uint8_t p[UPDATE_HEADER],update_meta_t *m)
{
    if(memcmp(p,"ZLAMPOTA",8) || p[8]!=1 || (p[9]!=1 && p[9]!=2) || p[10] || p[11])return false;
    memset(m,0,sizeof(*m));m->board=p[9];m->size=(uint32_t)p[12]|(uint32_t)p[13]<<8|(uint32_t)p[14]<<16|(uint32_t)p[15]<<24;
    memcpy(m->sha,p+16,32);memcpy(m->project,p+48,32);memcpy(m->version,p+80,32);memcpy(m->elf,p+112,32);
    if(m->size<288 || m->size>(m->board==1?0x1e0000:0x180000) || !memchr(m->project,0,32) || !memchr(m->version,0,32)
        || strcmp(m->project,m->board==1?"lamp_gateway":"hue_lamp"))return false;
    unsigned major,minor;
    return sscanf(m->version,"%u.%u",&major,&minor)==2 && (major>2 || (major==2 && minor>=7));
}
void update_hex(const uint8_t *p,size_t n,char *out)
{
    static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<n;i++){out[2*i]=digits[p[i]>>4];out[2*i+1]=digits[p[i]&15];}out[2*n]=0;
}
int update_unhex(const char *s,uint8_t *out,size_t cap)
{
    if(!s)return -1;
    size_t n=strlen(s);if(n%2 || n/2>cap)return -1;
    for(size_t i=0;i<n;i++) {
        int v=s[i]>='0' && s[i]<='9'?s[i]-'0':s[i]>='a' && s[i]<='f'?s[i]-'a'+10:-1;
        if(v<0)return -1;
        if(i%2)out[i/2]|=(uint8_t)v;else out[i/2]=(uint8_t)(v<<4);
    }
    return (int)n/2;
}
bool update_confirmed(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(),&state)!=ESP_OK || state!=ESP_OTA_IMG_PENDING_VERIFY;
}
void update_health_tick(bool healthy)
{
    static bool checked;
    if(!checked && healthy && esp_timer_get_time()>=30000000) {
        if(!update_confirmed()) {
            if(esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK)return;
            ESP_LOGI("update","New firmware passed startup check; rollback cancelled");
        }
        checked=true;
    }
}
void update_abort(update_t *u)
{
    if(u->state==1)esp_ota_abort(u->handle);
    mbedtls_sha256_free(&u->hash);memset(u,0,sizeof(*u));
}
const char *update_begin(update_t *u,const update_meta_t *m,unsigned board)
{
    if(u->state)return "update_busy";
    if(!update_confirmed())return "update_boot_unconfirmed";
    if(m->board!=board || strcmp(m->project,esp_app_get_description()->project_name))return "update_wrong_board";
    const esp_partition_t *partition=esp_ota_get_next_update_partition(NULL);
    if(!partition || partition==esp_ota_get_running_partition() || m->size>partition->size)return "update_partition";
    u->meta=*m;u->partition=partition;u->received=0;
    // Sequential erase keeps individual writes bounded, especially on the UART receiver.
    if(esp_ota_begin(partition,OTA_WITH_SEQUENTIAL_WRITES,&u->handle)!=ESP_OK)return "update_begin_failed";
    mbedtls_sha256_init(&u->hash);u->state=1;
    if(mbedtls_sha256_starts(&u->hash,0)!=0){update_abort(u);return "update_hash_failed";}
    return NULL;
}
const char *update_write(update_t *u,uint32_t offset,const uint8_t *p,size_t n)
{
    if(u->state!=1 || offset!=u->received || !n || n>UPDATE_CHUNK || n>u->meta.size-u->received)return "update_offset";
    if(!offset) {
        if(n<sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t))return "update_image_header";
        esp_app_desc_t desc;memcpy(&desc,p+sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t),sizeof(desc));
        if(p[0]!=ESP_IMAGE_HEADER_MAGIC || desc.magic_word!=ESP_APP_DESC_MAGIC_WORD || memcmp(desc.project_name,u->meta.project,32)
            || memcmp(desc.version,u->meta.version,32) || memcmp(desc.app_elf_sha256,u->meta.elf,32))return "update_image_header";
    }
    if(esp_ota_write(u->handle,p,n)!=ESP_OK)return "update_write_failed";
    if(mbedtls_sha256_update(&u->hash,p,n)!=0)return "update_hash_failed";
    u->received+=(uint32_t)n;return NULL;
}
const char *update_finish(update_t *u)
{
    if(u->state!=1 || u->received!=u->meta.size)return "update_incomplete";
    uint8_t sha[32];
    if(mbedtls_sha256_finish(&u->hash,sha)!=0 || memcmp(sha,u->meta.sha,32)){update_abort(u);return "update_checksum";}
    mbedtls_sha256_free(&u->hash);
    esp_err_t result=esp_ota_end(u->handle);u->handle=0;u->state=0;
    if(result!=ESP_OK)return "update_invalid_image";
    u->state=2;return NULL;
}
const char *update_commit(update_t *u)
{
    if(u->state!=2)return "update_not_verified";
    if(esp_ota_set_boot_partition(u->partition)!=ESP_OK)return "update_select_failed";
    u->state=3;return NULL;
}
