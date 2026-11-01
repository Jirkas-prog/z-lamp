#pragma once
#include <stdbool.h>
#include "cJSON.h"

void ble_start(void);
const char *ble_pair(bool physical);
const char *ble_configure(const cJSON *json);
cJSON *ble_status(void);
// Wait for the BLE worker to stop host/controller and release their heap before OTA.
bool ble_pause(bool pause);
