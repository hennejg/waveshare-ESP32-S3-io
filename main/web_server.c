#include "web_server.h"
#include "app_config.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include <esp_http_server.h>
#include <esp_log.h>

#define TAG        "web_server"
#define WWW_BASE   "/www"
#define CHUNK_SIZE  4096
#define BODY_MAX    1024

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

/* Extract a JSON string value: {"key":"value"} → value.
   Does not handle escape sequences — sufficient for simple config fields. */
static bool json_get_str(const char *json, const char *key,
                         char *out, size_t out_len)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t n = (size_t)(end - p);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* Extract a JSON boolean value: {"key":true} or {"key":false}. */
static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true",  4) == 0) { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

/* Parse the "di" JSON array into out[APP_CFG_DI_COUNT].
   Expects: "di":[{"invert":bool,...},...]  — unknown keys are ignored. */
static void json_parse_di(const char *json, di_config_t out[APP_CFG_DI_COUNT])
{
    const char *p = strstr(json, "\"di\":[");
    if (!p) return;
    p += 6;
    for (int i = 0; i < APP_CFG_DI_COUNT; i++) {
        const char *ob = strchr(p, '{');
        const char *cb = ob ? strchr(ob, '}') : NULL;
        if (!ob || !cb) return;
        char buf[128];
        size_t n = (size_t)(cb - ob);
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, ob, n);
        buf[n] = '\0';
        json_get_bool(buf, "invert", &out[i].invert);
        p = cb + 1;
    }
}

/* ----------------------------------------------------------------- /api/config */

static esp_err_t api_config_get(httpd_req_t *req)
{
    const app_config_t *cfg = app_config_get();

    /* Build DI array: [{"invert":bool},...]  max 145 chars */
    char di_arr[160];
    int  da = snprintf(di_arr, sizeof(di_arr), "[");
    for (int i = 0; i < APP_CFG_DI_COUNT; i++) {
        da += snprintf(di_arr + da, sizeof(di_arr) - (size_t)da,
            "{\"invert\":%s}%s",
            cfg->di[i].invert ? "true" : "false",
            i < APP_CFG_DI_COUNT - 1 ? "," : "");
    }
    snprintf(di_arr + da, sizeof(di_arr) - (size_t)da, "]");

    /* mqtt_password is intentionally omitted — never expose credentials via GET. */
    char json[768];
    int len = snprintf(json, sizeof(json),
        "{\"device_name\":\"%.31s\","
        "\"mqtt_url\":\"%.127s\","
        "\"mqtt_user\":\"%.63s\","
        "\"mqtt_password_set\":%s,"
        "\"mqtt_topic_prefix\":\"%.63s\","
        "\"di\":%.159s}",
        cfg->device_name,
        cfg->mqtt_url,
        cfg->mqtt_user,
        cfg->mqtt_password[0] ? "true" : "false",
        cfg->mqtt_topic_prefix,
        di_arr);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
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

    app_config_t cfg;
    memcpy(&cfg, app_config_get(), sizeof(cfg));

    json_get_str(body, "device_name",      cfg.device_name,       sizeof(cfg.device_name));
    json_get_str(body, "mqtt_url",         cfg.mqtt_url,          sizeof(cfg.mqtt_url));
    json_get_str(body, "mqtt_user",        cfg.mqtt_user,         sizeof(cfg.mqtt_user));
    json_get_str(body, "mqtt_topic_prefix",cfg.mqtt_topic_prefix, sizeof(cfg.mqtt_topic_prefix));

    /* Only update the password if a non-empty value was posted. */
    char new_pass[APP_CFG_MQTT_PASS_LEN] = "";
    if (json_get_str(body, "mqtt_password", new_pass, sizeof(new_pass)) && new_pass[0]) {
        strlcpy(cfg.mqtt_password, new_pass, sizeof(cfg.mqtt_password));
    }

    json_parse_di(body, cfg.di);

    free(body);
    app_config_update(&cfg);

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

/* -------------------------------------------------------------- start / stop */

static const httpd_uri_t s_handlers[] = {
    { .uri = "/api/config", .method = HTTP_GET,  .handler = api_config_get  },
    { .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post },
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
