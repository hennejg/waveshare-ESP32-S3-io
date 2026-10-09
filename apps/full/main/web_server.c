#include "web_server.h"
#include "net_budget.h"
#include "app_config.h"
#include "app_time.h"
#include "auth.h"
#include "buzzer.h"
#include "di.h"
#include "dout.h"
#include "aux_io.h"
#include "app_mqtt.h"
#include "esp_system.h"
#include "led.h"
#ifdef CONFIG_APP_MATTER_ENABLE
#include "matter.h"
#endif
#include "app_rtc.h"
#include "mb_tcp_server.h"
#include "mb_tcp_master.h"
#include "scripting.h"
#include "sntp_sync.h"

#include <time.h>
#include <sys/time.h>

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

#include <math.h>
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
#include "mbedtls/base64.h"

#define TAG        "web_server"

/* Longest password whose HTTP Basic header still fits check_auth()'s 160-byte
   buffer: "Basic " + base64(":" + password) must stay within 159 characters,
   which 113 does at 158 and 114 exceeds at 162. */
#define APP_CFG_PASSWORD_MAX 113

/* Total time a request body may take to arrive. */
#define BODY_RECV_TIMEOUT_MS 5000

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

/* httpd_req_recv() returns what a single socket read yielded. The loop in
   httpd_recv_with_opt() only repeats for HTTPD_RECV_OPT_BLOCKING, and
   httpd_recv() passes HTTPD_RECV_OPT_NONE, so one call can return far less
   than content_len. Measured on an ESP32-S3-POE-ETH-8DI-8DO with a 2895-byte
   rule script: a single call returned 1440 bytes when the body was sent in one
   go, 724 over four segments and 145 over twenty -- all three then failed to
   parse and answered 400 Invalid JSON.

   buf must hold content_len + 1 bytes. Returns the length read, or -1. */
/* A JSON number as an unsigned integer within [lo, hi]; false if it is not
   a number or lies outside. Checked as a double, before any conversion: a
   double outside the target type's range converts undefined in C, and in
   practice it wrapped -- base_id 4294967295 became 0xFFFFFFFF, "+ 5" made
   it 4, the 11-bit check passed and 0xFFFF was stored. Fractions are cut
   off, as the casts did before. NaN fails both comparisons. */
static bool json_uint(const cJSON *v, uint32_t lo, uint32_t hi, uint32_t *out)
{
    if (!cJSON_IsNumber(v)) return false;
    double d = v->valuedouble;
    if (!(d >= lo && d < (double)hi + 1.0)) return false;
    *out = (uint32_t)d;
    return true;
}

static int recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t want = req->content_len;
    if (want > cap) return -1;

    /* The deadline is absolute, not per read: a client that dribbles one byte
       at a time must not be able to hold the handler open indefinitely. The
       server processes handlers on its own task, so a stalled body would block
       every other request -- and /api/auth/set-password reads its body before
       the token is checked, so this is reachable without credentials. */
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

/* Returns true if the request carries a valid password (or no password is set). */
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

    // HTTP Basic format is "user:password"; accept any username, check password only.
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
       out of the very API that set it. "Basic " plus base64 of ":" + password
       must stay within that buffer -- see APP_CFG_PASSWORD_MAX. */
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

/* The value types and the two readable function codes travel as names rather
   than numbers: a configuration is read by people, and "f32" says what 4 does
   not. */
/* One table rather than two chains: they are exact inverses, and a type added
   to one of them alone would read back as something it is not. u16 is the
   default on both sides and so needs no row. */
static const struct { const char *name; uint8_t type; } k_mbm_types[] = {
    { "s16", MBM_VAL_S16 },
    { "u32", MBM_VAL_U32 },
    { "s32", MBM_VAL_S32 },
    { "f32", MBM_VAL_F32 },
};

static const char *mbm_type_name(uint8_t t)
{
    for (size_t i = 0; i < sizeof(k_mbm_types) / sizeof(k_mbm_types[0]); i++)
        if (k_mbm_types[i].type == t) return k_mbm_types[i].name;
    return "u16";
}

static void mbm_type_value(const char *s, uint8_t *out)
{
    for (size_t i = 0; i < sizeof(k_mbm_types) / sizeof(k_mbm_types[0]); i++)
        if (!strcmp(s, k_mbm_types[i].name)) { *out = k_mbm_types[i].type; return; }
    *out = MBM_VAL_U16;
}

static esp_err_t api_config_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_denied(req);
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
    cJSON *mbm = cJSON_AddArrayToObject(root, "mbm");
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
        const mbm_poll_t *e = &cfg->mbm[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject  (o, "enable",      e->enable);
        cJSON_AddStringToObject(o, "name",        e->name);
        cJSON_AddStringToObject(o, "host",        e->host);
        cJSON_AddNumberToObject(o, "port",        e->port);
        cJSON_AddNumberToObject(o, "unit_id",     e->unit_id);
        cJSON_AddStringToObject(o, "fc",          e->fc == 4 ? "input" : "holding");
        cJSON_AddNumberToObject(o, "reg",         e->reg);
        cJSON_AddStringToObject(o, "type",        mbm_type_name(e->type));
        cJSON_AddBoolToObject  (o, "word_swap",   e->word_swap);
        cJSON_AddStringToObject(o, "transport",   e->transport == MBM_TRANSPORT_RTU ? "rtu" : "tcp");
        /* Reported as stored. A stored 0 cannot occur: an enabled row with it
           is refused on the way in, a disabled one stores 1, and
           app_config_init() turns one from an older blob into 1. */
        cJSON_AddNumberToObject(o, "scale",       e->scale);
        cJSON_AddNumberToObject(o, "interval_ms", e->interval_ms);
        cJSON_AddItemToArray(mbm, o);
    }

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
    /* The IO1/RXD/TXD pads: mode, the modes this pad offers, and the PWM
       settings (kept while the pad is in another mode). */
    cJSON *aux = cJSON_AddArrayToObject(root, "aux");
    for (int a = 0; a < APP_CFG_DI_AUX_COUNT; a++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mode", aux_mode_name((aux_mode_t)cfg->aux_mode[a]));
        cJSON *modes = cJSON_AddArrayToObject(o, "modes");
        for (int m = 0; m < AUX_MODE_COUNT; m++)
            if (aux_mode_allowed((uint8_t)a, (aux_mode_t)m))
                cJSON_AddItemToArray(modes, cJSON_CreateString(aux_mode_name((aux_mode_t)m)));
        cJSON_AddNumberToObject(o, "pwm_freq", cfg->aux_pwm[a].freq_hz);
        cJSON_AddNumberToObject(o, "pwm_duty", cfg->aux_pwm[a].duty_pm / 10.0);
        cJSON_AddStringToObject(o, "out_name",   cfg->aux_out[a].name);
        cJSON_AddBoolToObject  (o, "out_invert", cfg->aux_out[a].invert);
        cJSON_AddItemToArray(aux, o);
    }
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
    if (!check_auth(req)) return send_denied(req);
    if (req->content_len > BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    int received = recv_body(req, body, req->content_len);
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

    /* String fields — macro to keep it terse */
    #define STR(key, dst) do { \
        cJSON *_v = cJSON_GetObjectItem(root, key); \
        if (cJSON_IsString(_v)) strlcpy(dst, _v->valuestring, sizeof(dst)); \
    } while (0)

    STR("device_name",      cfg.device_name);
    STR("mqtt_url",         cfg.mqtt_url);
    STR("mqtt_user",        cfg.mqtt_user);
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
            strlcpy(cfg.mqtt_topic_prefix, pfx, sizeof(cfg.mqtt_topic_prefix));
        }
    }
    #undef STR

    /* Password: only update when a non-empty value is sent */
    cJSON *pass = cJSON_GetObjectItem(root, "mqtt_password");
    if (cJSON_IsString(pass) && pass->valuestring[0])
        strlcpy(cfg.mqtt_password, pass->valuestring, sizeof(cfg.mqtt_password));

    parse_invert_array(cJSON_GetObjectItem(root, "di"),   cfg.di,   APP_CFG_DI_COUNT);
#if APP_CFG_DI_AUX_COUNT
    /* Only what is sent changes: a request without "aux", a shorter array, or
       an entry without a field leaves the rest as it is. A value outside its
       range is refused rather than clamped -- a PWM output is driving
       something, and quietly running it at another frequency or duty than
       asked is not a safe guess. */
    const char *aux_err = NULL;
    cJSON *aux_j = cJSON_GetObjectItem(root, "aux");
    if (cJSON_IsArray(aux_j)) {
        int n = cJSON_GetArraySize(aux_j);
        for (int a = 0; a < n && a < APP_CFG_DI_AUX_COUNT && !aux_err; a++) {
            cJSON *o = cJSON_GetArrayItem(aux_j, a);
            if (!cJSON_IsObject(o)) continue;
            cJSON *v;
            if ((v = cJSON_GetObjectItem(o, "mode"))) {
                aux_mode_t m;
                if (!cJSON_IsString(v) || !aux_mode_parse(v->valuestring, &m) ||
                    !aux_mode_allowed((uint8_t)a, m))
                    aux_err = "aux mode not available on this pad";
                else
                    cfg.aux_mode[a] = (uint8_t)m;
            }
            if (!aux_err && (v = cJSON_GetObjectItem(o, "pwm_freq"))) {
                if (!cJSON_IsNumber(v) || !aux_pwm_freq_ok(v->valuedouble))
                    aux_err = "pwm_freq must be a whole number of Hz from 5 to 1000000";
                else
                    cfg.aux_pwm[a].freq_hz = (uint32_t)v->valuedouble;
            }
            if (!aux_err && (v = cJSON_GetObjectItem(o, "pwm_duty"))) {
                if (!cJSON_IsNumber(v) || !aux_pwm_duty_from_pct(v->valuedouble, &cfg.aux_pwm[a].duty_pm))
                    aux_err = "pwm_duty must be a percentage from 0 to 100";
            }
            if (!aux_err && (v = cJSON_GetObjectItem(o, "out_name"))) {
                if (!cJSON_IsString(v) || strlen(v->valuestring) > APP_CFG_IO_NAME_MAX)
                    aux_err = "out_name must be a string of at most 20 characters";
                else
                    strlcpy(cfg.aux_out[a].name, v->valuestring, sizeof(cfg.aux_out[a].name));
            }
            if (!aux_err && (v = cJSON_GetObjectItem(o, "out_invert"))) {
                if (!cJSON_IsBool(v)) aux_err = "out_invert must be true or false";
                else cfg.aux_out[a].invert = cJSON_IsTrue(v);
            }
        }
    }
    if (aux_err) {
        /* Plain text, like every other refusal of this handler: the page
           shows the body as it is. */
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, aux_err);
        return ESP_OK;
    }
#endif
    parse_invert_array(cJSON_GetObjectItem(root, "dout"), cfg.dout, APP_CFG_DO_COUNT);

    uint32_t n;
    if (json_uint(cJSON_GetObjectItem(root, "led_mode"), LED_MODE_IO, LED_MODE_STATUS, &n))
        cfg.led_mode = (uint8_t)n;

    cJSON *can_j = cJSON_GetObjectItem(root, "can");
    if (cJSON_IsObject(can_j)) {
        if (json_uint(cJSON_GetObjectItem(can_j, "mode"), 0, 2, &n))
            cfg.can.mode = (uint8_t)n;
        if (json_uint(cJSON_GetObjectItem(can_j, "n2k_addr"), 1, 251, &n))
            cfg.can.n2k_addr = (uint8_t)n;
        /* base_id .. base_id + 5 are used for TX and RX matching, so the
           whole block has to fit in the 11-bit standard range. */
        if (json_uint(cJSON_GetObjectItem(can_j, "base_id"), 0, APP_CFG_CAN_BASE_ID_MAX, &n))
            cfg.can.base_id = (uint16_t)n;
        /* Outside this range twai_new_node_onchip() refuses the value and
           can_server_init() fails — see the boot-order note in main.c. */
        if (json_uint(cJSON_GetObjectItem(can_j, "bitrate"), 10000, 1000000, &n))
            cfg.can.bitrate = n;
        if (json_uint(cJSON_GetObjectItem(can_j, "tx_interval_ms"), 0, UINT16_MAX, &n))
            cfg.can.tx_interval_ms = (uint16_t)n;
    }

    cJSON *mb = cJSON_GetObjectItem(root, "modbus");
    if (cJSON_IsObject(mb)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(mb, "enable"))   && cJSON_IsBool(v))
            cfg.modbus.enable = cJSON_IsTrue(v) ? 1 : 0;
        if (json_uint(cJSON_GetObjectItem(mb, "address"), 1, 247, &n))
            cfg.modbus.address = (uint8_t)n;
        if (json_uint(cJSON_GetObjectItem(mb, "baudrate"), 1200, 921600, &n))
            cfg.modbus.baudrate = n;
        if ((v = cJSON_GetObjectItem(mb, "rs485_role")) && cJSON_IsString(v)) {
            if      (!strcmp(v->valuestring, "slave"))  cfg.modbus.rs485_role = MB_ROLE_SLAVE;
            else if (!strcmp(v->valuestring, "master")) cfg.modbus.rs485_role = MB_ROLE_MASTER;
        }
        if ((v = cJSON_GetObjectItem(mb, "tcp_server")) && cJSON_IsBool(v))
            cfg.modbus.tcp_server = cJSON_IsTrue(v) ? 1 : 0;
        if ((v = cJSON_GetObjectItem(mb, "gateway")) && cJSON_IsBool(v))
            cfg.modbus.gateway = cJSON_IsTrue(v) ? 1 : 0;
        if (json_uint(cJSON_GetObjectItem(mb, "tcp_uid"), 1, 247, &n))
            cfg.modbus.tcp_uid = (uint8_t)n;
        if (json_uint(cJSON_GetObjectItem(mb, "rs485_tout_ms"), MB_RS485_TOUT_MIN_MS, MB_RS485_TOUT_MAX_MS, &n))
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
        /* The array replaces the table wholesale rather than being merged into
           it: a partial update would leave an entry the user deleted in the
           UI still being polled. Entries past the end are cleared. */
        memset(cfg.mbm, 0, sizeof(cfg.mbm));
        int n = cJSON_GetArraySize(mbm_j);
        if (n > APP_CFG_MBM_COUNT) n = APP_CFG_MBM_COUNT;

        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_GetArrayItem(mbm_j, i);
            if (!cJSON_IsObject(o)) continue;
            mbm_poll_t *e = &cfg.mbm[i];
            cJSON *v;
            uint32_t u;

            e->port = MBM_PORT_DEFAULT; e->unit_id = MBM_UNIT_ID_DEFAULT;
            e->fc = MBM_FC_DEFAULT; e->scale = 1.0f; e->interval_ms = MBM_INTERVAL_DEFAULT_MS;

            /* A script writing "enable": 1 rather than true would otherwise
               store a silently disabled entry and get 200 back. */
            if ((v = cJSON_GetObjectItem(o, "enable"))) {
                if (cJSON_IsBool(v))        e->enable = cJSON_IsTrue(v) ? 1 : 0;
                else if (cJSON_IsNumber(v)) e->enable = v->valuedouble != 0 ? 1 : 0;
            }
            if ((v = cJSON_GetObjectItem(o, "name")) && cJSON_IsString(v))
                strlcpy(e->name, v->valuestring, sizeof(e->name));
            if ((v = cJSON_GetObjectItem(o, "host")) && cJSON_IsString(v))
                strlcpy(e->host, v->valuestring, sizeof(e->host));
            if (json_uint(cJSON_GetObjectItem(o, "port"), 1, 65535, &u))
                e->port = (uint16_t)u;
            if (json_uint(cJSON_GetObjectItem(o, "unit_id"), 1, 247, &u))
                e->unit_id = (uint8_t)u;
            if ((v = cJSON_GetObjectItem(o, "fc")) && cJSON_IsString(v))
                e->fc = strcmp(v->valuestring, "input") == 0 ? 4 : 3;
            if (json_uint(cJSON_GetObjectItem(o, "reg"), 0, 65535, &u))
                e->reg = (uint16_t)u;
            if ((v = cJSON_GetObjectItem(o, "type")) && cJSON_IsString(v))
                mbm_type_value(v->valuestring, &e->type);
            if ((v = cJSON_GetObjectItem(o, "word_swap")) && cJSON_IsBool(v))
                e->word_swap = cJSON_IsTrue(v) ? 1 : 0;
            if ((v = cJSON_GetObjectItem(o, "transport")) && cJSON_IsString(v))
                e->transport = strcmp(v->valuestring, "rtu") == 0 ? MBM_TRANSPORT_RTU : MBM_TRANSPORT_TCP;
            if ((v = cJSON_GetObjectItem(o, "scale")) && cJSON_IsNumber(v)) {
                /* The only field with no natural range, and the one that can
                   carry an infinity straight through to MQTT: cJSON parses
                   1e400 as inf, cJSON_IsNumber accepts it, and the published
                   value becomes the literal "inf". Zero is refused too --
                   it silently turns every reading into nothing. */
                /* Checked as stored: 1e-50 is a non-zero double and a zero
                   float. */
                double sc  = v->valuedouble;
                float  scf = (float)sc;
                if (isfinite(sc) && sc > -1e9 && sc < 1e9 && scf != 0.0f)
                    e->scale = scf;
                else if (e->enable)
                    e->scale = 0.0f;   /* flagged below */
                else
                    /* Nothing reads a disabled row, so nothing to refuse --
                       but storing 0 would hand it out on GET, and enabling
                       the row as it stands would then be refused. */
                    e->scale = 1.0f;
            }
            if (json_uint(cJSON_GetObjectItem(o, "interval_ms"), MBM_INTERVAL_MIN_MS, MBM_INTERVAL_MAX_MS, &u))
                e->interval_ms = u;

            /* A name that is empty or carries MQTT syntax would make a topic
               nothing can subscribe to, so the entry is refused rather than
               quietly polled into nowhere. */
            if (e->enable) {
                const char *why = NULL;
                if (!e->name[0])                  why = "a Modbus master entry needs a name";
                else if (strpbrk(e->name, "#+/")) why = "a Modbus master name must not contain '#', '+' or '/'";
                else if (!e->host[0] && e->transport != MBM_TRANSPORT_RTU)
                                                  why = "a Modbus master entry needs a host";
                else if (e->scale == 0.0f)        why = "a Modbus master scale must be a non-zero finite number";
                else {
                    /* Two entries of the same name would publish to one topic
                       and feed one rule key, last poll winning at random.
                       app_config_io_names_ok() does the same for the I/O names. */
                    for (int j = 0; j < i; j++)
                        if (cfg.mbm[j].enable && !strcmp(cfg.mbm[j].name, e->name)) {
                            why = "two Modbus master entries have the same name";
                            break;
                        }
                }
                if (why) {
                    cJSON_Delete(root);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
                    return ESP_OK;
                }
            }
        }
    }

    cJSON *sntp_j = cJSON_GetObjectItem(root, "sntp");
    if (cJSON_IsObject(sntp_j)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(sntp_j, "enable")) && cJSON_IsBool(v))
            cfg.sntp.enable = cJSON_IsTrue(v) ? 1 : 0;
        if ((v = cJSON_GetObjectItem(sntp_j, "server")) && cJSON_IsString(v))
            strlcpy(cfg.sntp.server, v->valuestring, sizeof(cfg.sntp.server));
    }

    cJSON *tz_j = cJSON_GetObjectItem(root, "tz");
    if (cJSON_IsString(tz_j))
        strlcpy(cfg.tz, tz_j->valuestring, sizeof(cfg.tz));

    cJSON_Delete(root);

    /* Validate names before saving */
    const char *err = NULL;
    if (!app_config_io_names_ok(&cfg, &err)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"message\":\"%s\"}", err);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    esp_err_t upd = app_config_update(&cfg);
    if (upd != ESP_OK) {
        /* Nothing below is applied: the live configuration is still the old
           one, and that is what the device keeps running. */
        return send_json(req, "500 Internal Server Error", "{\"error\":\"configuration could not be stored\"}");
    }

    /* Only when this request actually carried the table. Reloading clears the
       master's counters and drops its connections to every meter, so doing it
       for a request that merely renamed an input or changed the NTP server
       would throw away readings nobody asked to lose.

       The master re-reads the table on its own, but the task only exists once
       something is enabled, and what it knew about the old entries no longer
       describes the new ones. Everything else on this page needs a reboot,
       which is what the form says; this one does not have to. */
    if (mbm_present) mb_tcp_master_reload();
    /* Before di_publish_all() below. A pad that cannot take its mode is left
       in its standard function; the save itself has happened. */
    esp_err_t aux_ret = aux_io_apply();
    app_time_apply_tz();  /* timezone first, so a sync that follows computes in it */
    sntp_sync_apply();    /* apply any SNTP server / enable change immediately */
    di_publish_all();
    /* Re-subscribe with potentially new names, then publish all DO states. */
    dout_on_mqtt_connected();
    if (app_mqtt_is_connected()) aux_io_on_mqtt_connected();

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
static void iso8601_utc(time_t t, char *buf, size_t len)
{
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/* GET /api/time — report the system clock and the battery-backed RTC.
 * All times are UTC. "rtc_valid" is false when the RTC reports an oscillator
 * stop (i.e. it lost power and its time is meaningless). */
static esp_err_t api_time_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_denied(req);
    time_t now = time(NULL);

    struct tm rtc_tm;
    bool rtc_valid = false;
    bool rtc_ok    = (rtc_dev_read(&rtc_tm, &rtc_valid) == ESP_OK);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "epoch", (double)now);
    char iso[24];
    iso8601_utc(now, iso, sizeof(iso));
    cJSON_AddStringToObject(root, "utc", iso);

    cJSON_AddBoolToObject(root, "rtc_present", rtc_ok);
    cJSON_AddBoolToObject(root, "rtc_valid",   rtc_ok && rtc_valid);
    if (rtc_ok && rtc_valid) {
        /* struct tm from the RTC is UTC; turn it back into an epoch for display. */
        char riso[24];
        strftime(riso, sizeof(riso), "%Y-%m-%dT%H:%M:%SZ", &rtc_tm);
        cJSON_AddStringToObject(root, "rtc_utc", riso);
    }

    /* Local time and the configured zone (TZ is applied process-wide via tzset). */
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    char local[32];
    strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &local_tm);
    cJSON_AddStringToObject(root, "local", local);
    cJSON_AddStringToObject(root, "tz", app_config_get()->tz);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    return ESP_OK;
}

/* POST /api/time {"epoch": <unix seconds, UTC>} — set the system clock and
 * mirror it to the RTC. Used to set the time manually from the browser. */
static esp_err_t api_time_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_denied(req);
    if (req->content_len > BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }
    int received = recv_body(req, body, req->content_len);
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

    cJSON *epoch = cJSON_GetObjectItem(root, "epoch");
    /* Up to 2099: the RTC counts years 2000..2099 (rtc.c), and beyond
       time_t the cast below would be undefined. */
    if (!cJSON_IsNumber(epoch) || !(epoch->valuedouble >= 1000000000.0 &&
                                    epoch->valuedouble < 4102444800.0)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing/invalid epoch");
        return ESP_OK;
    }
    time_t t = (time_t)epoch->valuedouble;
    cJSON_Delete(root);

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    struct tm utc;
    gmtime_r(&t, &utc);
    esp_err_t rtc_ret = rtc_dev_set(&utc);

    char iso[24];
    iso8601_utc(t, iso, sizeof(iso));
    ESP_LOGI(TAG, "time set from UI: %s (RTC %s)", iso,
             rtc_ret == ESP_OK ? "updated" : "unavailable");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "utc", iso);
    cJSON_AddBoolToObject(resp, "rtc_updated", rtc_ret == ESP_OK);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{\"status\":\"ok\"}");
    cJSON_free(json);
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

#ifdef CONFIG_APP_MATTER_ENABLE
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

/* --------------------------------------------------------------- /api/matter/pairing */

static esp_err_t api_matter_pairing(httpd_req_t *req)
{
    if (!check_auth(req)) return send_denied(req);

    const char *qr  = matter_get_qr_code();
    const char *man = matter_get_manual_code();
    bool commissioned = matter_is_commissioned();

    /* Return 404 only when Matter is truly inactive (no QR code AND not commissioned). */
    if ((!qr || !qr[0]) && !commissioned) {
        return send_json(req, "404 Not Found", "{\"error\":\"matter_not_enabled\"}");
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
    if (!check_auth(req)) return send_denied(req);
    if (req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_OK;
    }

    char body[65] = {};
    int received = recv_body(req, body, sizeof(body) - 1);
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
        return send_json(req, "500 Internal Server Error", "{\"error\":\"setting could not be stored, not rebooting\"}");
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
    if (!check_auth(req)) return send_denied(req);

    /* Erase the entire NVS default partition, then reboot. This clears WiFi
       credentials, all config, and IO names. Erased first, answered after:
       "resetting" over a failed erase rebooted into the old configuration
       and password. */
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: erase failed: %s", esp_err_to_name(err));
        return send_json(req, "500 Internal Server Error", "{\"error\":\"erase failed, nothing reset\"}");
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
    if (!check_auth(req)) return send_denied(req);

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
    if (!check_auth(req)) return send_denied(req);

    cJSON *root = cJSON_CreateObject();
    cJSON *di   = cJSON_AddArrayToObject(root, "di");
    cJSON *dout = cJSON_AddArrayToObject(root, "dout");
    for (int i = 0; i < APP_CFG_DI_COUNT; i++) {
        cJSON_AddItemToArray(di,   cJSON_CreateBool(di_get((uint8_t)i)));
    }
#if APP_CFG_DI_AUX_COUNT
    /* Which channels are inputs right now; an auxiliary one in another mode
       reads false above without meaning "low". */
    cJSON *act = cJSON_AddArrayToObject(root, "di_active");
    for (int i = 0; i < APP_CFG_DI_COUNT; i++)
        cJSON_AddItemToArray(act, cJSON_CreateBool(di_is_active((uint8_t)i)));
    /* The pads as they run: PWM as the timer really produces it, analog as
       read now. */
    cJSON *aux = cJSON_AddArrayToObject(root, "aux");
    for (int a = 0; a < APP_CFG_DI_AUX_COUNT; a++) {
        cJSON *o = cJSON_CreateObject();
        aux_status_t st;                         /* the pad at one moment */
        aux_io_status((uint8_t)a, &st);
        cJSON_AddStringToObject(o, "mode", aux_mode_name(st.mode));
        if (st.mode == AUX_MODE_PWM) {
            /* As it runs, which Modbus, MQTT or a rule may have changed since
               the configuration was saved: the values as set -- what Modbus
               and MQTT report too -- and the frequency the timer actually
               produces from them. */
            cJSON_AddNumberToObject(o, "pwm_freq", st.freq_hz);
            cJSON_AddNumberToObject(o, "pwm_freq_actual", st.actual_hz);
            cJSON_AddNumberToObject(o, "pwm_duty", st.duty_pm / 10.0);
            cJSON_AddNumberToObject(o, "pwm_bits", st.bits);
        } else if (st.mode == AUX_MODE_ANALOG && st.mv_valid) {
            cJSON_AddNumberToObject(o, "mv", st.mv);
        } else if (st.mode == AUX_MODE_OUTPUT) {
            cJSON_AddBoolToObject(o, "out", st.out);
        }
        cJSON_AddItemToArray(aux, o);
    }
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

    if (ch >= APP_CFG_DO_COUNT) {
        uint8_t a = (uint8_t)(ch - APP_CFG_DO_COUNT);
        cJSON *tog = cJSON_GetObjectItem(root, "toggle");
        cJSON *val = cJSON_GetObjectItem(root, "value");
        esp_err_t r;
        if (cJSON_IsTrue(tog))      r = aux_io_out_toggle(a);
        else if (cJSON_IsBool(val)) r = aux_io_out_set(a, cJSON_IsTrue(val));
        else {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value or toggle required");
            return ESP_OK;
        }
        cJSON_Delete(root);
        if (r == ESP_ERR_INVALID_STATE) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "this pad is not set to output");
            return ESP_OK;
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, r == ESP_OK ? "{\"status\":\"ok\"}" : "{\"status\":\"error\"}");
        return ESP_OK;
    }

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
        uint32_t n;
        if (json_uint(rj, 0, 255, &n)) r = (uint8_t)n;
        if (json_uint(gj, 0, 255, &n)) g = (uint8_t)n;
        if (json_uint(bj, 0, 255, &n)) b = (uint8_t)n;
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

    uint32_t freq = 440, dur = 200;
    cJSON *fj = cJSON_GetObjectItem(root, "freq");
    cJSON *dj = cJSON_GetObjectItem(root, "duration");
    /* buzzer_beep_once() clamps both; out of uint32_t they are left. */
    json_uint(fj, 0, UINT32_MAX, &freq);
    json_uint(dj, 0, UINT32_MAX, &dur);
    cJSON_Delete(root);

    buzzer_beep_once(freq, dur);
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
    if (!check_auth(req)) return send_denied(req);

    char *buf = NULL;
    const char *script = DEMO_SCRIPT;
    esp_err_t err = rules_nvs_read(&buf);
    if (err == ESP_OK) script = buf;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "stored rules unreadable: %s", esp_err_to_name(err));
        char msg[128];
        snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"error\":\"stored rules unreadable: %s\"}",
                 esp_err_to_name(err));
        return send_json(req, "500 Internal Server Error", msg);
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
    if (!check_auth(req)) return send_denied(req);
    /* Room for the escapes, at worst six bytes for one (\u00e4 for an umlaut,
       as Python's json writes it); the script's own length is checked below. */
    if (req->content_len == 0 || req->content_len > 6 * RULES_MAX_LEN + 100) {
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
        return send_json(req, "400 Bad Request", msg);
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
        return send_json(req, "503 Service Unavailable", "{\"status\":\"busy\",\"detail\":\"engine queue full, nothing stored\"}");

    scripting_reload_status_t now = { 0 };
    for (int waited = 0; waited < RULES_APPLY_TIMEOUT_MS; waited += 10) {
        scripting_reload_status(&now);
        if (now.ticket >= mine) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (now.ticket != mine) {
        /* No verdict for this request yet, or already a later one's: the
           engine stores this script if and when it accepts it. */
        return send_json(req, "202 Accepted", "{\"status\":\"accepted\",\"detail\":\"pending; stored once the engine accepts it\"}");
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
        return send_json(req, "500 Internal Server Error", msg);
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
    if (!check_auth(req)) return send_denied(req);
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
    if (!check_auth(req)) return send_denied(req);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    mb_tcp_stats_t st;
    mb_tcp_server_get_stats(&st);
    cJSON *srv = cJSON_AddObjectToObject(root, "tcp_server");
    cJSON_AddNumberToObject(srv, "accepted",   st.accepted);
    cJSON_AddNumberToObject(srv, "refused",    st.refused);
    cJSON_AddNumberToObject(srv, "evicted",    st.evicted);
    cJSON_AddNumberToObject(srv, "requests",   st.requests);
    cJSON_AddNumberToObject(srv, "exceptions", st.exceptions);
    cJSON_AddNumberToObject(srv, "forwarded",  st.forwarded);
    cJSON_AddNumberToObject(srv, "overloaded", st.overloaded);
    cJSON_AddNumberToObject(srv, "malformed",  st.malformed);

    mbm_status_t ms[APP_CFG_MBM_COUNT];
    uint8_t n = mb_tcp_master_get_status(ms, APP_CFG_MBM_COUNT);
    cJSON *arr = cJSON_AddArrayToObject(root, "tcp_master");
    for (uint8_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name",    ms[i].name);
        cJSON_AddBoolToObject  (o, "enabled", ms[i].enabled);
        if (ms[i].valid) {
            cJSON_AddNumberToObject(o, "value",  ms[i].value);
            cJSON_AddNumberToObject(o, "age_ms", (double)ms[i].age_ms);
        } else {
            cJSON_AddNullToObject(o, "value");
        }
        cJSON_AddNumberToObject(o, "reads",  ms[i].reads);
        cJSON_AddNumberToObject(o, "errors", ms[i].errors);
        if (ms[i].last_error[0]) cJSON_AddStringToObject(o, "last_error", ms[i].last_error);
        cJSON_AddItemToArray(arr, o);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    return ESP_OK;
}

static const httpd_uri_t s_handlers[] = {
    { .uri = "/api/auth/status",       .method = HTTP_GET,  .handler = api_auth_status       },
    { .uri = "/api/auth/begin",        .method = HTTP_POST, .handler = api_auth_begin        },
    { .uri = "/api/auth/token",        .method = HTTP_GET,  .handler = api_auth_token        },
    { .uri = "/api/auth/set-password", .method = HTTP_POST, .handler = api_auth_set_password },
    { .uri = "/api/config",            .method = HTTP_GET,  .handler = api_config_get        },
    { .uri = "/api/config",            .method = HTTP_POST, .handler = api_config_post       },
    { .uri = "/api/io/state",          .method = HTTP_GET,  .handler = api_io_state          },
    { .uri = "/api/modbus/status",     .method = HTTP_GET,  .handler = api_modbus_status     },
    { .uri = "/api/io/output",         .method = HTTP_POST, .handler = api_io_output         },
    { .uri = "/api/io/led",            .method = HTTP_POST, .handler = api_io_led            },
    { .uri = "/api/io/buzzer",         .method = HTTP_POST, .handler = api_io_buzzer         },
#ifdef CONFIG_APP_MATTER_ENABLE
    { .uri = "/api/matter/pairing",       .method = HTTP_GET,  .handler = api_matter_pairing      },
    { .uri = "/api/matter/decommission", .method = HTTP_POST, .handler = api_matter_decommission },
#endif
    { .uri = "/api/eth-only",          .method = HTTP_POST, .handler = api_eth_only          },
    { .uri = "/api/reboot",            .method = HTTP_POST, .handler = api_reboot            },
    { .uri = "/api/factory-reset",     .method = HTTP_POST, .handler = api_factory_reset     },
    { .uri = "/api/rules",             .method = HTTP_GET,  .handler = api_rules_get         },
    { .uri = "/api/rules",             .method = HTTP_POST, .handler = api_rules_post        },
    { .uri = "/api/time",              .method = HTTP_GET,  .handler = api_time_get          },
    { .uri = "/api/time",              .method = HTTP_POST, .handler = api_time_post         },
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
