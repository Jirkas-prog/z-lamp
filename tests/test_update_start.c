#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct { int unused; } update_job_t;
static struct { bool updating; } app;
static struct { char phase[32],error[80]; } status;
static bool paused,stop_ok=true,task_ok=true;
static unsigned tasks;
#define pdPASS 1
static void app_lock(void) {}
static void app_unlock(void) {}
static void phase(const char *name) { snprintf(status.phase,sizeof(status.phase),"%s",name); }
static bool ble_pause(bool value) { paused=value;return !value || stop_ok; }
static void worker(void *arg) { (void)arg; }
static int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{
    (void)name;(void)arg;(void)priority;(void)handle;
    // Simulate the measured hardware: no 16 KB block exists until BLE stops.
    assert(paused && stop_ok && app.updating && fn==worker && stack==16384);
    tasks++;return task_ok?pdPASS:0;
}
#include "update-start-under-test.inc"
int main(void)
{
    update_job_t job={0};app.updating=true;
    assert(!start(&job) && paused && app.updating && tasks==1);
    paused=false;stop_ok=false;app.updating=true;
    assert(!strcmp(start(&job),"update_ble_busy"));
    assert(!paused && !app.updating && tasks==1 && !strcmp(status.error,"update_ble_busy"));
    stop_ok=true;task_ok=false;app.updating=true;
    assert(!strcmp(start(&job),"update_memory"));
    assert(!paused && !app.updating && tasks==2 && !strcmp(status.phase,"error"));
    task_ok=true;app.updating=true;
    assert(!start(&job) && paused && app.updating && tasks==3);
    puts("OTA startup: release BLE RAM before task allocation; resume BLE after either startup failure.");
}
