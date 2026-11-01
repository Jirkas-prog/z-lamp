#pragma once
#include "cJSON.h"

// Shared command validation and application actions for HTTP and BLE.
const char *app_api(const char *uri,const cJSON *args);
// Bounded, credential-free reads used by the BLE protocol. Caller owns result.
cJSON *app_query(const char *op,const cJSON *args,const char **error);
