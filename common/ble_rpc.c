#include "ble_rpc.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

const char *ble_rpc_execute(ble_rpc_t *rpc,const char *request,char error_reply[BLE_RPC_ERROR_MAX],ble_rpc_handler_t handler,void *context)
{
    cJSON *root=cJSON_ParseWithOpts(request,NULL,true),*data=NULL;
    const char *error=NULL,*op=NULL;int id=0;
    const cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"v"),*number=cJSON_GetObjectItemCaseSensitive(root,"id");
    const cJSON *args=cJSON_GetObjectItemCaseSensitive(root,"args");
    op=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root,"op"));
    if(cJSON_IsNumber(number) && number->valuedouble>=1 && number->valuedouble<=2147483647 && number->valuedouble==number->valueint)id=number->valueint;
    if(!cJSON_IsObject(root) || !cJSON_IsNumber(version) || version->valuedouble!=1 || !id || !op || strlen(op)>32 || !cJSON_IsObject(args))error="invalid_request";
    for(const cJSON *p=root?root->child:NULL;!error && p;p=p->next) {
        if(!p->string || (strcmp(p->string,"v") && strcmp(p->string,"id") && strcmp(p->string,"op") && strcmp(p->string,"args")))error="invalid_request";
        for(const cJSON *q=root->child;q!=p;q=q->next)if(!strcmp(q->string,p->string))error="invalid_request";
    }
    if(!error && rpc->last_id==id) {
        if(!strcmp(rpc->request,request)) { cJSON_Delete(root);return rpc->reply; }
        error="id_reused";
    }
    bool cache=!error;
    char *reply=cache?rpc->reply:error_reply;
    size_t capacity=cache?sizeof(rpc->reply):BLE_RPC_ERROR_MAX;
    if(!error)data=handler(op,args,&error,context);
    cJSON *out=cJSON_CreateObject();
    cJSON_AddNumberToObject(out,"v",1);cJSON_AddNumberToObject(out,"id",id);cJSON_AddBoolToObject(out,"ok",!error);
    if(error)cJSON_AddStringToObject(out,"error",error);
    else if(data) {
        const cJSON *revision=cJSON_GetObjectItemCaseSensitive(data,"rev");
        if(cJSON_IsNumber(revision))cJSON_AddNumberToObject(out,"rev",revision->valuedouble);
        cJSON_AddItemToObject(out,"data",data);data=NULL;
    }
    if(!out || !cJSON_PrintPreallocated(out,reply,capacity,false))
        snprintf(reply,capacity,"{\"v\":1,\"id\":%d,\"ok\":false,\"error\":\"reply_too_large\"}",id);
    if(cache) { rpc->last_id=id;snprintf(rpc->request,sizeof(rpc->request),"%s",request); }
    cJSON_Delete(data);cJSON_Delete(out);cJSON_Delete(root);
    return reply;
}
