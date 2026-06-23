#include "web_server.h"
#include "app_config.h"
#include "auth.h"
#include "buzzer.h"
#include "di.h"
#include "dout.h"
#include "led.h"
#include "matter.h"
#include "scripting.h"

extern const char DEMO_SCRIPT[];

#define RULES_NVS_NS  "scripting"
#define RULES_NVS_KEY "script"
#define RULES_MAX_LEN 3900

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <lwip/sockets.h>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_netif.h>
#include "cJSON.h"
#include "mbedtls/base64.h"

#define TAG        "web_server"
#define WWW_BASE   "/www"
#define CHUNK_SIZE  4096
#define BODY_MAX    2048  /* names add ~768 bytes to the di/dout arrays */

#define NVS_ETH_NS  "app_config"
#define NVS_ETH_KEY "eth_only"

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

/* ------------------------------------------------------------------ auth check */

/* Returns true if the request carries a valid password (or no password is set). */
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

    // HTTP Basic format is "user:password"; accept any username, check password only.
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

/* GET /api/auth/status — no auth required */
static esp_err_t api_auth_status(httpd_req_t *req)
{
    char json[32];
    snprintf(json, sizeof(json), "{\"password_set\":%s}",
             auth_is_password_set() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* POST /api/auth/begin — start token flow; no auth required */
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

/* GET /api/auth/token?s=<session> — poll for token; no auth required */
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

/* POST /api/auth/set-password — set/change password using token; no prior auth needed */
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
    cJSON_AddStringToObject(root, "device_name",       cfg->device_name);
    cJSON_AddStringToObject(root, "mqtt_url",          cfg->mqtt_url);
    cJSON_AddStringToObject(root, "mqtt_user",         cfg->mqtt_user);
    /* Never expose the password — return a flag instead. */
    cJSON_AddBoolToObject  (root, "mqtt_password_set", cfg->mqtt_password[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);
    cJSON_AddNumberToObject(root, "led_mode",          cfg->led_mode);

    cJSON *can = cJSON_AddObjectToObject(root, "can");
    cJSON_AddNumberToObject(can, "mode",           cfg->can.mode);
    cJSON_AddNumberToObject(can, "n2k_addr",       cfg->can.n2k_addr);
    cJSON_AddNumberToObject(can, "base_id",        cfg->can.base_id);
    cJSON_AddNumberToObject(can, "bitrate",        cfg->can.bitrate);
    cJSON_AddNumberToObject(can, "tx_interval_ms", cfg->can.tx_interval_ms);

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

    uint8_t eth_only_val = 0;
    nvs_handle_t nvs_h;
    if (nvs_open(NVS_ETH_NS, NVS_READONLY, &nvs_h) == ESP_OK) {
        nvs_get_u8(nvs_h, NVS_ETH_KEY, &eth_only_val);
        nvs_close(nvs_h);
    }
    cJSON_AddBoolToObject(root, "eth_only", eth_only_val != 0);

    esp_netif_t *eth_if = esp_netif_get_handle_from_ifkey("ETH_DEF");
    cJSON_AddBoolToObject(root, "eth_connected",
                          eth_if != NULL && esp_netif_is_netif_up(eth_if));

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

    cJSON *led_mode_v = cJSON_GetObjectItem(root, "led_mode");
    if (cJSON_IsNumber(led_mode_v))
        cfg.led_mode = (uint8_t)led_mode_v->valuedouble;

    cJSON *can_j = cJSON_GetObjectItem(root, "can");
    if (cJSON_IsObject(can_j)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(can_j, "mode")) && cJSON_IsNumber(v)) {
            uint8_t m = (uint8_t)v->valuedouble;
            if (m <= 2) cfg.can.mode = m;
        }
        if ((v = cJSON_GetObjectItem(can_j, "n2k_addr")) && cJSON_IsNumber(v)) {
            uint8_t a = (uint8_t)v->valuedouble;
            if (a >= 1 && a <= 251) cfg.can.n2k_addr = a;
        }
        if ((v = cJSON_GetObjectItem(can_j, "base_id")) && cJSON_IsNumber(v)) {
            uint32_t id = (uint32_t)v->valuedouble;
            if (id <= 0x7FF) cfg.can.base_id = (uint16_t)id;
        }
        if ((v = cJSON_GetObjectItem(can_j, "bitrate")) && cJSON_IsNumber(v))
            cfg.can.bitrate = (uint32_t)v->valuedouble;
        if ((v = cJSON_GetObjectItem(can_j, "tx_interval_ms")) && cJSON_IsNumber(v))
            cfg.can.tx_interval_ms = (uint16_t)v->valuedouble;
    }

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

    const char *file = (strcmp(uri, "/") == 0) ? "/index.html" : uri;

    /* Disable Nagle's algorithm for this connection.  Without TCP_NODELAY, lwIP
     * buffers the first file chunk waiting for an ACK of the HTTP headers —
     * which can take seconds when BLE coexistence delays WiFi ACKs. */
    int fd = httpd_req_to_sockfd(req);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Check if the client accepts gzip.  All modern browsers do; this lets us
     * serve pre-compressed files (index.html.gz, qrcode.min.js.gz) which are
     * small enough to fit in the TCP send buffer without waiting for ACKs —
     * critical when BLE coexistence delays WiFi ACKs. */
    char accept_enc[64] = "";
    httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_enc, sizeof(accept_enc));
    bool accept_gzip = strstr(accept_enc, "gzip") != NULL;

    /* WWW_BASE(4) + file(max 120) + ".gz"(3) + NUL = 128 bytes — fits in path[132]. */
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

    /* MIME type is determined by the logical file extension, not the .gz suffix. */
    httpd_resp_set_type(req, mime_type(file));

    /* index.html must not be cached — it changes with every firmware flash.
     * Other static assets (JS, SVG) are versioned by flash and can be cached. */
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

/* --------------------------------------------------------- /api/matter/decommission */

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

/* --------------------------------------------------------------- /api/matter/pairing */

static esp_err_t api_matter_pairing(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);

    const char *qr  = matter_get_qr_code();
    const char *man = matter_get_manual_code();
    bool commissioned = matter_is_commissioned();

    /* Return 404 only when Matter is truly inactive (no QR code AND not commissioned). */
    if ((!qr || !qr[0]) && !commissioned) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"matter_not_enabled\"}");
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

/* --------------------------------------------------------------- /api/eth-only */

static void do_restart(void *arg);  /* defined below in /api/reboot section */

static esp_err_t api_eth_only(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);
    if (req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char body[65] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'enabled'");
        return ESP_OK;
    }
    bool want_eth_only = cJSON_IsTrue(enabled);
    cJSON_Delete(root);

    nvs_handle_t nvs_h;
    if (nvs_open(NVS_ETH_NS, NVS_READWRITE, &nvs_h) == ESP_OK) {
        if (want_eth_only) nvs_set_u8(nvs_h, NVS_ETH_KEY, 1);
        else               nvs_erase_key(nvs_h, NVS_ETH_KEY);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");

    esp_timer_handle_t t;
    esp_timer_create(&(esp_timer_create_args_t){
        .callback = do_restart, .name = "eth_only"
    }, &t);
    esp_timer_start_once(t, 200 * 1000);
    return ESP_OK;
}

/* ------------------------------------------------------------------ /api/reboot */

static void do_restart(void *arg) { esp_restart(); }

static esp_err_t api_factory_reset(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);
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

/* ----------------------------------------------------------------- /api/io/... */

/* GET /api/io/state — current logical state of all DI and DO channels */
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

/* POST /api/io/output — {"channel":0-7,"value":bool} or {"channel":0-7,"toggle":true} */
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

/* POST /api/io/led — {"r":0-255,"g":0-255,"b":0-255} or {"color":"#RRGGBB"} */
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

/* POST /api/io/buzzer — {"freq":440,"duration":200} */
static esp_err_t api_io_buzzer(httpd_req_t *req)
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

    uint32_t freq = 440, dur = 200;
    cJSON *fj = cJSON_GetObjectItem(root, "freq");
    cJSON *dj = cJSON_GetObjectItem(root, "duration");
    if (cJSON_IsNumber(fj)) freq = (uint32_t)fj->valuedouble;
    if (cJSON_IsNumber(dj)) dur  = (uint32_t)dj->valuedouble;
    cJSON_Delete(root);

    buzzer_beep_once(freq, dur);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------ rules endpoints */

/* GET /api/rules — returns {"script":"..."} from NVS, or the demo script if unset */
static esp_err_t api_rules_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);

    char *buf = malloc(RULES_MAX_LEN + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_OK; }

    const char *script = DEMO_SCRIPT;
    nvs_handle_t h;
    size_t len = RULES_MAX_LEN + 1;
    if (nvs_open(RULES_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, RULES_NVS_KEY, buf, &len) == ESP_OK)
            script = buf;
        nvs_close(h);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "script", script);
    free(buf);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

/* POST /api/rules — {"script":"..."} saves to NVS and hot-reloads the engine */
static esp_err_t api_rules_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_401(req);
    if (req->content_len == 0 || req->content_len > RULES_MAX_LEN + 100) {
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

    cJSON *script_j = cJSON_GetObjectItemCaseSensitive(root, "script");
    if (!cJSON_IsString(script_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'script' field");
        return ESP_OK;
    }

    const char *script = script_j->valuestring;
    nvs_handle_t h;
    if (nvs_open(RULES_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (script[0] == '\0')
            nvs_erase_key(h, RULES_NVS_KEY);
        else
            nvs_set_str(h, RULES_NVS_KEY, script);
        nvs_commit(h);
        nvs_close(h);
    }

    scripting_reload(script[0] ? script : DEMO_SCRIPT);

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* -------------------------------------------------------------- start / stop */

static const httpd_uri_t s_handlers[] = {
    { .uri = "/api/auth/status",       .method = HTTP_GET,  .handler = api_auth_status       },
    { .uri = "/api/auth/begin",        .method = HTTP_POST, .handler = api_auth_begin        },
    { .uri = "/api/auth/token",        .method = HTTP_GET,  .handler = api_auth_token        },
    { .uri = "/api/auth/set-password", .method = HTTP_POST, .handler = api_auth_set_password },
    { .uri = "/api/config",            .method = HTTP_GET,  .handler = api_config_get        },
    { .uri = "/api/config",            .method = HTTP_POST, .handler = api_config_post       },
    { .uri = "/api/io/state",          .method = HTTP_GET,  .handler = api_io_state          },
    { .uri = "/api/io/output",         .method = HTTP_POST, .handler = api_io_output         },
    { .uri = "/api/io/led",            .method = HTTP_POST, .handler = api_io_led            },
    { .uri = "/api/io/buzzer",         .method = HTTP_POST, .handler = api_io_buzzer         },
    { .uri = "/api/matter/pairing",       .method = HTTP_GET,  .handler = api_matter_pairing      },
    { .uri = "/api/matter/decommission", .method = HTTP_POST, .handler = api_matter_decommission },
    { .uri = "/api/eth-only",          .method = HTTP_POST, .handler = api_eth_only          },
    { .uri = "/api/reboot",            .method = HTTP_POST, .handler = api_reboot            },
    { .uri = "/api/factory-reset",     .method = HTTP_POST, .handler = api_factory_reset     },
    { .uri = "/api/rules",             .method = HTTP_GET,  .handler = api_rules_get         },
    { .uri = "/api/rules",             .method = HTTP_POST, .handler = api_rules_post        },
    { .uri = "/*",                     .method = HTTP_GET,  .handler = file_get              },
};

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 18;
    /* Allocate the httpd task stack from the reserved internal DMA pool
     * (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), not from PSRAM.
     * PSRAM stacks fail the esp_ptr_in_dram() check inside the SPI-flash driver,
     * which asserts before every NVS write.  After Matter init the general internal
     * heap is nearly exhausted (~1.5 KB), but CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL
     * keeps 32 KB of DMA-capable internal RAM aside for exactly this kind of request,
     * and the DMA pool still has ~29 KB free at that point. */
    cfg.task_caps        = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    cfg.send_wait_timeout = 10;   /* seconds; default 5 is too tight with Matter on WiFi */

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
