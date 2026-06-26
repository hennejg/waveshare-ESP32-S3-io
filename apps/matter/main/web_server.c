#include "web_server.h"
#include "app_config.h"
#include "matter.h"
#include "di.h"
#include "dout.h"

#include <string.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>
#include "cJSON.h"

#define TAG "web_server"

static httpd_handle_t s_server = NULL;

static esp_err_t handle_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", app_config_get()->device_name);
    cJSON_AddBoolToObject(root, "commissioned", matter_is_commissioned());
    cJSON_AddStringToObject(root, "qr_code", matter_get_qr_code());
    cJSON_AddStringToObject(root, "manual_code", matter_get_manual_code());

    cJSON *di_arr = cJSON_AddArrayToObject(root, "di");
    cJSON *do_arr = cJSON_AddArrayToObject(root, "do");
    for (int i = 0; i < 8; i++) {
        cJSON_AddItemToArray(di_arr, cJSON_CreateBool(di_get(i)));
        cJSON_AddItemToArray(do_arr, cJSON_CreateBool(dout_get(i)));
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static const char INDEX_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<title>Waveshare Matter</title></head><body>"
    "<h1>Waveshare ESP32-S3 (Matter)</h1>"
    "<div id=s>Loading...</div>"
    "<script>"
    "fetch('/api/status').then(r=>r.json()).then(d=>{"
    "  var s=document.getElementById('s');"
    "  s.innerHTML='<p>Device: '+d.device_name+'</p>'"
    "  +(d.commissioned?'<p>Commissioned</p>'"
    "  :'<p>QR: <b>'+d.qr_code+'</b><br>Manual: <b>'+d.manual_code+'</b></p>');"
    "});"
    "</script></body></html>";

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, INDEX_HTML);
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = CONFIG_HTTPD_STACK_SIZE;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start failed");
    httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET, .handler = handle_root   },
        { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status },
    };
    for (int i = 0; i < 2; i++)
        httpd_register_uri_handler(s_server, &routes[i]);
    ESP_LOGI(TAG, "Matter web server started");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
}
