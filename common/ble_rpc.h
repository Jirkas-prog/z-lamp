#pragma once
#include "ble_wire.h"
#include "cJSON.h"

typedef struct {
    int last_id;
    char request[BLE_MESSAGE_MAX+1],reply[BLE_MESSAGE_MAX+1];
} ble_rpc_t;
typedef cJSON *(*ble_rpc_handler_t)(const char *op,const cJSON *args,const char **error,void *context);
// Caller serializes requests and discards this cache at every disconnect.
#define BLE_RPC_ERROR_MAX 128
// Reply points into the cache or error buffer; valid until the next execution.
const char *ble_rpc_execute(ble_rpc_t *rpc,const char *request,char error_reply[BLE_RPC_ERROR_MAX],
                     ble_rpc_handler_t handler,void *context);
