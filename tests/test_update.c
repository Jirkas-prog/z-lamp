// Exercise production OTA state transitions with the flash and SHA APIs replaced.
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "model.h"
#include "wire.h"
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
typedef int esp_err_t;
typedef unsigned esp_ota_handle_t;
typedef struct { unsigned size,address; } esp_partition_t;
typedef int esp_ota_img_states_t;
typedef struct { int unused; } mbedtls_sha256_context;
typedef struct { uint8_t bytes[24]; } esp_image_header_t;
typedef struct { uint8_t bytes[8]; } esp_image_segment_header_t;
typedef struct { uint32_t magic_word,secure_version,reserv1[2];char version[32],project_name[32],time[16],date[16],idf_ver[32];uint8_t app_elf_sha256[32];uint32_t reserv2[20]; } esp_app_desc_t;
#define ESP_OK 0
#define ESP_OTA_IMG_PENDING_VERIFY 1
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffeu
#define ESP_LOGI(...) ((void)0)
static esp_partition_t running={0x180000,0x10000},spare={0x180000,0x1a0000};
static esp_app_desc_t description={.magic_word=ESP_APP_DESC_MAGIC_WORD,.version="2.7.0",.project_name="hue_lamp"};
static bool pending,invalid,wrong_hash;
static unsigned writes,aborts,ends,commits,begin_count;
static int64_t clock_us;
static const esp_partition_t *esp_ota_get_running_partition(void){return &running;}
static const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p){(void)p;return &spare;}
static const esp_app_desc_t *esp_app_get_description(void){return &description;}
static int esp_ota_get_state_partition(const esp_partition_t *p,int *s){(void)p;*s=pending?1:0;return 0;}
static int esp_ota_mark_app_valid_cancel_rollback(void){pending=false;return 0;}
static int64_t esp_timer_get_time(void){return clock_us;}
static int esp_ota_begin(const esp_partition_t *p,size_t n,unsigned *h){assert(p==&spare && n==OTA_WITH_SEQUENTIAL_WRITES);*h=42;begin_count++;return 0;}
static int esp_ota_write(unsigned h,const void *p,size_t n){assert(h==42 && p && n<=512);writes+=(unsigned)n;return 0;}
static int esp_ota_abort(unsigned h){assert(h==42);aborts++;return 0;}
static int esp_ota_end(unsigned h){assert(h==42);ends++;return invalid?-1:0;}
static int esp_ota_set_boot_partition(const esp_partition_t *p){assert(p==&spare);commits++;return 0;}
static void mbedtls_sha256_init(mbedtls_sha256_context *c){(void)c;}
static void mbedtls_sha256_free(mbedtls_sha256_context *c){(void)c;}
static int mbedtls_sha256_starts(mbedtls_sha256_context *c,int n){(void)c;assert(n==0);return 0;}
static int mbedtls_sha256_update(mbedtls_sha256_context *c,const void *p,size_t n){(void)c;(void)p;(void)n;return 0;}
static int mbedtls_sha256_finish(mbedtls_sha256_context *c,uint8_t *out){(void)c;memset(out,wrong_hash?1:0,32);return 0;}
#include "update-under-test.inc"
static update_t firmware_update;
static int update_sid;
static uint64_t update_last,update_reboot;
static uint64_t now_ms(void){return (uint64_t)clock_us/1000;}
#include "ota-command-under-test.inc"
int main(void)
{
#ifdef _MSC_VER
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);_set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    uint8_t header[UPDATE_HEADER]={0};memcpy(header,"ZLAMPOTA",8);header[8]=1;header[9]=2;header[13]=4;
    memcpy(header+48,"hue_lamp",9);memcpy(header+80,"2.7.0",6);update_meta_t meta;
    assert(update_header(header,&meta) && meta.size==1024 && meta.board==2);
    header[10]=1;assert(!update_header(header,&meta));header[10]=0;
    header[82]='6';assert(!update_header(header,&meta));header[82]='7';assert(update_header(header,&meta));
    char hex[UPDATE_HEADER*2+1];uint8_t decoded[UPDATE_HEADER];update_hex(header,sizeof(header),hex);
    assert(update_unhex(hex,decoded,sizeof(decoded))==UPDATE_HEADER && !memcmp(header,decoded,sizeof(header)));
    assert(update_unhex("0g",decoded,2)==-1 && update_unhex("a",decoded,2)==-1 && update_unhex(hex,decoded,1)==-1);
    update_t u={0};pending=true;assert(!strcmp(update_begin(&u,&meta,2),"update_boot_unconfirmed") && !begin_count);
    clock_us=29000000;update_health_tick(true);assert(pending);clock_us=31000000;update_health_tick(false);assert(pending);update_health_tick(true);assert(!pending);
    assert(!strcmp(update_begin(&u,&meta,1),"update_wrong_board") && !begin_count);
    spare.size=100;assert(!strcmp(update_begin(&u,&meta,2),"update_partition"));spare.size=0x180000;
    assert(!update_begin(&u,&meta,2));assert(!strcmp(update_begin(&u,&meta,2),"update_busy"));
    uint8_t chunk[512]={0};chunk[0]=0xe9;memcpy(chunk+32,&description,sizeof(description));
    assert(!update_write(&u,0,chunk,512));assert(u.received==512 && writes==512 && !commits);
    assert(update_write(&u,0,chunk,512) && update_write(&u,513,chunk,512));assert(update_finish(&u));
    update_abort(&u);assert(aborts==1 && !commits && !u.state); // Interrupted transfer cannot select another boot slot.
    assert(!update_begin(&u,&meta,2));chunk[80]='X';assert(!strcmp(update_write(&u,0,chunk,512),"update_image_header") && !u.received);chunk[80]='h';
    assert(!update_write(&u,0,chunk,512) && !update_write(&u,512,chunk,512));wrong_hash=true;
    assert(!strcmp(update_finish(&u),"update_checksum") && !commits && !u.state && !ends);wrong_hash=false;
    assert(!update_begin(&u,&meta,2));assert(!update_write(&u,0,chunk,512) && !update_write(&u,512,chunk,512));invalid=true;
    assert(!strcmp(update_finish(&u),"update_invalid_image") && !commits);invalid=false;
    assert(!update_begin(&u,&meta,2));assert(!update_write(&u,0,chunk,512) && !update_write(&u,512,chunk,512));
    assert(!update_finish(&u) && u.state==2 && !commits);assert(!update_commit(&u) && u.state==3 && commits==1);
    cJSON *request=cJSON_CreateObject();cJSON_AddStringToObject(request,"header",hex);
    assert(!update_command(request,"ota_begin",123) && update_sid==123 && update_last==31000);
    cJSON_Delete(request);request=cJSON_CreateObject();char data[1025];update_hex(chunk,sizeof(chunk),data);
    cJSON_AddNumberToObject(request,"offset",0);cJSON_AddStringToObject(request,"data",data);
    assert(!strcmp(update_command(request,"ota_data",124),"update_session") && !firmware_update.received);
    assert(!update_command(request,"ota_data",123) && firmware_update.received==512);
    assert(!strcmp(update_command(request,"ota_finish",123),"update_incomplete"));
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(request,"offset"),512);
    assert(!update_command(request,"ota_data",123) && !update_command(request,"ota_finish",123));
    assert(!update_command(request,"ota_commit",123) && update_reboot==35000);
    assert(!strcmp(update_command(request,"ota_abort",123),"update_restarting") && firmware_update.state==3);
    cJSON_AddStringToObject(request,"op","ota_data");cJSON_AddNumberToObject(request,"v",1);
    cJSON_AddNumberToObject(request,"sid",2147483647);cJSON_AddNumberToObject(request,"seq",2147483647);
    char json[WIRE_CAP],frame[WIRE_CAP];assert(cJSON_PrintPreallocated(request,json,sizeof(json),false));
    size_t length=wire_encode(json,frame,sizeof(frame));assert(length>1024 && length<WIRE_CAP);
    wire_rx_t rx={0};unsigned messages=0;
    for(size_t i=0;i<length;i++)if(wire_feed(&rx,(uint8_t)frame[i]))messages++;
    assert(messages==1 && !strcmp(rx.data,json));cJSON_Delete(request);
    puts("OTA: envelope, board/version/size, sequential offsets, interruption, image header, checksum, validation, commit ordering and boot confirmation passed.");
}
