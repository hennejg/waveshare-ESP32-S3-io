#include "web_server.h"
#include "net_budget.h"
#include "app_config.h"
#include "app_time.h"
#include "auth.h"
#include "web_common.h"
#include "di.h"
#include "dout.h"
#include "aux_io.h"
#include "app_mqtt.h"
#include "esp_system.h"
#include "led.h"
#ifdef CONFIG_APP_MATTER_ENABLE
#include "matter.h"
#endif
#include "mb_tcp_server.h"
#include "mb_tcp_master.h"
#include "mb_status_json.h"
#include "scripting.h"
#include "sntp_sync.h"

extern const char DEMO_SCRIPT[];

#define RULES_NVS_NS   "scripting"
/* A blob, which NVS spreads over pages. A string has to fit in one page next
   to whatever else is on it: after a garbage collection moved a few entries
   onto the free page, a script of 3840 bytes no longer went in
   (ESP_ERR_NVS_NOT_ENOUGH_SPACE) -- below RULES_MAX_LEN, and depending on
   the state of the flash. The string key is what older firmware stored; it
   is still read, and removed once a blob has been stored. */
#define RULES_NVS_BLOB "script_b"
#define RULES_NVS_KEY  "script"
/* In bytes of UTF-8, not characters: what NVS stores and the boot's buffer
   (main.c, 4096) holds. Checked on the script itself -- the request body is
   JSON, longer by its escapes, and a limit on the body alone let a script of
   3942 bytes through that GET then could not read back. */
#define RULES_MAX_LEN 3900

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
#include <esp_netif.h>
#include "cJSON.h"

#define TAG        "web_server"

/* How long POST /api/rules waits for the rule engine to accept or refuse the
   script. Handlers run on the server task, so this caps how long one request
   can hold up the others. */
#define RULES_APPLY_TIMEOUT_MS 1000
#define WWW_BASE   "/www"
#define CHUNK_SIZE  4096
/* The whole configuration travels in one body. The di and dout names add
   about 768 bytes; the eight Modbus master rows add up to another 1850 at
   their maximum name and host lengths, measured at 2868 bytes for a full
   table. At 2048 a table of five rows could be displayed but never saved. */
#define BODY_MAX    4096

#define NVS_ETH_NS  "app_config"
#define NVS_ETH_KEY "eth_only"

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------ helpers */

/* Parse a cJSON array of {invert, name} objects into a di_config_t array. */
static void parse_invert_array(cJSON *arr, di_config_t *out, int max, const char *where, web_cfg_err_t *bad)
{
    if (!cJSON_IsArray(arr)) return;
    int n = cJSON_GetArraySize(arr);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;
        cJSON *inv  = cJSON_GetObjectItem(item, "invert");
        if (cJSON_IsBool(inv)) out[i].invert = cJSON_IsTrue(inv);
        char w[16];
        snprintf(w, sizeof(w), "%s%d.", where, i + 1);
        web_cfg_str(item, w, "name", out[i].name, sizeof(out[i].name), bad);
    }
}

/* ----------------------------------------------------------------- /api/config */

/* The value types and the two readable function codes travel as names rather
   than numbers: a configuration is read by people, and "f32" says what 4 does
   not. */
static esp_err_t api_config_get(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);
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
    cJSON_AddStringToObject(mb, "rs485_role",
                            cfg->modbus.rs485_role == MB_ROLE_MASTER ? "master" : "slave");
    cJSON_AddBoolToObject  (mb, "tcp_server", cfg->modbus.tcp_server);
    cJSON_AddBoolToObject  (mb, "gateway",    cfg->modbus.gateway);
    cJSON_AddNumberToObject(mb, "tcp_uid",     cfg->modbus.tcp_uid);
    web_mbm_config_json(root, cfg);

    cJSON_AddNumberToObject(mb, "rs485_tout_ms", cfg->modbus.rs485_tout_ms);

    cJSON *sntp = cJSON_AddObjectToObject(root, "sntp");
    cJSON_AddBoolToObject  (sntp, "enable", cfg->sntp.enable);
    cJSON_AddStringToObject(sntp, "server", cfg->sntp.server);

    cJSON_AddStringToObject(root, "tz", cfg->tz);

    cJSON *di = cJSON_AddArrayToObject(root, "di");
    for (int i = 0; i < APP_CFG_DI_COUNT; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddBoolToObject  (item, "invert", cfg->di[i].invert);
        cJSON_AddStringToObject(item, "name",   cfg->di[i].name);
        cJSON_AddItemToArray(di, item);
    }
    /* Inputs from this index on are not optocoupler-isolated. */
    cJSON_AddNumberToObject(root, "di_isolated", APP_CFG_DI_ISOLATED_COUNT);
#if APP_CFG_DI_AUX_COUNT
    web_aux_config_json(root, cfg);
#endif

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
    if (!web_check_auth(req)) return web_send_denied(req);
    if (req->content_len > BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    int received = web_recv_body(req, body, req->content_len);
    if (received < 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    app_config_t cfg;
    memcpy(&cfg, app_config_get(), sizeof(cfg));
    web_cfg_err_t bad = { "" };

    web_cfg_str(root, "", "device_name", cfg.device_name, sizeof(cfg.device_name), &bad);
    web_cfg_str(root, "", "mqtt_url",    cfg.mqtt_url,    sizeof(cfg.mqtt_url),    &bad);
    web_cfg_str(root, "", "mqtt_user",   cfg.mqtt_user,   sizeof(cfg.mqtt_user),   &bad);
    /* The prefix is used verbatim as a publish topic and as the last-will
       topic. '#' and '+' are illegal there (MQTT 3.1.1 3.3.2.1 / 4.7) and a
       conforming broker closes the connection on CONNECT, after which MQTT
       never reconnects and nothing in the log points at the cause. '/' at
       either end produces an empty topic level. */
    {
        cJSON *_v = cJSON_GetObjectItem(root, "mqtt_topic_prefix");
        if (cJSON_IsString(_v)) {
            const char *pfx = _v->valuestring;
            size_t len = strlen(pfx);
            if (strpbrk(pfx, "#+") || (len && (pfx[0] == '/' || pfx[len - 1] == '/'))) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "mqtt_topic_prefix must not contain '#' or '+' "
                                    "or start or end with '/'");
                return ESP_OK;
            }
            web_cfg_str(root, "", "mqtt_topic_prefix", cfg.mqtt_topic_prefix, sizeof(cfg.mqtt_topic_prefix), &bad);
        }
    }

    /* Password: only update when a non-empty value is sent */
    cJSON *pass = cJSON_GetObjectItem(root, "mqtt_password");
    if (cJSON_IsString(pass) && pass->valuestring[0])
        web_cfg_str(root, "", "mqtt_password", cfg.mqtt_password, sizeof(cfg.mqtt_password), &bad);

    parse_invert_array(cJSON_GetObjectItem(root, "di"), cfg.di, APP_CFG_DI_COUNT, "di.", &bad);
#if APP_CFG_DI_AUX_COUNT
    const char *aux_err = web_aux_config_parse(root, &cfg);
    if (aux_err) {
        /* Plain text, like every other refusal of this handler: the page
           shows the body as it is. */
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, aux_err);
        return ESP_OK;
    }
#endif
    parse_invert_array(cJSON_GetObjectItem(root, "dout"), cfg.dout, APP_CFG_DO_COUNT, "dout.", &bad);

    uint32_t n;
    if (web_cfg_uint(root, "", "led_mode", LED_MODE_IO, LED_MODE_STATUS, &n, &bad))
        cfg.led_mode = (uint8_t)n;

    cJSON *can_j = cJSON_GetObjectItem(root, "can");
    if (cJSON_IsObject(can_j)) {
        if (web_cfg_uint(can_j, "can.", "mode", 0, 2, &n, &bad))
            cfg.can.mode = (uint8_t)n;
        if (web_cfg_uint(can_j, "can.", "n2k_addr", 1, 251, &n, &bad))
            cfg.can.n2k_addr = (uint8_t)n;
        /* base_id .. base_id + 5 are used for TX and RX matching, so the
           whole block has to fit in the 11-bit standard range. */
        if (web_cfg_uint(can_j, "can.", "base_id", 0, APP_CFG_CAN_BASE_ID_MAX, &n, &bad))
            cfg.can.base_id = (uint16_t)n;
        /* Outside this range twai_new_node_onchip() refuses the value and
           can_server_init() fails — see the boot-order note in main.c. */
        if (web_cfg_uint(can_j, "can.", "bitrate", 10000, 1000000, &n, &bad))
            cfg.can.bitrate = n;
        if (web_cfg_uint(can_j, "can.", "tx_interval_ms", 0, UINT16_MAX, &n, &bad))
            cfg.can.tx_interval_ms = (uint16_t)n;
    }

    cJSON *mb = cJSON_GetObjectItem(root, "modbus");
    if (cJSON_IsObject(mb)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(mb, "enable"))   && cJSON_IsBool(v))
            cfg.modbus.enable = cJSON_IsTrue(v) ? 1 : 0;
        if (web_cfg_uint(mb, "modbus.", "address", 1, 247, &n, &bad))
            cfg.modbus.address = (uint8_t)n;
        if (web_cfg_uint(mb, "modbus.", "baudrate", 1200, 921600, &n, &bad))
            cfg.modbus.baudrate = n;
        if ((v = cJSON_GetObjectItem(mb, "rs485_role")) && cJSON_IsString(v)) {
            if      (!strcmp(v->valuestring, "slave"))  cfg.modbus.rs485_role = MB_ROLE_SLAVE;
            else if (!strcmp(v->valuestring, "master")) cfg.modbus.rs485_role = MB_ROLE_MASTER;
        }
        if ((v = cJSON_GetObjectItem(mb, "tcp_server")) && cJSON_IsBool(v))
            cfg.modbus.tcp_server = cJSON_IsTrue(v) ? 1 : 0;
        if ((v = cJSON_GetObjectItem(mb, "gateway")) && cJSON_IsBool(v))
            cfg.modbus.gateway = cJSON_IsTrue(v) ? 1 : 0;
        if (web_cfg_uint(mb, "modbus.", "tcp_uid", 1, 247, &n, &bad))
            cfg.modbus.tcp_uid = (uint8_t)n;
        if (web_cfg_uint(mb, "modbus.", "rs485_tout_ms", MB_RS485_TOUT_MIN_MS, MB_RS485_TOUT_MAX_MS, &n, &bad))
            cfg.modbus.rs485_tout_ms = (uint16_t)n;

        /* Two combinations are worth refusing rather than quietly accepting.
           A master with no TCP server has nobody to act for, and a local unit
           ID in the range the gateway forwards would shadow the device that
           really has that address -- a request meant for it would be answered
           by this board's own two registers instead. */
        /* The gateway forwards Modbus TCP requests to the segment, so it
           needs both: the segment's master, and the TCP server. The master on
           its own is fine -- it reads the RS-485 rows of the poll table. The
           unit ID is the user's to choose with the gateway on as well; the
           page says that a device with that address on the segment is then
           unreachable through it. */
        if (cfg.modbus.enable && cfg.modbus.gateway) {
            const char *why = NULL;
            if (cfg.modbus.rs485_role != MB_ROLE_MASTER)
                why = "the gateway needs RS-485 set to master";
            else if (!cfg.modbus.tcp_server)
                why = "the gateway needs the Modbus TCP server enabled";
            if (why) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
                return ESP_OK;
            }
        }
    }

    cJSON *mbm_j = cJSON_GetObjectItem(root, "mbm");
    bool mbm_present = cJSON_IsArray(mbm_j);
    if (mbm_present) {
        const char *why = web_mbm_config_parse(mbm_j, &cfg, true, &bad);
        if (why) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
            return ESP_OK;
        }
    }

    cJSON *sntp_j = cJSON_GetObjectItem(root, "sntp");
    if (cJSON_IsObject(sntp_j)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(sntp_j, "enable")) && cJSON_IsBool(v))
            cfg.sntp.enable = cJSON_IsTrue(v) ? 1 : 0;
        web_cfg_str(sntp_j, "sntp.", "server", cfg.sntp.server, sizeof(cfg.sntp.server), &bad);
    }

    web_cfg_str(root, "", "tz", cfg.tz, sizeof(cfg.tz), &bad);

    cJSON_Delete(root);

    if (bad.field[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg), "invalid value for %s (wrong type, out of range or too long)", bad.field);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }

    /* Validate names before saving -- a 400 in plain text, like every other
       refusal here (it was a 200 with a JSON error). */
    const char *err = NULL;
    if (!app_config_io_names_ok(&cfg, &err)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_OK;
    }

    /* The page always sends the table; whether it changed is what counts. */
    bool mbm_changed = mbm_present && memcmp(cfg.mbm, app_config_get()->mbm, sizeof(cfg.mbm)) != 0;

    esp_err_t upd = app_config_update(&cfg);
    if (upd != ESP_OK) {
        /* Nothing below is applied: the live configuration is still the old
           one, and that is what the device keeps running. */
        return web_send_json(req, "500 Internal Server Error", "{\"error\":\"configuration could not be stored\"}");
    }

    /* Only when the table changed. Reloading clears the master's counters and
       drops its connections to every meter, so doing it for a request that
       merely renamed an input or changed the NTP server would throw away
       readings nobody asked to lose. The page sends the table with every
       save, so "the request carried it" was every time.

       The master re-reads the table on its own, but the task only exists once
       something is enabled, and what it knew about the old entries no longer
       describes the new ones. Everything else on this page needs a reboot,
       which is what the form says; this one does not have to. */
    if (mbm_changed) mb_tcp_master_reload();
    /* Before di_publish_all() below. A pad that cannot take its mode is left
       in its standard function; the save itself has happened. */
    esp_err_t aux_ret = aux_io_apply();
    app_time_apply_tz();  /* timezone first, so a sync that follows computes in it */
    sntp_sync_apply();    /* apply any SNTP server / enable change immediately */
    di_publish_all();
    /* The relays take a changed invert here, with or without MQTT: before,
       only dout_on_mqtt_connected() re-applied them, and it is empty in a
       build without MQTT -- the save said ok and the next unrelated output
       write flipped them. */
    esp_err_t dout_ret = dout_publish_all();
    /* Re-subscribe with potentially new names, then publish all DO states. */
    dout_on_mqtt_connected();
    if (app_mqtt_is_connected()) aux_io_on_mqtt_connected();

    if (dout_ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "Saved, but the outputs could not be re-applied and may still use the old invert setting (see the device log)");
        return ESP_OK;
    }
    if (aux_ret != ESP_OK) {
        /* Stored, but not in effect: say so, rather than "ok" next to a mode
           the Device tab shows and the pad does not have. */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "Saved, but a header pad could not be switched and is in its standard function (see the device log)");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ----------------------------------------------------------------- time / RTC */

/* Format a UTC epoch as ISO-8601 "YYYY-MM-DDTHH:MM:SSZ" into buf (>= 21 bytes). */
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
    httpd_resp_set_type(req, web_mime_type(file));

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

#ifdef CONFIG_APP_MATTER_ENABLE
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

/* --------------------------------------------------------------- /api/matter/pairing */

static esp_err_t api_matter_pairing(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);

    const char *qr  = matter_get_qr_code();
    const char *man = matter_get_manual_code();
    bool commissioned = matter_is_commissioned();

    /* Return 404 only when Matter is truly inactive (no QR code AND not commissioned). */
    if ((!qr || !qr[0]) && !commissioned) {
        return web_send_json(req, "404 Not Found", "{\"error\":\"matter_not_enabled\"}");
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
#endif /* CONFIG_APP_MATTER_ENABLE */

/* --------------------------------------------------------------- /api/eth-only */

static void do_restart(void *arg);  /* defined below in /api/reboot section */

static esp_err_t api_eth_only(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);
    if (req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char body[65] = {};
    int received = web_recv_body(req, body, sizeof(body) - 1);
    if (received < 0) {
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

    /* Rebooted only once the flash has it: answering "rebooting" over a
       failed write came back up in the old mode while the page said the
       new one. */
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_ETH_NS, NVS_READWRITE, &nvs_h);
    if (err == ESP_OK) {
        if (want_eth_only) err = nvs_set_u8(nvs_h, NVS_ETH_KEY, 1);
        else {
            err = nvs_erase_key(nvs_h, NVS_ETH_KEY);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        }
        if (err == ESP_OK) err = nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "eth-only: not stored: %s", esp_err_to_name(err));
        return web_send_json(req, "500 Internal Server Error", "{\"error\":\"setting could not be stored, not rebooting\"}");
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
    if (!web_check_auth(req)) return web_send_denied(req);

    /* Erase the entire NVS default partition, then reboot. This clears WiFi
       credentials, all config, and IO names. Erased first, answered after:
       "resetting" over a failed erase rebooted into the old configuration
       and password. */
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
    if (!web_check_auth(req)) return web_send_denied(req);

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
    if (!web_check_auth(req)) return web_send_denied(req);

    cJSON *root = cJSON_CreateObject();
    cJSON *di   = cJSON_AddArrayToObject(root, "di");
    cJSON *dout = cJSON_AddArrayToObject(root, "dout");
    for (int i = 0; i < APP_CFG_DI_COUNT; i++) {
        cJSON_AddItemToArray(di,   cJSON_CreateBool(di_get((uint8_t)i)));
    }
#if APP_CFG_DI_AUX_COUNT
    web_aux_state_json(root);
#endif
    for (int i = 0; i < APP_CFG_DO_COUNT; i++) {
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
        esp_err_t r = web_aux_output(req, (uint8_t)(ch - APP_CFG_DO_COUNT), root);
        cJSON_Delete(root);
        return r;
    }
#endif

    /* A toggle goes to dout_modify() as one: read and flip under the output
       lock. Reading here and setting the opposite lost one of two toggles
       arriving together, and the output ended where neither asked for. */
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

    if (set_ret != ESP_OK) {
        /* dout_set() restores the previous state when the transfer fails, so
           confirming the command here would report a switch that never
           happened. */
        ESP_LOGW(TAG, "output %d: set failed: %s", ch + 1, esp_err_to_name(set_ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "output write failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* POST /api/io/led — {"r":0-255,"g":0-255,"b":0-255} or {"color":"#RRGGBB"} */
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
        uint32_t n;
        if (web_json_uint(rj, 0, 255, &n)) r = (uint8_t)n;
        if (web_json_uint(gj, 0, 255, &n)) g = (uint8_t)n;
        if (web_json_uint(bj, 0, 255, &n)) b = (uint8_t)n;
    }
    cJSON_Delete(root);

    led_set_rgb(r, g, b);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------ rules endpoints */

/* GET /api/rules — returns {"script":"..."} from NVS, or the demo script if unset.
   Only "unset" means the demo: a stored script that cannot be read is an
   error, not the demo -- the UI reloads the script after saving and showed
   the demo in place of the user's code, ready to be saved over it. */
esp_err_t rules_nvs_read(char **out)
{
    *out = NULL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(RULES_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    /* Size, then read -- and a save may land in between: a script stored
       after a 202 grew past the buffer, NVS answered INVALID_LENGTH with the
       new size in len, and the terminator went past the end of the buffer.
       The buffer's own size bounds every write. A script that changed in
       between -- grew, or moved from the old string to the blob -- is looked
       up again from the start. */
    for (int tries = 0; tries < 3; tries++) {
        bool blob = true;
        size_t len = 0;
        err = nvs_get_blob(h, RULES_NVS_BLOB, NULL, &len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            blob = false;
            err = nvs_get_str(h, RULES_NVS_KEY, NULL, &len);
        }
        if (err != ESP_OK) break;            /* NOT_FOUND: none stored */
        size_t cap = len;
        char *buf = malloc(cap + 1);
        if (!buf) { err = ESP_ERR_NO_MEM; break; }
        err = blob ? nvs_get_blob(h, RULES_NVS_BLOB, buf, &len)
                   : nvs_get_str(h, RULES_NVS_KEY, buf, &len);
        if (err == ESP_OK && len <= cap) {
            buf[len] = '\0';
            *out = buf;
            break;
        }
        free(buf);
        if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;   /* never: NVS keeps within cap */
        if (err != ESP_ERR_NVS_INVALID_LENGTH && err != ESP_ERR_NVS_NOT_FOUND) break;
        err = ESP_ERR_INVALID_STATE;                     /* changed while read, and kept changing */
    }
    nvs_close(h);
    return err;
}

static esp_err_t api_rules_get(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);

    char *buf = NULL;
    const char *script = DEMO_SCRIPT;
    esp_err_t err = rules_nvs_read(&buf);
    if (err == ESP_OK) script = buf;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "stored rules unreadable: %s", esp_err_to_name(err));
        char msg[128];
        snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"error\":\"stored rules unreadable: %s\"}",
                 esp_err_to_name(err));
        return web_send_json(req, "500 Internal Server Error", msg);
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
/* Makes to_store the script the next boot loads ("" = the built-in demo).
   Called by the rule engine, on its own task, once it has accepted the
   script -- see scripting_reload_and_store(). */
static esp_err_t store_rules(const char *to_store)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(RULES_NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    if (to_store[0] == '\0') {
        ret = nvs_erase_key(h, RULES_NVS_BLOB);
        if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;   /* erasing what was not there */
    } else {
        ret = nvs_set_blob(h, RULES_NVS_BLOB, to_store, strlen(to_store));
    }
    if (ret == ESP_OK) {
        /* The blob comes first when reading, but the old string would take
           up a page's worth of entries for nothing. */
        esp_err_t e = nvs_erase_key(h, RULES_NVS_KEY);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) ret = e;
    }
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

static esp_err_t api_rules_post(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);
    /* Room for the escapes, at worst six bytes for one (\u00e4 for an umlaut,
       as Python's json writes it); the script's own length is checked below. */
    if (req->content_len == 0 || req->content_len > 6 * RULES_MAX_LEN + 100) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_OK; }

    int n = web_recv_body(req, body, req->content_len);
    if (n < 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv"); return ESP_OK; }

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
    size_t script_len = strlen(script);
    if (script_len > RULES_MAX_LEN) {
        cJSON_Delete(root);
        char msg[160];
        snprintf(msg, sizeof(msg), "{\"status\":\"rejected\",\"error\":\"script too long: %u bytes of UTF-8, at most %d\","
                 "\"detail\":\"previous rules still running\"}", (unsigned)script_len, RULES_MAX_LEN);
        return web_send_json(req, "400 Bad Request", msg);
    }

    /* The engine first, the flash second. Stored before the verdict, a
       script the engine refused -- a syntax error, say -- was what the
       device booted into next time, with no fallback. The last script the
       engine accepted is the only one worth keeping -- so the engine stores
       it itself, the moment it accepts it (store_rules, on its task).
       Stored here after the verdict instead, a script accepted after this
       handler had stopped waiting was running but never stored, and the next
       boot brought the old one back.

       Wait for the engine's verdict instead of acknowledging a request we
       cannot see through. A reload can be refused -- a syntax error, a global
       const clashing with the previous script, no free timers -- and the rules
       already running then stay in place, which the caller has to be told.

       Bounded on purpose: handlers run on the server's own task, so a long
       wait here would hold up every other request. A reload normally finishes
       in milliseconds; if it has not after RULES_APPLY_TIMEOUT_MS the request
       is answered with 202, and the engine still stores the script if it
       accepts it. The verdict is matched to this request by ticket. */
    uint32_t mine = scripting_reload_and_store(script[0] ? script : DEMO_SCRIPT,
                                               script, store_rules);
    cJSON_Delete(root);
    if (!mine)
        return web_send_json(req, "503 Service Unavailable", "{\"status\":\"busy\",\"detail\":\"engine queue full, nothing stored\"}");

    scripting_reload_status_t now = { 0 };
    for (int waited = 0; waited < RULES_APPLY_TIMEOUT_MS; waited += 10) {
        scripting_reload_status(&now);
        if (now.ticket >= mine) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (now.ticket != mine) {
        /* No verdict for this request yet, or already a later one's: the
           engine stores this script if and when it accepts it. */
        return web_send_json(req, "202 Accepted", "{\"status\":\"accepted\",\"detail\":\"pending; stored once the engine accepts it\"}");
    }

    if (!now.ok) {
        /* Refused and not stored: say so plainly, including that the previous
           rules are the ones still in effect, in memory and in flash. */
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "rejected");
        cJSON_AddStringToObject(resp, "error", now.message);
        cJSON_AddStringToObject(resp, "detail", "previous rules still running");
        char *body_out = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body_out ? body_out : "{\"status\":\"rejected\"}");
        free(body_out);
        return ESP_OK;
    }

    /* Accepted and running. A failed write is reported rather than
       discarded: the rules run until the next reboot, when the previous
       script comes back, and the caller has to know that. */
    if (!now.store_tried || now.store_err != ESP_OK) {
        char msg[192];
        snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"error\":\"rules running but not stored: %s\","
                 "\"detail\":\"the previous script returns at the next reboot\"}",
                 now.store_tried ? esp_err_to_name(now.store_err) : "not tried");
        return web_send_json(req, "500 Internal Server Error", msg);
    }

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
    /* Why the board last started: a watchdog or a panic otherwise leaves no
       trace unless something happened to be reading the console. */
    static const char *const reasons[] = {
        [ESP_RST_UNKNOWN] = "unknown",   [ESP_RST_POWERON] = "poweron",  [ESP_RST_EXT] = "ext",
        [ESP_RST_SW] = "sw",             [ESP_RST_PANIC] = "panic",      [ESP_RST_INT_WDT] = "int_wdt",
        [ESP_RST_TASK_WDT] = "task_wdt", [ESP_RST_WDT] = "wdt",          [ESP_RST_DEEPSLEEP] = "deepsleep",
        [ESP_RST_BROWNOUT] = "brownout", [ESP_RST_SDIO] = "sdio",        [ESP_RST_USB] = "usb",
        [ESP_RST_JTAG] = "jtag",
    };
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rs = ((size_t)rr < sizeof(reasons) / sizeof(reasons[0]) && reasons[rr]) ? reasons[rr] : "other";
    char json[200];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"idf\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"reset_reason\":\"%s\"}",
             d->version, d->idf_ver, d->date, d->time, rs);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* -------------------------------------------------------------- start / stop */

/* What the two Modbus sides are doing, as opposed to how they are set up.
   Without it a value that never arrives looks the same as one that arrives
   wrong. */
static esp_err_t api_modbus_status(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    mb_status_json(root);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    return ESP_OK;
}

static const httpd_uri_t s_handlers[] = {
    { .uri = "/api/auth/status",       .method = HTTP_GET,  .handler = web_api_auth_status       },
    { .uri = "/api/auth/begin",        .method = HTTP_POST, .handler = web_api_auth_begin        },
    { .uri = "/api/auth/token",        .method = HTTP_GET,  .handler = web_api_auth_token        },
    { .uri = "/api/auth/set-password", .method = HTTP_POST, .handler = web_api_auth_set_password },
    { .uri = "/api/config",            .method = HTTP_GET,  .handler = api_config_get        },
    { .uri = "/api/config",            .method = HTTP_POST, .handler = api_config_post       },
    { .uri = "/api/io/state",          .method = HTTP_GET,  .handler = api_io_state          },
    { .uri = "/api/modbus/status",     .method = HTTP_GET,  .handler = api_modbus_status     },
    { .uri = "/api/io/output",         .method = HTTP_POST, .handler = api_io_output         },
    { .uri = "/api/io/led",            .method = HTTP_POST, .handler = api_io_led            },
    { .uri = "/api/io/buzzer",         .method = HTTP_POST, .handler = web_api_io_buzzer     },
#ifdef CONFIG_APP_MATTER_ENABLE
    { .uri = "/api/matter/pairing",       .method = HTTP_GET,  .handler = api_matter_pairing      },
    { .uri = "/api/matter/decommission", .method = HTTP_POST, .handler = api_matter_decommission },
#endif
    { .uri = "/api/eth-only",          .method = HTTP_POST, .handler = api_eth_only          },
    { .uri = "/api/reboot",            .method = HTTP_POST, .handler = api_reboot            },
    { .uri = "/api/factory-reset",     .method = HTTP_POST, .handler = api_factory_reset     },
    { .uri = "/api/rules",             .method = HTTP_GET,  .handler = api_rules_get         },
    { .uri = "/api/rules",             .method = HTTP_POST, .handler = api_rules_post        },
    { .uri = "/api/time",              .method = HTTP_GET,  .handler = web_api_time_get      },
    { .uri = "/api/time",              .method = HTTP_POST, .handler = web_api_time_post     },
    { .uri = "/api/version",           .method = HTTP_GET,  .handler = api_version           },
    { .uri = "/*",                     .method = HTTP_GET,  .handler = file_get              },
};

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = NET_SOCK_HTTPD_CLIENTS;   /* the share net_budget.h allots */
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    /* Derive from the table so adding a handler never overflows the cap and
     * silently drops the last registration (the wildcard file-serving fallback). */
    cfg.max_uri_handlers = sizeof(s_handlers) / sizeof(s_handlers[0]);
    /* Allocate the httpd task stack from the reserved internal DMA pool
     * (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), not from PSRAM.
     * PSRAM stacks fail the esp_ptr_in_dram() check inside the SPI-flash driver,
     * which asserts before every NVS write.  After Matter init the general internal
     * heap is nearly exhausted (~1.5 KB), but CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL
     * keeps 32 KB of DMA-capable internal RAM aside for exactly this kind of request,
     * and the DMA pool still has ~29 KB free at that point. */
    cfg.task_caps        = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    cfg.send_wait_timeout = 10;   /* seconds; default 5 is too tight with Matter on WiFi */
    /* HTTPD_DEFAULT_CONFIG gives 4096. A handler holds a whole app_config_t
       (1.7 kB) or the Modbus status table (640 B) plus cJSON's own working
       set, so the default leaves too little. sdkconfig carried a
       CONFIG_HTTPD_STACK_SIZE line for years that does not exist as a Kconfig
       symbol and therefore never did anything. */
    cfg.stack_size        = 6144;

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
