#include "web_server.h"
#include "app_config.h"
#include "di.h"
#include "dout.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include "cJSON.h"

#define TAG        "web_server"
#define WWW_BASE   "/www"
#define CHUNK_SIZE  4096
#define BODY_MAX    2048  /* names add ~768 bytes to the di/dout arrays */

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------ helpers */

static const char *mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcmp(ext, ".html")) return "text/html";
    if (!strcmp(ext, ".js"))   return "application/javascript";
    if (!strcmp(ext, ".css"))  return "text/css";
    if (!strcmp(ext, ".json")) return "application/json";
    if (!strcmp(ext, ".ico"))  return "image/x-icon";
    if (!strcmp(ext, ".png"))  return "image/png";
    if (!strcmp(ext, ".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

/* Parse a cJSON array of {invert, name} objects into a di_config_t array. */
static void parse_invert_array(cJSON *arr, di_config_t *out, int max)
{
    if (!cJSON_IsArray(arr)) return;
    int n = cJSON_GetArraySize(arr);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;
        cJSON *inv  = cJSON_GetObjectItem(item, "invert");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsBool(inv))    out[i].invert = cJSON_IsTrue(inv);
        if (cJSON_IsString(name)) strlcpy(out[i].name, name->valuestring, sizeof(out[i].name));
    }
}

/* Validate names: no '/', unique within the array (empty names ignored). */
static bool validate_names(const di_config_t *arr, int count, const char **err_msg)
{
    for (int i = 0; i < count; i++) {
        if (!arr[i].name[0]) continue;
        if (strchr(arr[i].name, '/')) {
            *err_msg = "name must not contain '/'";
            return false;
        }
        for (int j = i + 1; j < count; j++) {
            if (arr[j].name[0] && strcmp(arr[i].name, arr[j].name) == 0) {
                *err_msg = "names must be unique";
                return false;
            }
        }
    }
    return true;
}

/* ----------------------------------------------------------------- /api/config */

static esp_err_t api_config_get(httpd_req_t *req)
{
    const app_config_t *cfg = app_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name",       cfg->device_name);
    cJSON_AddStringToObject(root, "mqtt_url",          cfg->mqtt_url);
    cJSON_AddStringToObject(root, "mqtt_user",         cfg->mqtt_user);
    /* Never expose the password — return a flag instead. */
    cJSON_AddBoolToObject  (root, "mqtt_password_set", cfg->mqtt_password[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);

    cJSON *mb = cJSON_AddObjectToObject(root, "modbus");
    cJSON_AddBoolToObject  (mb, "enable",   cfg->modbus.enable);
    cJSON_AddNumberToObject(mb, "address",  cfg->modbus.address);
    cJSON_AddNumberToObject(mb, "baudrate", cfg->modbus.baudrate);

    cJSON *di = cJSON_AddArrayToObject(root, "di");
    for (int i = 0; i < APP_CFG_DI_COUNT; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddBoolToObject  (item, "invert", cfg->di[i].invert);
        cJSON_AddStringToObject(item, "name",   cfg->di[i].name);
        cJSON_AddItemToArray(di, item);
    }

    cJSON *dout = cJSON_AddArrayToObject(root, "dout");
    for (int i = 0; i < APP_CFG_DO_COUNT; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddBoolToObject  (item, "invert", cfg->dout[i].invert);
        cJSON_AddStringToObject(item, "name",   cfg->dout[i].name);
        cJSON_AddItemToArray(dout, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t api_config_post(httpd_req_t *req)
{
    if (req->content_len > BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    app_config_t cfg;
    memcpy(&cfg, app_config_get(), sizeof(cfg));

    /* String fields — macro to keep it terse */
    #define STR(key, dst) do { \
        cJSON *_v = cJSON_GetObjectItem(root, key); \
        if (cJSON_IsString(_v)) strlcpy(dst, _v->valuestring, sizeof(dst)); \
    } while (0)

    STR("device_name",      cfg.device_name);
    STR("mqtt_url",         cfg.mqtt_url);
    STR("mqtt_user",        cfg.mqtt_user);
    STR("mqtt_topic_prefix",cfg.mqtt_topic_prefix);
    #undef STR

    /* Password: only update when a non-empty value is sent */
    cJSON *pass = cJSON_GetObjectItem(root, "mqtt_password");
    if (cJSON_IsString(pass) && pass->valuestring[0])
        strlcpy(cfg.mqtt_password, pass->valuestring, sizeof(cfg.mqtt_password));

    parse_invert_array(cJSON_GetObjectItem(root, "di"),   cfg.di,   APP_CFG_DI_COUNT);
    parse_invert_array(cJSON_GetObjectItem(root, "dout"), cfg.dout, APP_CFG_DO_COUNT);

    cJSON *mb = cJSON_GetObjectItem(root, "modbus");
    if (cJSON_IsObject(mb)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(mb, "enable"))   && cJSON_IsBool(v))
            cfg.modbus.enable = cJSON_IsTrue(v) ? 1 : 0;
        if ((v = cJSON_GetObjectItem(mb, "address"))  && cJSON_IsNumber(v)) {
            uint32_t a = (uint32_t)v->valuedouble;
            if (a >= 1 && a <= 247) cfg.modbus.address = (uint8_t)a;
        }
        if ((v = cJSON_GetObjectItem(mb, "baudrate")) && cJSON_IsNumber(v))
            cfg.modbus.baudrate = (uint32_t)v->valuedouble;
    }

    cJSON_Delete(root);

    /* Validate names before saving */
    const char *err = NULL;
    if (!validate_names(cfg.di,   APP_CFG_DI_COUNT, &err) ||
        !validate_names(cfg.dout, APP_CFG_DO_COUNT, &err)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"message\":\"%s\"}", err);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    app_config_update(&cfg);
    di_publish_all();
    /* Re-subscribe with potentially new names, then publish all DO states. */
    dout_on_mqtt_connected();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ----------------------------------------------------------------- static files */

static esp_err_t file_get(httpd_req_t *req)
{
    const char *uri = req->uri;

    /* Reject path traversal and URIs too long to fit in the local buffer */
    if (strstr(uri, "..") || strlen(uri) > 120) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }

    /* WWW_BASE(4) + uri(max 120) + NUL = 125 bytes — fits in path[128].
       Use %.120s so GCC's -Wformat-truncation can verify the bound statically. */
    char path[128];
    const char *file = (strcmp(uri, "/") == 0) ? "/index.html" : uri;
    snprintf(path, sizeof(path), "%s%.120s", WWW_BASE, file);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, mime_type(path));

    char *chunk = malloc(CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    size_t n;
    while ((n = fread(chunk, 1, CHUNK_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
            ESP_LOGW(TAG, "Send chunk failed for %s", path);
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);

    free(chunk);
    fclose(f);
    return ESP_OK;
}

/* ------------------------------------------------------------------ /api/reboot */

static void do_restart(void *arg) { esp_restart(); }

static esp_err_t api_factory_reset(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"resetting\"}");

    /* Erase the entire NVS default partition, then reboot.
       This clears WiFi credentials, all config, and IO names. */
    nvs_flash_erase();

    esp_timer_handle_t t;
    esp_timer_create(&(esp_timer_create_args_t){
        .callback = do_restart, .name = "factory_reset"
    }, &t);
    esp_timer_start_once(t, 200 * 1000);
    return ESP_OK;
}

static esp_err_t api_reboot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");

    /* Delay restart by 200 ms so the HTTP response has time to flush. */
    esp_timer_handle_t t;
    esp_timer_create(&(esp_timer_create_args_t){
        .callback = do_restart, .name = "reboot"
    }, &t);
    esp_timer_start_once(t, 200 * 1000);
    return ESP_OK;
}

/* -------------------------------------------------------------- start / stop */

static const httpd_uri_t s_handlers[] = {
    { .uri = "/api/config", .method = HTTP_GET,  .handler = api_config_get  },
    { .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post },
    { .uri = "/api/reboot",        .method = HTTP_POST, .handler = api_reboot        },
    { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = api_factory_reset },
    { .uri = "/*",          .method = HTTP_GET,  .handler = file_get        },
};

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(ret));
        return ret;
    }

    for (size_t i = 0; i < sizeof(s_handlers) / sizeof(s_handlers[0]); i++) {
        httpd_register_uri_handler(s_server, &s_handlers[i]);
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", cfg.server_port);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
}
