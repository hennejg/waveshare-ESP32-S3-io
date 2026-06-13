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

/* ----------------------------------------------------------------- /api/config */

static esp_err_t api_config_get(httpd_req_t *req)
{
    const app_config_t *cfg = app_config_get();
    char json[256];
    /* Mirror new fields here when extending app_config_t. */
    int len = snprintf(json, sizeof(json),
        "{\"device_name\":\"%s\"}",
        cfg->device_name);

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

    /* Mirror new fields here when extending app_config_t. */
    json_get_str(body, "device_name", cfg.device_name, sizeof(cfg.device_name));

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
