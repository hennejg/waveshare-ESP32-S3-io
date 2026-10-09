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

/* Longest password whose HTTP Basic header still fits check_auth()'s 160-byte
   buffer: "Basic " + base64(":" + password) must stay within 159 characters,
   which 113 does at 158 and 114 exceeds at 162. Same figure as in apps/full. */
#define APP_CFG_PASSWORD_MAX 113

/* Total time a request body may take to arrive. */
#define BODY_RECV_TIMEOUT_MS 5000

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------ helpers */

/* Reads the whole body, as many socket reads as that takes. httpd_req_recv()
   returns what one read delivered, which is less than content_len whenever
   the body arrives in more than one segment -- and parsing that as JSON
   failed valid configuration and password requests at random. Same helper
   as in apps/full, with the same absolute deadline: a client that dribbles
   one byte at a time must not hold the server's single handler task open. */
static int recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t want = req->content_len;
    if (want > cap) return -1;

    const int64_t deadline = esp_timer_get_time() + (int64_t)BODY_RECV_TIMEOUT_MS * 1000;
    size_t got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, buf + got, want - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() >= deadline) return -1;
            continue;
        }
        if (r <= 0) return -1;
        got += (size_t)r;
        if (esp_timer_get_time() >= deadline && got < want) return -1;
    }
    buf[got] = '\0';
    return (int)got;
}

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

/* A JSON number as an integer in [lo, hi], or false. The range is checked on
   the double: casting an out-of-range double to an integer type is undefined
   behaviour, so {"led_mode":1e10} must never reach the cast. Same as the full
   app's. */
static bool json_uint(const cJSON *v, uint32_t lo, uint32_t hi, uint32_t *out)
{
    if (!cJSON_IsNumber(v)) return false;
    double d = v->valuedouble;
    if (!(d >= lo && d < (double)hi + 1.0)) return false;
    *out = (uint32_t)d;
    return true;
}

/* ------------------------------------------------------------------ auth check */

/* A request a browser sends on behalf of a page from somewhere else: a form
   posted cross-site, a fetch() from another origin. Once the browser has the
   password cached (its own Basic dialog, opened by our WWW-Authenticate), it
   would attach it to those too -- switching outputs, saving rules or wiping
   the device from any web page the user happens to open. Browsers say where
   a request comes from in Sec-Fetch-Site (all current ones) or, failing
   that, Origin; curl and scripts send neither and are not affected. */
static bool cross_site(httpd_req_t *req)
{
    char v[24];
    esp_err_t r = httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", v, sizeof(v));
    if (r == ESP_OK)
        return strcmp(v, "same-origin") != 0 && strcmp(v, "none") != 0;
    if (r != ESP_ERR_NOT_FOUND) return true;            /* too long to be any of them */

    char origin[96], host[64] = "";
    r = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (r == ESP_ERR_NOT_FOUND) return false;
    if (r != ESP_OK) return true;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return true;
    const char *o = strstr(origin, "://");
    return !o || strcmp(o + 3, host) != 0;              /* "null" has no scheme */
}


/* A device without a password is one that has not been set up yet, and
   until that happens nothing but the setup flow itself may be used: the
   outputs, the rules (which run as code on the device) and the factory
   reset must not be one unauthenticated request away just because nobody
   has pressed the button yet. The physical confirmation guards the
   password; this guards everything else until there is one. */
static bool check_auth(httpd_req_t *req)
{
    if (!auth_is_password_set()) return false;
    if (cross_site(req)) {
        ESP_LOGW(TAG, "cross-site request to %s refused", req->uri);
        return false;
    }

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

/* One shape for every JSON answer that is not a payload: status line and
   body, nothing else to get out of step. */
static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* A request that may not pass. Without a password the device is not set up,
   and no credentials would help: that is 403, with a body a client can act
   on, and no WWW-Authenticate for a browser to open a dialog over. With a
   password it is the ordinary 401 challenge. */
static esp_err_t send_denied(httpd_req_t *req)
{
    if (!auth_is_password_set())
        return send_json(req, "403 Forbidden", "{\"error\":\"setup_required\"}");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Device\"");
    return send_json(req, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
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
        return send_json(req, "409 Conflict", "{\"error\":\"already_pending\"}");
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
    int n = recv_body(req, body, req->content_len);
    if (n < 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv"); return ESP_OK; }

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

    /* Check the password before spending the token. It is single use and only
       obtainable by pressing the button on the device, so rejecting the input
       afterwards would send the user back to the hardware for a typo.

       The Basic-auth parser reads the header into a 160-byte buffer, so a
       password that cannot fit there could be set and would then lock the user
       out of the very API that set it -- see APP_CFG_PASSWORD_MAX. */
    if (strlen(pw_j->valuestring) > APP_CFG_PASSWORD_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "password too long (max 113 characters)");
        return ESP_OK;
    }
    if (strlen(pw_j->valuestring) < 8) {
        cJSON_Delete(root);
        return send_json(req, "400 Bad Request", "{\"error\":\"password_too_short\"}");
    }

    if (!auth_token_consume(token_j->valuestring)) {
        cJSON_Delete(root);
        return send_json(req, "403 Forbidden", "{\"error\":\"invalid_token\"}");
    }

    esp_err_t set_ret = auth_set_password(pw_j->valuestring);
    cJSON_Delete(root);
    if (set_ret != ESP_OK) {
        /* The token is spent either way; say what happened rather than
           reporting a protection the flash does not have. */
        return send_json(req, "500 Internal Server Error", "{\"error\":\"password could not be stored\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ----------------------------------------------------------------- /api/config */

static esp_err_t api_config_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_denied(req);
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
    if (!check_auth(req)) return send_denied(req);
    if (req->content_len > BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_OK; }

    int received = recv_body(req, body, req->content_len);
    if (received < 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }

    app_config_t cfg;
    memcpy(&cfg, app_config_get(), sizeof(cfg));

    cJSON *dn = cJSON_GetObjectItem(root, "device_name");
    if (cJSON_IsString(dn)) strlcpy(cfg.device_name, dn->valuestring, sizeof(cfg.device_name));

    uint32_t n;
    if (json_uint(cJSON_GetObjectItem(root, "led_mode"), LED_MODE_IO, LED_MODE_STATUS, &n))
        cfg.led_mode = (uint8_t)n;

    parse_invert_array(cJSON_GetObjectItem(root, "di"),   cfg.di,   APP_CFG_DI_COUNT);
    parse_invert_array(cJSON_GetObjectItem(root, "dout"), cfg.dout, APP_CFG_DO_COUNT);

    cJSON_Delete(root);

    /* The rules the loader applies at boot (io_names_normalize): checked
       here with the same function, a name accepted now would otherwise be
       dropped silently by the next boot -- "2" for DO1 clashes with DO2's
       number, "set" and "read" are reserved, '+' and '#' are MQTT wildcards. */
    const char *err = NULL;
    if (!app_config_io_names_ok(&cfg, &err)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"message\":\"%s\"}", err);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    if (app_config_update(&cfg) != ESP_OK) {
        return send_json(req, "500 Internal Server Error", "{\"error\":\"configuration could not be stored\"}");
    }
    matter_set_node_label(cfg.device_name);

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
    if (!check_auth(req)) return send_denied(req);

    const char *qr  = matter_get_qr_code();
    const char *man = matter_get_manual_code();
    bool commissioned = matter_is_commissioned();

    if ((!qr || !qr[0]) && !commissioned) {
        return send_json(req, "404 Not Found", "{\"error\":\"matter_not_active\"}");
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
    if (!check_auth(req)) return send_denied(req);

    if (!matter_is_commissioned()) {
        return send_json(req, "409 Conflict", "{\"error\":\"not_commissioned\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"decommissioning\"}");

    matter_decommission();
    return ESP_OK;
}

/* ------------------------------------------------------------------ /api/io/... */

static esp_err_t api_io_state(httpd_req_t *req)
{
    if (!check_auth(req)) return send_denied(req);

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
    if (!check_auth(req)) return send_denied(req);
    if (req->content_len == 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_OK;
    }
    char body[65];
    int n = recv_body(req, body, sizeof(body) - 1);
    if (n < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv"); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }

    cJSON *ch_j = cJSON_GetObjectItem(root, "channel");
    if (!cJSON_IsNumber(ch_j) || ch_j->valuedouble < 0 || ch_j->valuedouble > 7 ||
        ch_j->valuedouble != (int)ch_j->valuedouble) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channel 0-7 required");
        return ESP_OK;
    }
    uint8_t ch = (uint8_t)ch_j->valuedouble;

    /* A toggle is one dout_toggle() call, under the output lock: reading
       and setting the opposite lost one of two toggles arriving together. */
    esp_err_t set_ret;
    cJSON *tog = cJSON_GetObjectItem(root, "toggle");
    cJSON *val = cJSON_GetObjectItem(root, "value");
    if (cJSON_IsTrue(tog)) {
        cJSON_Delete(root);
        set_ret = dout_toggle(ch);
    } else if (cJSON_IsBool(val)) {
        bool new_val = cJSON_IsTrue(val);
        cJSON_Delete(root);
        set_ret = dout_set(ch, new_val);
    } else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value or toggle required");
        return ESP_OK;
    }

    /* The hardware moved; tell the Matter side, or a controller keeps
       showing the state before this request. dout_get(): what the port
       really holds, not what was asked. */
    if (set_ret != ESP_OK) {
        ESP_LOGW(TAG, "output %d: set failed: %s", ch + 1, esp_err_to_name(set_ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "output write failed");
        return ESP_OK;
    }
    matter_do_update(ch, dout_get(ch));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t api_io_led(httpd_req_t *req)
{
    if (!check_auth(req)) return send_denied(req);
    if (req->content_len == 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_OK;
    }
    char body[65];
    int n = recv_body(req, body, sizeof(body) - 1);
    if (n < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv"); return ESP_OK; }

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
        uint32_t u;
        if (json_uint(rj, 0, 255, &u)) r = (uint8_t)u;
        if (json_uint(gj, 0, 255, &u)) g = (uint8_t)u;
        if (json_uint(bj, 0, 255, &u)) b = (uint8_t)u;
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
    /* Firmware and IDF version, build date and the last reset reason tell an
       attacker which known bugs apply and when the device crashed: behind the
       password like everything else outside the setup flow. */
    if (!check_auth(req)) return send_denied(req);
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
    if (!check_auth(req)) return send_denied(req);
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
    /* Was open: anyone on the network -- or any web page the user had open,
       by a cross-site form post -- could reboot the device, in a loop. */
    if (!check_auth(req)) return send_denied(req);
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
    /* HTTPD_DEFAULT_CONFIG gives 4096; api_config_post holds a whole
       app_config_t (1.8 kB) plus cJSON's working set and goes down into NVS
       from there. The full app raised it for the same reason. */
    cfg.stack_size        = 6144;

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
