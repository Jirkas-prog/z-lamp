#pragma once
#include "esp_http_server.h"
esp_err_t updates_status(httpd_req_t *request);
esp_err_t updates_upload(httpd_req_t *request);
const char *updates_url(const char *target,const char *url);
const char *updates_cancel(void);
