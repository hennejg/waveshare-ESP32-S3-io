#include "web_server.h"
#include "app_config.h"
#include "auth.h"
#include "web_common.h"
#include "net_budget.h"
#include "mb_server.h"
#include "app_time.h"
#include "aux_io.h"
#include "sntp_sync.h"
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
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "cJSON.h"

#define TAG        "web_server"
#define WWW_BASE   "/www"
#define CHUNK_SIZE  4096
#define BODY_MAX    2048

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------ helpers */

static void parse_invert_array(cJSON *arr, di_config_t *out, int max, bool *too_long)
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
        /* Whole or not at all: cut at 20 bytes, a name could end inside a
           character, and Matter labels must be UTF-8. */
        if (cJSON_IsString(name)) {
            if (strlen(name->valuestring) >= sizeof(out[i].name)) { *too_long = true; continue; }
            strlcpy(out[i].name, name->valuestring, sizeof(out[i].name));
        }
    }
}

/* A text field of the configuration: only when sent; false if it is not
   text, not UTF-8 or does not fit. */
static bool str_field(const cJSON *obj, const char *key, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v || cJSON_IsNull(v)) return true;
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap || !app_config_utf8_ok(v->valuestring))
        return false;
    strlcpy(dst, v->valuestring, cap);
    return true;
}

/* ----------------------------------------------------------------- /api/config */

static esp_err_t api_config_get(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);
    const app_config_t *cfg = app_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddNumberToObject(root, "led_mode",    cfg->led_mode);
    cJSON_AddNumberToObject(root, "di_isolated", APP_CFG_DI_ISOLATED_COUNT);

    cJSON *sntp = cJSON_AddObjectToObject(root, "sntp");
    cJSON_AddBoolToObject  (sntp, "enable", cfg->sntp.enable);
    cJSON_AddStringToObject(sntp, "server", cfg->sntp.server);
    cJSON_AddStringToObject(root, "tz", cfg->tz);
#if APP_CFG_DI_AUX_COUNT
    web_aux_config_json(root, cfg);
#endif

    /* Modbus: RTU slave on RS-485 and the Modbus TCP server only. It runs on
       a commissioned device ("running"); changes apply after a reboot. */
    cJSON *mb = cJSON_AddObjectToObject(root, "modbus");
    cJSON_AddBoolToObject  (mb, "enable",     cfg->modbus.enable);
    cJSON_AddNumberToObject(mb, "address",    cfg->modbus.address);
    cJSON_AddNumberToObject(mb, "baudrate",   cfg->modbus.baudrate);
    cJSON_AddBoolToObject  (mb, "tcp_server", cfg->modbus.tcp_server);
    cJSON_AddNumberToObject(mb, "tcp_uid",    cfg->modbus.tcp_uid);
    cJSON_AddBoolToObject  (mb, "running",    mb_server_running());

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
    if (!web_check_auth(req)) return web_send_denied(req);
    if (req->content_len > BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_OK; }

    int received = web_recv_body(req, body, req->content_len);
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

    bool too_long = false;
    cJSON *dn = cJSON_GetObjectItem(root, "device_name");
    if (cJSON_IsString(dn)) {
        if (strlen(dn->valuestring) >= sizeof(cfg.device_name)) too_long = true;
        else strlcpy(cfg.device_name, dn->valuestring, sizeof(cfg.device_name));
    }

    uint32_t n;
    if (web_json_uint(cJSON_GetObjectItem(root, "led_mode"), LED_MODE_IO, LED_MODE_STATUS, &n))
        cfg.led_mode = (uint8_t)n;

    parse_invert_array(cJSON_GetObjectItem(root, "di"),   cfg.di,   APP_CFG_DI_COUNT, &too_long);
    parse_invert_array(cJSON_GetObjectItem(root, "dout"), cfg.dout, APP_CFG_DO_COUNT, &too_long);

    const char *bad = NULL;
    cJSON *sntp_j = cJSON_GetObjectItem(root, "sntp");
    if (cJSON_IsObject(sntp_j)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(sntp_j, "enable")) && cJSON_IsBool(v))
            cfg.sntp.enable = cJSON_IsTrue(v) ? 1 : 0;
        if (!str_field(sntp_j, "server", cfg.sntp.server, sizeof(cfg.sntp.server)))
            bad = "invalid value for sntp.server (wrong type or too long)";
    }
    if (!bad && !str_field(root, "tz", cfg.tz, sizeof(cfg.tz)))
        bad = "invalid value for tz (wrong type or too long)";
#if APP_CFG_DI_AUX_COUNT
    if (!bad) bad = web_aux_config_parse(root, &cfg);
#endif
    bool mb_changed = false;
    cJSON *mb = cJSON_GetObjectItem(root, "modbus");
    if (!bad && cJSON_IsObject(mb)) {
        cJSON *v;
        uint32_t u;
        if ((v = cJSON_GetObjectItem(mb, "enable")) && cJSON_IsBool(v))
            cfg.modbus.enable = cJSON_IsTrue(v) ? 1 : 0;
        if ((v = cJSON_GetObjectItem(mb, "tcp_server")) && cJSON_IsBool(v))
            cfg.modbus.tcp_server = cJSON_IsTrue(v) ? 1 : 0;
        if ((v = cJSON_GetObjectItem(mb, "address"))) {
            if (web_json_uint(v, 1, 247, &u)) cfg.modbus.address = (uint8_t)u;
            else bad = "modbus.address must be 1-247";
        }
        if (!bad && (v = cJSON_GetObjectItem(mb, "baudrate"))) {
            if (web_json_uint(v, 1200, 921600, &u)) cfg.modbus.baudrate = u;
            else bad = "modbus.baudrate must be 1200-921600";
        }
        if (!bad && (v = cJSON_GetObjectItem(mb, "tcp_uid"))) {
            if (web_json_uint(v, 1, 247, &u)) cfg.modbus.tcp_uid = (uint8_t)u;
            else bad = "modbus.tcp_uid must be 1-247";
        }
        /* This firmware is only ever an RTU slave; a master stored by the
           other firmware is cleared on the first save here. */
        if (!bad && (v = cJSON_GetObjectItem(mb, "rs485_role")) &&
            !(cJSON_IsString(v) && !strcmp(v->valuestring, "slave")))
            bad = "only rs485_role \"slave\" in this firmware (no RS-485 master, no gateway)";
        cfg.modbus.rs485_role = MB_ROLE_SLAVE;
        cfg.modbus.gateway    = 0;
        mb_changed = memcmp(&cfg.modbus, &app_config_get()->modbus, sizeof(cfg.modbus)) != 0;
    }

    cJSON_Delete(root);

    if (bad) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, bad);
        return ESP_OK;
    }

    if (too_long) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a name is too long (at most 20 bytes as UTF-8)");
        return ESP_OK;
    }

    /* The rules the loader applies at boot (io_names_normalize): checked
       here with the same function, a name accepted now would otherwise be
       dropped silently by the next boot -- "2" for DO1 clashes with DO2's
       number, "set" and "read" are reserved, '+' and '#' are MQTT wildcards. */
    const char *err = NULL;
    if (!app_config_io_names_ok(&cfg, &err)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_OK;
    }

    if (app_config_update(&cfg) != ESP_OK) {
        return web_send_json(req, "500 Internal Server Error", "{\"error\":\"configuration could not be stored\"}");
    }
    matter_set_node_label(cfg.device_name);

    /* The pads switch now; their Matter endpoints were made at boot for the
       mode each had then, so a changed mode reaches Matter after a reboot. */
    bool reboot = mb_changed;   /* Modbus is set up once, at start */
#if APP_CFG_DI_AUX_COUNT
    for (int a = 0; a < APP_CFG_DI_AUX_COUNT; a++)
        if (cfg.aux_mode[a] != (uint8_t)aux_io_mode((uint8_t)a)) reboot = true;
#endif
    esp_err_t aux_ret = aux_io_apply();
    app_time_apply_tz();  /* timezone first, so a sync that follows computes in it */
    sntp_sync_apply();
    for (int a = 0; a < APP_CFG_DI_AUX_COUNT; a++) matter_aux_update((uint8_t)a);
    if (aux_ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "Saved, but a header pad could not be switched and is in its standard function (see the device log)");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, reboot ? "{\"status\":\"ok\",\"reboot_required\":true}"
                                   : "{\"status\":\"ok\"}");
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

    httpd_resp_set_type(req, web_mime_type(file));

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
    if (!web_check_auth(req)) return web_send_denied(req);

    const char *qr  = matter_get_qr_code();
    const char *man = matter_get_manual_code();
    bool commissioned = matter_is_commissioned();

    if ((!qr || !qr[0]) && !commissioned) {
        return web_send_json(req, "404 Not Found", "{\"error\":\"matter_not_active\"}");
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
    if (!web_check_auth(req)) return web_send_denied(req);

    if (!matter_is_commissioned()) {
        return web_send_json(req, "409 Conflict", "{\"error\":\"not_commissioned\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"decommissioning\"}");

    matter_decommission();
    return ESP_OK;
}

/* ------------------------------------------------------------------ /api/io/... */

static esp_err_t api_io_state(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);

    cJSON *root = cJSON_CreateObject();
    cJSON *di   = cJSON_AddArrayToObject(root, "di");
    cJSON *dout = cJSON_AddArrayToObject(root, "dout");
    for (int i = 0; i < APP_CFG_DI_COUNT; i++)
        cJSON_AddItemToArray(di,   cJSON_CreateBool(di_get((uint8_t)i)));
    for (int i = 0; i < APP_CFG_DO_COUNT; i++)
        cJSON_AddItemToArray(dout, cJSON_CreateBool(dout_get((uint8_t)i)));
#if APP_CFG_DI_AUX_COUNT
    web_aux_state_json(root);
#endif
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t api_io_output(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);
    if (req->content_len == 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_OK;
    }
    char body[65];
    int n = web_recv_body(req, body, sizeof(body) - 1);
    if (n < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv"); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }

    /* 0-7 are DO1-DO8; 8-10 the header pads, while they are outputs. */
    const int last = APP_CFG_DO_COUNT - 1 + APP_CFG_DI_AUX_COUNT;
    cJSON *ch_j = cJSON_GetObjectItem(root, "channel");
    if (!cJSON_IsNumber(ch_j) || ch_j->valuedouble < 0 || ch_j->valuedouble > last ||
        ch_j->valuedouble != (int)ch_j->valuedouble) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            last > 7 ? "channel 0-10 required" : "channel 0-7 required");
        return ESP_OK;
    }
    uint8_t ch = (uint8_t)ch_j->valuedouble;

#if APP_CFG_DI_AUX_COUNT
    if (ch >= APP_CFG_DO_COUNT) {
        uint8_t a = (uint8_t)(ch - APP_CFG_DO_COUNT);
        esp_err_t r = web_aux_output(req, a, root);
        cJSON_Delete(root);
        matter_aux_update(a);
        return r;
    }
#endif

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
    if (!web_check_auth(req)) return web_send_denied(req);
    if (req->content_len == 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_OK;
    }
    char body[65];
    int n = web_recv_body(req, body, sizeof(body) - 1);
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
        if (web_json_uint(rj, 0, 255, &u)) r = (uint8_t)u;
        if (web_json_uint(gj, 0, 255, &u)) g = (uint8_t)u;
        if (web_json_uint(bj, 0, 255, &u)) b = (uint8_t)u;
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
    if (!web_check_auth(req)) return web_send_denied(req);
    const esp_app_desc_t *d = esp_app_get_description();
    /* Internal RAM is what Matter runs short of (README): free now, the
       lowest it has been since boot, and the largest block left. */
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    char json[320];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"idf\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
             "\"heap_internal_free\":%u,\"heap_internal_min\":%u,\"heap_internal_largest\":%u,"
             "\"heap_dma_free\":%u}",
             d->version, d->idf_ver, d->date, d->time,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* ------------------------------------------------------------------ /api/reboot */

static void do_restart(void *arg) { esp_restart(); }

static esp_err_t api_factory_reset(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);
    /* Erased first, answered after: "resetting" over a failed erase rebooted
       into the old configuration and password. */
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: erase failed: %s", esp_err_to_name(err));
        return web_send_json(req, "500 Internal Server Error", "{\"error\":\"erase failed, nothing reset\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"resetting\"}");

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
    if (!web_check_auth(req)) return web_send_denied(req);
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
    { .uri = "/api/auth/status",          .method = HTTP_GET,  .handler = web_api_auth_status       },
    { .uri = "/api/auth/begin",           .method = HTTP_POST, .handler = web_api_auth_begin        },
    { .uri = "/api/auth/token",           .method = HTTP_GET,  .handler = web_api_auth_token        },
    { .uri = "/api/auth/set-password",    .method = HTTP_POST, .handler = web_api_auth_set_password },
    { .uri = "/api/config",               .method = HTTP_GET,  .handler = api_config_get        },
    { .uri = "/api/config",               .method = HTTP_POST, .handler = api_config_post       },
    { .uri = "/api/io/state",             .method = HTTP_GET,  .handler = api_io_state          },
    { .uri = "/api/io/output",            .method = HTTP_POST, .handler = api_io_output         },
    { .uri = "/api/io/led",               .method = HTTP_POST, .handler = api_io_led            },
    { .uri = "/api/io/buzzer",            .method = HTTP_POST, .handler = web_api_io_buzzer     },
    { .uri = "/api/time",                 .method = HTTP_GET,  .handler = web_api_time_get      },
    { .uri = "/api/time",                 .method = HTTP_POST, .handler = web_api_time_post     },
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
    cfg.max_open_sockets = NET_SOCK_HTTPD_CLIENTS;   /* the share net_budget.h allots */
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
