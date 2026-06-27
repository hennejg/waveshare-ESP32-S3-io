#include "web_server.h"
#include "app_config.h"
#include "auth.h"
#include "di.h"
#include "dout.h"
#include "led.h"
#include "matter.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <lwip/sockets.h>

#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "cJSON.h"
#include "mbedtls/base64.h"

#define TAG        "web_server"
#define WWW_BASE   "/www"
#define CHUNK_SIZE  4096
#define BODY_MAX    2048

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

/* ------------------------------------------------------------------ auth check */

static bool check_auth(httpd_req_t *req)
{
    if (!auth_is_password_set()) return true;

    char hdr[160] = "";
    httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr));
    if (strncmp(hdr, "Basic ", 6) != 0) return false;

    unsigned char decoded[128];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              (const unsigned char *)(hdr + 6),
                              strlen(hdr + 6)) != 0) return false;
    decoded[decoded_len] = '\0';

    const char *pw = (const char *)decoded;
    const char *colon = (const char *)memchr(decoded, ':', decoded_len);
    if (colon) pw = colon + 1;

    bool ok = auth_check_password(pw);
    memset(decoded, 0, sizeof(decoded));
    return ok;
}

static esp_err_t send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Device\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ auth endpoints */

static esp_err_t api_auth_status(httpd_req_t *req)
{
    char json[32];
    snprintf(json, sizeof(json), "{\"password_set\":%s}",
             auth_is_password_set() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t api_auth_begin(httpd_req_t *req)
{
    char session[9];
    esp_err_t r = auth_token_begin(session);
    if (r == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"already_pending\"}");
        return ESP_OK;
    }
    if (r != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal error");
        return ESP_OK;
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"session\":\"%.8s\"}", session);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t api_auth_token(httpd_req_t *req)
{
    char query[16] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char param[9] = "";
    httpd_query_key_value(query, "s", param, sizeof(param));

    char token[33] = "";
    auth_tok_state_t state = auth_token_status(param, token);
    httpd_resp_set_type(req, "application/json");
    switch (state) {
    case AUTH_TOK_WAITING:
        httpd_resp_sendstr(req, "{\"status\":\"waiting\"}");
        break;
    case AUTH_TOK_READY: {
        char json[64];
        snprintf(json, sizeof(json), "{\"status\":\"ready\",\"token\":\"%.32s\"}", token);
        httpd_resp_sendstr(req, json);
        break;
    }
    case AUTH_TOK_TIMEOUT:
        httpd_resp_set_status(req, "408 Request Timeout");
        httpd_resp_sendstr(req, "{\"status\":\"timeout\"}");
        break;
    default:
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"status\":\"idle\"}");
        break;
    }
    return ESP_OK;
}

static esp_err_t api_auth_set_password(httpd_req_t *req)
{
    if (req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_OK; }
    int n = httpd_req_recv(req, body, req->content_len);
    if (n <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv"); return ESP_OK; }
    body[n] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }

    cJSON *token_j = cJSON_GetObjectItem(root, "token");
    cJSON *pw_j    = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(token_j) || !cJSON_IsString(pw_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fields");
        return ESP_OK;
    }

    if (!auth_token_consume(token_j->valuestring)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_token\"}");
        return ESP_OK;
    }

    if (strlen(pw_j->valuestring) < 8) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"password_too_short\"}");
        return ESP_OK;
    }

    auth_set_password(pw_j->valuestring);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ----------------------------------------------------------------- /api/config */

static esp_err_t api_config_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);
    const app_config_t *cfg = app_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddNumberToObject(root, "led_mode",    cfg->led_mode);

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
    if (!check_auth(req)) return send_401(req);
    if (req->content_len > BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_OK; }

    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }

    app_config_t cfg;
    memcpy(&cfg, app_config_get(), sizeof(cfg));

    cJSON *dn = cJSON_GetObjectItem(root, "device_name");
    if (cJSON_IsString(dn)) strlcpy(cfg.device_name, dn->valuestring, sizeof(cfg.device_name));

    cJSON *led_mode_v = cJSON_GetObjectItem(root, "led_mode");
    if (cJSON_IsNumber(led_mode_v)) cfg.led_mode = (uint8_t)led_mode_v->valuedouble;

    parse_invert_array(cJSON_GetObjectItem(root, "di"),   cfg.di,   APP_CFG_DI_COUNT);
    parse_invert_array(cJSON_GetObjectItem(root, "dout"), cfg.dout, APP_CFG_DO_COUNT);

    cJSON_Delete(root);

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

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ----------------------------------------------------------------- static files */

static esp_err_t file_get(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (strstr(uri, "..") || strlen(uri) > 120) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }

    const char *file = (strcmp(uri, "/") == 0) ? "/index.html" : uri;

    /* Disable Nagle's algorithm — critical when BLE coexistence delays WiFi ACKs. */
    int fd = httpd_req_to_sockfd(req);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    char accept_enc[64] = "";
    httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_enc, sizeof(accept_enc));
    bool accept_gzip = strstr(accept_enc, "gzip") != NULL;

    char path[132];
    bool serving_gz = false;

    if (accept_gzip) {
        snprintf(path, sizeof(path), "%s%.120s.gz", WWW_BASE, file);
        FILE *probe = fopen(path, "r");
        if (probe) {
            fclose(probe);
            serving_gz = true;
        }
    }

    if (!serving_gz)
        snprintf(path, sizeof(path), "%s%.120s", WWW_BASE, file);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, mime_type(file));

    if (strcmp(file, "/index.html") == 0)
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    else
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");

    if (serving_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    }

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

/* --------------------------------------------------------------- /api/matter/pairing */

static esp_err_t api_matter_pairing(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);

    const char *qr  = matter_get_qr_code();
    const char *man = matter_get_manual_code();
    bool commissioned = matter_is_commissioned();

    if ((!qr || !qr[0]) && !commissioned) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"matter_not_active\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "qr_code",      (qr && qr[0]) ? qr : "");
    cJSON_AddStringToObject(root, "manual_code",  (man && man[0]) ? man : "");
    cJSON_AddBoolToObject  (root, "commissioned", commissioned);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    return ESP_OK;
}

/* --------------------------------------------------------------- /api/matter/decommission */

static esp_err_t api_matter_decommission(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);

    if (!matter_is_commissioned()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"not_commissioned\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"decommissioning\"}");

    matter_decommission();
    return ESP_OK;
}

/* ------------------------------------------------------------------ /api/io/... */

static esp_err_t api_io_state(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);

    cJSON *root = cJSON_CreateObject();
    cJSON *di   = cJSON_AddArrayToObject(root, "di");
    cJSON *dout = cJSON_AddArrayToObject(root, "dout");
    for (int i = 0; i < 8; i++) {
        cJSON_AddItemToArray(di,   cJSON_CreateBool(di_get((uint8_t)i)));
        cJSON_AddItemToArray(dout, cJSON_CreateBool(dout_get((uint8_t)i)));
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t api_io_output(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);
    if (req->content_len == 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_OK;
    }
    char body[65];
    int n = httpd_req_recv(req, body, req->content_len);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv"); return ESP_OK; }
    body[n] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }

    cJSON *ch_j = cJSON_GetObjectItem(root, "channel");
    if (!cJSON_IsNumber(ch_j) || (int)ch_j->valuedouble < 0 || (int)ch_j->valuedouble > 7) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channel 0-7 required");
        return ESP_OK;
    }
    uint8_t ch = (uint8_t)ch_j->valuedouble;

    bool new_val;
    cJSON *tog = cJSON_GetObjectItem(root, "toggle");
    cJSON *val = cJSON_GetObjectItem(root, "value");
    if (cJSON_IsTrue(tog)) {
        new_val = !dout_get(ch);
    } else if (cJSON_IsBool(val)) {
        new_val = cJSON_IsTrue(val);
    } else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value or toggle required");
        return ESP_OK;
    }
    cJSON_Delete(root);

    dout_set(ch, new_val);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t api_io_led(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);
    if (req->content_len == 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_OK;
    }
    char body[65];
    int n = httpd_req_recv(req, body, req->content_len);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv"); return ESP_OK; }
    body[n] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }

    uint8_t r = 0, g = 0, b = 0;
    cJSON *color_j = cJSON_GetObjectItem(root, "color");
    if (cJSON_IsString(color_j) && color_j->valuestring[0] == '#') {
        unsigned int ri = 0, gi = 0, bi = 0;
        if (sscanf(color_j->valuestring + 1, "%02x%02x%02x", &ri, &gi, &bi) == 3) {
            r = (uint8_t)ri; g = (uint8_t)gi; b = (uint8_t)bi;
        }
    } else {
        cJSON *rj = cJSON_GetObjectItem(root, "r");
        cJSON *gj = cJSON_GetObjectItem(root, "g");
        cJSON *bj = cJSON_GetObjectItem(root, "b");
        if (cJSON_IsNumber(rj)) r = (uint8_t)(int)rj->valuedouble;
        if (cJSON_IsNumber(gj)) g = (uint8_t)(int)gj->valuedouble;
        if (cJSON_IsNumber(bj)) b = (uint8_t)(int)bj->valuedouble;
    }
    cJSON_Delete(root);

    led_set_rgb(r, g, b);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* -------------------------------------------------------------- /api/version */

static esp_err_t api_version(httpd_req_t *req)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char json[160];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"idf\":\"%s\",\"date\":\"%s\",\"time\":\"%s\"}",
             d->version, d->idf_ver, d->date, d->time);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* ------------------------------------------------------------------ /api/reboot */

static void do_restart(void *arg) { esp_restart(); }

static esp_err_t api_factory_reset(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"resetting\"}");

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

    esp_timer_handle_t t;
    esp_timer_create(&(esp_timer_create_args_t){
        .callback = do_restart, .name = "reboot"
    }, &t);
    esp_timer_start_once(t, 200 * 1000);
    return ESP_OK;
}

/* -------------------------------------------------------------- start / stop */

static const httpd_uri_t s_handlers[] = {
    { .uri = "/api/auth/status",          .method = HTTP_GET,  .handler = api_auth_status       },
    { .uri = "/api/auth/begin",           .method = HTTP_POST, .handler = api_auth_begin        },
    { .uri = "/api/auth/token",           .method = HTTP_GET,  .handler = api_auth_token        },
    { .uri = "/api/auth/set-password",    .method = HTTP_POST, .handler = api_auth_set_password },
    { .uri = "/api/config",               .method = HTTP_GET,  .handler = api_config_get        },
    { .uri = "/api/config",               .method = HTTP_POST, .handler = api_config_post       },
    { .uri = "/api/io/state",             .method = HTTP_GET,  .handler = api_io_state          },
    { .uri = "/api/io/output",            .method = HTTP_POST, .handler = api_io_output         },
    { .uri = "/api/io/led",               .method = HTTP_POST, .handler = api_io_led            },
    { .uri = "/api/matter/pairing",       .method = HTTP_GET,  .handler = api_matter_pairing    },
    { .uri = "/api/matter/decommission",  .method = HTTP_POST, .handler = api_matter_decommission },
    { .uri = "/api/reboot",               .method = HTTP_POST, .handler = api_reboot            },
    { .uri = "/api/factory-reset",        .method = HTTP_POST, .handler = api_factory_reset     },
    { .uri = "/api/version",              .method = HTTP_GET,  .handler = api_version           },
    { .uri = "/*",                        .method = HTTP_GET,  .handler = file_get              },
};

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = sizeof(s_handlers) / sizeof(s_handlers[0]);
    /* Keep the httpd task stack in internal DMA-capable RAM — PSRAM stacks fail
     * the esp_ptr_in_dram() check inside the SPI-flash driver before NVS writes. */
    cfg.task_caps        = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    cfg.send_wait_timeout = 10;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(ret));
        return ret;
    }

    for (size_t i = 0; i < sizeof(s_handlers) / sizeof(s_handlers[0]); i++)
        httpd_register_uri_handler(s_server, &s_handlers[i]);

    ESP_LOGI(TAG, "HTTP server listening on port %d", cfg.server_port);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
}
