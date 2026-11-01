#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_rpc.h"

static int actions;
static cJSON *command(const char *op,const cJSON *args,const char **error,void *context)
{
    (void)args;(void)context;
    if(strcmp(op,"control")) { *error="unknown_operation";return NULL; }
    actions++;cJSON *o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"rev",actions);return o;
}
static void expect_error(const char *reply,const char *expected)
{
    cJSON *o=cJSON_Parse(reply);assert(o);assert(cJSON_IsFalse(cJSON_GetObjectItem(o,"ok")));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(o,"error")),expected));cJSON_Delete(o);
}
static void transfer(const char *text,unsigned mtu)
{
    ble_message_t m={0};uint8_t packet[247];size_t total=strlen(text);
    for(size_t offset=0;offset<total;) {
        size_t n=ble_message_frame(text,total,offset,mtu,packet,sizeof(packet));assert(n>=5 && n<=mtu-3);
        int state=ble_message_feed(&m,packet,n,100+offset);offset+=n-4;
        assert(state==(offset==total?1:0));
    }
    assert(m.received==total && !strcmp(m.data,text));
}
int main(void)
{
    const char *request="{\"v\":1,\"id\":42,\"op\":\"control\",\"args\":{\"target\":\"display\",\"patch\":{\"level\":254}}}";
    transfer(request,23);transfer(request,247);
    char full[BLE_MESSAGE_MAX+1];memset(full,'x',BLE_MESSAGE_MAX);full[BLE_MESSAGE_MAX]=0;
    // Split a UTF-8 character across minimum-MTU frames; reassembly preserves bytes.
    memcpy(full+15,"\xc5\xbe\xc3\xa1rovka",9);transfer(full,23);transfer(full,247);
    ble_message_t m={0};uint8_t first[23],second[23];
    size_t n=ble_message_frame(request,strlen(request),0,23,first,sizeof(first));
    size_t n2=ble_message_frame(request,strlen(request),16,23,second,sizeof(second));
    assert(ble_message_feed(&m,second,n2,0)==-1 && !m.received);
    assert(ble_message_feed(&m,first,n,1)==0);
    assert(ble_message_feed(&m,first,n,2)==-1); // duplicate/out-of-order fragment
    assert(ble_message_feed(&m,first,n,3)==0);
    assert(ble_message_feed(&m,second,n2,5003)==-1); // timeout cannot dispatch partial JSON
    assert(ble_message_feed(&m,first,4,6000)==-1);
    first[0]=1;first[1]=7;assert(ble_message_feed(&m,first,n,6000)==-1); // oversized message
    first[0]=1;first[1]=0;first[4]=0;assert(ble_message_feed(&m,first,5,6000)==-1);
    first[4]=0xff;assert(ble_message_feed(&m,first,5,6000)==-1); // malformed UTF-8
    assert(!ble_message_frame(request,strlen(request),0,22,first,sizeof(first)));
    ble_rpc_t cache={0};char error_reply[BLE_RPC_ERROR_MAX],saved[BLE_MESSAGE_MAX+1];const char *reply;
    reply=ble_rpc_execute(&cache,request,error_reply,command,NULL);assert(actions==1);strcpy(saved,reply);
    reply=ble_rpc_execute(&cache,request,error_reply,command,NULL);assert(actions==1 && !strcmp(saved,reply));
    reply=ble_rpc_execute(&cache,"{\"v\":1,\"id\":42,\"op\":\"control\",\"args\":{}}",error_reply,command,NULL);
    expect_error(reply,"id_reused");assert(actions==1);
    reply=ble_rpc_execute(&cache,request,error_reply,command,NULL);assert(actions==1 && !strcmp(saved,reply));
    const char *invalid[]={"{}","{", "{\"v\":2,\"id\":43,\"op\":\"control\",\"args\":{}}",
        "{\"v\":1,\"id\":43.5,\"op\":\"control\",\"args\":{}}", "{\"v\":1,\"id\":43,\"id\":44,\"op\":\"control\",\"args\":{}}",
        "{\"v\":1,\"id\":43,\"op\":\"control\",\"args\":null}","{\"v\":1,\"id\":43,\"op\":\"control\",\"args\":{},\"extra\":1}"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {reply=ble_rpc_execute(&cache,invalid[i],error_reply,command,NULL);expect_error(reply,"invalid_request");}
    assert(actions==1);
    reply=ble_rpc_execute(&cache,"{\"v\":1,\"id\":43,\"op\":\"settings\",\"args\":{}}",error_reply,command,NULL);
    expect_error(reply,"unknown_operation");assert(actions==1);
    memset(&cache,0,sizeof(cache));reply=ble_rpc_execute(&cache,request,error_reply,command,NULL);assert(actions==2);
    puts("BLE: MTU 23/247, max-length UTF-8 fragmentation, invalid frames, timeout, envelope validation and idempotent commands verified.");
    return 0;
}
