#include "updates.h"
#include "app.h"
#include "update.h"
#include "ble.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static struct {
    unsigned board,received,total;
    bool cancel,committed;
    char phase[32],error[80],expected[65],version[32];
} status;
typedef struct { unsigned board;httpd_req_t *request;char url[1024]; } update_job_t;
static void phase(const char *name)
{ app_lock();snprintf(status.phase,sizeof(status.phase),"%s",name);app_unlock(); }
static bool cancelled(void) { app_lock();bool value=status.cancel;app_unlock();return value; }
static esp_err_t json_reply(httpd_req_t *r,const char *error)
{
    cJSON *o=cJSON_CreateObject();if(error)cJSON_AddStringToObject(o,"error",error);else cJSON_AddBoolToObject(o,"ok",true);
    char *body=cJSON_PrintUnformatted(o);cJSON_Delete(o);
    httpd_resp_set_status(r,error?"400 Bad Request":"200 OK");httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"Connection","close");
    esp_err_t result=httpd_resp_sendstr(r,body?body:"{}");free(body);return error?ESP_FAIL:result;
}
esp_err_t updates_status(httpd_req_t *r)
{
    static uint32_t boot; if(!boot)boot=esp_random()|1;
    cJSON *o=cJSON_CreateObject();char id[65];update_hex(esp_app_get_description()->app_elf_sha256,32,id);
    app_lock();
    cJSON_AddBoolToObject(o,"busy",app.updating);cJSON_AddNumberToObject(o,"target",status.board);
    cJSON_AddNumberToObject(o,"received",status.received);cJSON_AddNumberToObject(o,"total",status.total);
    cJSON_AddStringToObject(o,"phase",status.phase);cJSON_AddStringToObject(o,"error",status.error);
    cJSON_AddStringToObject(o,"expectedId",status.expected);cJSON_AddStringToObject(o,"expectedVersion",status.version);
    cJSON *gateway=cJSON_AddObjectToObject(o,"gateway"),*coordinator=cJSON_AddObjectToObject(o,"coordinator");
    cJSON_AddStringToObject(gateway,"version",esp_app_get_description()->version);cJSON_AddStringToObject(gateway,"id",id);
    cJSON_AddNumberToObject(gateway,"boot",boot);cJSON_AddBoolToObject(gateway,"confirmed",update_confirmed());
    cJSON_AddStringToObject(coordinator,"version",app.c6_version);cJSON_AddStringToObject(coordinator,"id",app.c6_firmware);
    cJSON_AddNumberToObject(coordinator,"boot",app.c6_boot);cJSON_AddBoolToObject(coordinator,"confirmed",app.c6_confirmed);
    cJSON_AddBoolToObject(coordinator,"available",app.ota_api==1 && app.c6_last_ms && app_now()-app.c6_last_ms<6000);
    app_unlock();
    char *body=cJSON_PrintUnformatted(o);cJSON_Delete(o);httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t result=httpd_resp_sendstr(r,body?body:"{}");free(body);return result;
}
static const char *claim(unsigned board)
{
    if(board!=1 && board!=2)return "update_target";
    app_lock();
    const char *error=app.updating?"update_busy":app.restart_ms?"update_restarting":board==2 && (app.ota_api!=1 || !app.c6_last_ms || app_now()-app.c6_last_ms>=6000)?"coordinator_update_required":NULL;
    if(!error) { memset(&status,0,sizeof(status));status.board=board;app.updating=true;app.ir_effect=IR_STATIC;app.ir_generation++;app.ir_revision++; }
    app_unlock();return error;
}
const char *updates_cancel(void)
{
    app_lock();const char *error=status.committed?"update_restarting":NULL;if(!error)status.cancel=true;app_unlock();return error;
}
static bool uart(cJSON *o,char error[80])
{ bool ok=bridge_request(o,error);cJSON_Delete(o);return ok; }
static cJSON *operation(const char *op)
{ cJSON *o=cJSON_CreateObject();cJSON_AddStringToObject(o,"op",op);return o; }
static esp_err_t http_event(esp_http_client_event_t *event)
{
    if(event->user_data && event->event_id==HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key,"Location")) {
        char *location=event->user_data;snprintf(location,1024,"%s",event->header_value);
    }
    return ESP_OK;
}
static esp_http_client_handle_t download(const char *url,char error[80])
{
    static bool sntp;
    if(!sntp) { esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,"pool.ntp.org");esp_sntp_init();sntp=true; }
    phase("clock");uint64_t deadline=app_now()+15000;
    while(time(NULL)<1700000000 && app_now()<deadline && !cancelled())vTaskDelay(pdMS_TO_TICKS(100));
    if(cancelled()) { snprintf(error,80,"update_cancelled");return NULL; }
    if(time(NULL)<1700000000) { snprintf(error,80,"update_clock");return NULL; }
    char current[1024],location[1024];snprintf(current,sizeof(current),"%s",url);
    phase("download");
    ESP_LOGI("update","HTTPS heap: free=%u, largest=%u",(unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    for(int redirects=0;redirects<4 && !cancelled();redirects++) {
        if(strncmp(current,"https://",8))break;
        location[0]=0;
        esp_http_client_config_t config={.url=current,.crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=12000,
            .disable_auto_redirect=true,.event_handler=http_event,.user_data=location,.buffer_size=1024};
        esp_http_client_handle_t client=esp_http_client_init(&config);
        if(!client)break;
        if(esp_http_client_open(client,0)==ESP_OK) {
            esp_http_client_fetch_headers(client);int code=esp_http_client_get_status_code(client);
            if(code==200) { esp_http_client_set_user_data(client,NULL);return client; }
            if((code==301 || code==302 || code==303 || code==307 || code==308) && !strncmp(location,"https://",8)) {
                snprintf(current,sizeof(current),"%s",location);esp_http_client_cleanup(client);continue;
            }
        }
        esp_http_client_cleanup(client);break;
    }
    snprintf(error,80,"%s",cancelled()?"update_cancelled":"update_https_failed");return NULL;
}
static bool read_exact(update_job_t *job,esp_http_client_handle_t client,uint8_t *buffer,size_t count)
{
    size_t received=0;
    while(received<count && !cancelled()) {
        int n=client?esp_http_client_read(client,(char *)buffer+received,(int)(count-received)):
            httpd_req_recv(job->request,(char *)buffer+received,count-received);
        if(n<=0)return false;
        received+=(size_t)n;
    }
    return received==count;
}
static void worker(void *arg)
{
    update_job_t *job=arg;update_t local={0};update_meta_t meta;
    esp_http_client_handle_t client=NULL;char error[80]={0};const char *failure=NULL;
    bool remote_started=false;int old_boot;
    xSemaphoreTake(command_mutex,portMAX_DELAY);app_lock();old_boot=app.c6_boot;app_unlock();
    uint8_t header[UPDATE_HEADER],bytes[UPDATE_CHUNK];char hex[UPDATE_CHUNK*2+1];
    if(!failure && !job->request) { client=download(job->url,error);if(!client)failure=error; }
    if(!failure && (!read_exact(job,client,header,sizeof(header)) || !update_header(header,&meta)))failure=cancelled()?"update_cancelled":"update_header";
    if(!failure && meta.board!=job->board)failure="update_wrong_board";
    int64_t content_length=client?esp_http_client_get_content_length(client):job->request?job->request->content_len:-1;
    if(!failure && !(client && esp_http_client_is_chunked_response(client)) && content_length>=0 && content_length!=(int64_t)meta.size+UPDATE_HEADER)failure="update_size";
    if(!failure) {
        app_lock();status.total=meta.size;snprintf(status.version,sizeof(status.version),"%s",meta.version);update_hex(meta.elf,32,status.expected);app_unlock();phase("transfer");
        if(job->board==1)failure=update_begin(&local,&meta,1);
        else {
            update_hex(header,sizeof(header),hex);cJSON *o=operation("ota_begin");cJSON_AddStringToObject(o,"header",hex);
            if(!uart(o,error))failure=error;else remote_started=true;
        }
    }
    for(uint32_t offset=0;!failure && offset<meta.size;) {
        size_t n=meta.size-offset;if(n>sizeof(bytes))n=sizeof(bytes);
        if(!read_exact(job,client,bytes,n)) { failure=cancelled()?"update_cancelled":"update_interrupted";break; }
        if(job->board==1)failure=update_write(&local,offset,bytes,n);
        else {
            update_hex(bytes,n,hex);cJSON *o=operation("ota_data");cJSON_AddNumberToObject(o,"offset",offset);cJSON_AddStringToObject(o,"data",hex);
            if(!uart(o,error))failure=error;
        }
        if(!failure) { offset+=(uint32_t)n;app_lock();status.received=offset;app_unlock(); }
        vTaskDelay(1);
    }
    if(!failure && client) {
        char extra;int n=esp_http_client_read(client,&extra,1);
        if(n!=0 || !esp_http_client_is_complete_data_received(client))failure="update_size";
    }
    if(client)esp_http_client_cleanup(client);
    if(!failure && cancelled())failure="update_cancelled";
    if(!failure) {
        phase("validate");
        if(job->board==1)failure=update_finish(&local);
        else if(!uart(operation("ota_finish"),error))failure=error;
    }
    if(!failure) {
        app_lock();bool cancel=status.cancel;if(!cancel)status.committed=true;app_unlock();
        if(cancel)failure="update_cancelled";
        else {
            phase("restart");
            if(job->board==1)failure=update_commit(&local);
            else if(!uart(operation("ota_commit"),error))failure="update_commit_unknown";
        }
    }
    if(failure) {
        update_abort(&local);
        if(remote_started) { char ignored[80];uart(operation("ota_abort"),ignored); }
    }
    xSemaphoreGive(command_mutex);
    if(!failure && job->board==2) {
        phase("verify_boot");uint64_t deadline=app_now()+80000;bool confirmed=false;
        while(app_now()<deadline) {
            app_lock();confirmed=app.c6_boot!=old_boot && app.c6_confirmed && !strcmp(app.c6_firmware,status.expected) && app_now()-app.c6_last_ms<6000;app_unlock();
            if(confirmed)break;
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if(!confirmed)failure="update_boot_failed";
    }
    app_lock();
    snprintf(status.error,sizeof(status.error),"%s",failure?failure:"");snprintf(status.phase,sizeof(status.phase),"%s",failure?"error":job->board==1?"restart":"done");
    if(!failure && job->board==1)app.restart_ms=app_now()+2000;
    else app.updating=false;
    app_unlock();
    if(job->request) {
        httpd_handle_t server=job->request->handle;int socket=httpd_req_to_sockfd(job->request);
        json_reply(job->request,failure);httpd_req_async_handler_complete(job->request);httpd_sess_trigger_close(server,socket);
    }
    if(failure || job->board==2)ble_pause(false);
    free(job);vTaskDelete(NULL);
}
static const char *start(update_job_t *job)
{
    // BLE owns enough RAM to prevent allocation of the OTA stack. Stop it first,
    // outside command_mutex; an accepted BLE command may still own that mutex.
    phase("pause_ble");
    const char *failure=ble_pause(true)?NULL:"update_ble_busy";
    if(!failure && xTaskCreate(worker,"firmware",16384,job,4,NULL)==pdPASS)return NULL;
    if(!failure)failure="update_memory";
    ble_pause(false);
    app_lock();app.updating=false;
    snprintf(status.phase,sizeof(status.phase),"error");snprintf(status.error,sizeof(status.error),"%s",failure);
    app_unlock();return failure;
}
esp_err_t updates_upload(httpd_req_t *r)
{
    unsigned board=!strcmp(r->uri,"/api/update/gateway")?1:!strcmp(r->uri,"/api/update/coordinator")?2:0;
    if(r->content_len<UPDATE_HEADER+288 || r->content_len>UPDATE_HEADER+0x1e0000)return json_reply(r,"update_size");
    const char *failure=claim(board);if(failure)return json_reply(r,failure);
    update_job_t *job=calloc(1,sizeof(*job));
    if(!job) { app_lock();app.updating=false;app_unlock();return json_reply(r,"update_memory"); }
    job->board=board;
    if(httpd_req_async_handler_begin(r,&job->request)!=ESP_OK) { free(job);app_lock();app.updating=false;app_unlock();return json_reply(r,"update_memory"); }
    failure=start(job);
    if(failure) {
        int socket=httpd_req_to_sockfd(job->request);json_reply(job->request,failure);
        httpd_req_async_handler_complete(job->request);httpd_sess_trigger_close(r->handle,socket);free(job);
    }
    return ESP_OK;
}
const char *updates_url(const char *target,const char *url)
{
    unsigned board=target && !strcmp(target,"gateway")?1:target && !strcmp(target,"coordinator")?2:0;
    if(!url || strncmp(url,"https://",8) || strlen(url)>=1024 || strpbrk(url,"\r\n\t "))return "update_https_url";
    const char *failure=claim(board);if(failure)return failure;
    update_job_t *job=calloc(1,sizeof(*job));
    if(job) { job->board=board;snprintf(job->url,sizeof(job->url),"%s",url);failure=start(job); }
    else failure="update_memory";
    if(failure) { free(job);app_lock();app.updating=false;app_unlock(); }
    return failure;
}
