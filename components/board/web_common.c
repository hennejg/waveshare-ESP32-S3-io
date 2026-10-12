/* The parts of the web API both firmwares share: reading a request body,
   the access check, the uniform JSON answers and the password setup flow
   (the /api/auth/ endpoints). They were two copies, in apps/full and
   apps/matter, and a fix to one of them had to be remembered for the other. */
#include "web_common.h"
#include "app_config.h"
#include "app_rtc.h"
#include "auth.h"
#include "aux_io.h"
#include "buzzer.h"
#include "di.h"
#ifdef CONFIG_APP_SCRIPTING_ENABLE
void scripting_on_time_sync(void);
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <esp_log.h>
#include <esp_timer.h>
#include "mbedtls/base64.h"

#define TAG "web"

/* Total time a request body may take to arrive. */
#define BODY_RECV_TIMEOUT_MS 5000

/* POST /api/time carries one number. */
#define TIME_BODY_MAX 128

/* ------------------------------------------------------------------ helpers */

const char *web_mime_type(const char *path)
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

/* httpd_req_recv() returns what a single socket read yielded. The loop in
   httpd_recv_with_opt() only repeats for HTTPD_RECV_OPT_BLOCKING, and
   httpd_recv() passes HTTPD_RECV_OPT_NONE, so one call can return far less
   than content_len. Measured on an ESP32-S3-POE-ETH-8DI-8DO with a 2895-byte
   rule script: a single call returned 1440 bytes when the body was sent in one
   go, 724 over four segments and 145 over twenty -- all three then failed to
   parse and answered 400 Invalid JSON.

   buf must hold content_len + 1 bytes. Returns the length read, or -1. */
int web_recv_body(httpd_req_t *req, char *buf, size_t cap)
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

/* A JSON number as an unsigned integer within [lo, hi]; false if it is not
   a number or lies outside. Checked as a double, before any conversion: a
   double outside the target type's range converts undefined in C, and in
   practice it wrapped -- base_id 4294967295 became 0xFFFFFFFF, "+ 5" made
   it 4, the 11-bit check passed and 0xFFFF was stored. Fractions are cut
   off, as the casts did before. NaN fails both comparisons. */
bool web_json_uint(const cJSON *v, uint32_t lo, uint32_t hi, uint32_t *out)
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

/* Returns true if the request carries a valid password (or no password is set). */
/* A device without a password is one that has not been set up yet, and
   until that happens nothing but the setup flow itself may be used: the
   outputs, the rules (which run as code on the device) and the factory
   reset must not be one unauthenticated request away just because nobody
   has pressed the button yet. The physical confirmation guards the
   password; this guards everything else until there is one. */
bool web_check_auth(httpd_req_t *req)
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
esp_err_t web_send_json(httpd_req_t *req, const char *status, const char *body)
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
esp_err_t web_send_denied(httpd_req_t *req)
{
    if (!auth_is_password_set())
        return web_send_json(req, "403 Forbidden", "{\"error\":\"setup_required\"}");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Device\"");
    return web_send_json(req, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
}

/* ------------------------------------------------------------------ auth endpoints */

/* GET /api/auth/status — no auth required */
esp_err_t web_api_auth_status(httpd_req_t *req)
{
    char json[32];
    snprintf(json, sizeof(json), "{\"password_set\":%s}",
             auth_is_password_set() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* POST /api/auth/begin — start token flow; no auth required */
esp_err_t web_api_auth_begin(httpd_req_t *req)
{
    char session[9];
    esp_err_t r = auth_token_begin(session);
    if (r == ESP_ERR_INVALID_STATE) {
        return web_send_json(req, "409 Conflict", "{\"error\":\"already_pending\"}");
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
esp_err_t web_api_auth_token(httpd_req_t *req)
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
esp_err_t web_api_auth_set_password(httpd_req_t *req)
{
    if (req->content_len > 256) {
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
       must stay within that buffer -- see WEB_PASSWORD_MAX. */
    if (strlen(pw_j->valuestring) > WEB_PASSWORD_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "password too long (max 113 bytes as UTF-8)");
        return ESP_OK;
    }
    if (strlen(pw_j->valuestring) < 8) {
        cJSON_Delete(root);
        return web_send_json(req, "400 Bad Request", "{\"error\":\"password_too_short\"}");
    }

    if (!auth_token_consume(token_j->valuestring)) {
        cJSON_Delete(root);
        return web_send_json(req, "403 Forbidden", "{\"error\":\"invalid_token\"}");
    }

    esp_err_t set_ret = auth_set_password(pw_j->valuestring);
    cJSON_Delete(root);
    if (set_ret != ESP_OK) {
        /* The token is spent either way; say what happened rather than
           reporting a protection the flash does not have. */
        return web_send_json(req, "500 Internal Server Error", "{\"error\":\"password could not be stored\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ time */

static void iso8601_utc(time_t t, char *buf, size_t len)
{
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/* GET /api/time — report the system clock and the battery-backed RTC.
 * All times are UTC. "rtc_valid" is false when the RTC reports an oscillator
 * stop (i.e. it lost power and its time is meaningless). */
esp_err_t web_api_time_get(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);
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
esp_err_t web_api_time_post(httpd_req_t *req)
{
    if (!web_check_auth(req)) return web_send_denied(req);
    if (req->content_len > TIME_BODY_MAX) {
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
    /* As after an SNTP sync: the clock is valid now, and cron is armed
       again from it. Without this, a board without SNTP and with no valid
       RTC -- where setting the time by hand is the only way -- never armed
       a cron rule until the next reboot. */
#ifdef CONFIG_APP_SCRIPTING_ENABLE
    scripting_on_time_sync();
#endif

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


/* ------------------------------------------------------------------ buzzer */

/* POST /api/io/buzzer — {"freq":440,"duration":200} */
esp_err_t web_api_io_buzzer(httpd_req_t *req)
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

    uint32_t freq = 440, dur = 200;
    cJSON *fj = cJSON_GetObjectItem(root, "freq");
    cJSON *dj = cJSON_GetObjectItem(root, "duration");
    /* buzzer_beep_once() clamps both; out of uint32_t they are left. */
    web_json_uint(fj, 0, UINT32_MAX, &freq);
    web_json_uint(dj, 0, UINT32_MAX, &dur);
    cJSON_Delete(root);

    buzzer_beep_once(freq, dur);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}


/* ------------------------------------------------------------------ header pads */

#if APP_CFG_DI_AUX_COUNT
void web_aux_config_json(cJSON *root, const app_config_t *cfg)
{
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
}

const char *web_aux_config_parse(const cJSON *root, app_config_t *cfg)
{
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
                    cfg->aux_mode[a] = (uint8_t)m;
            }
            if (!aux_err && (v = cJSON_GetObjectItem(o, "pwm_freq"))) {
                if (!cJSON_IsNumber(v) || !aux_pwm_freq_ok(v->valuedouble))
                    aux_err = "pwm_freq must be a whole number of Hz from 5 to 1000000";
                else
                    cfg->aux_pwm[a].freq_hz = (uint32_t)v->valuedouble;
            }
            if (!aux_err && (v = cJSON_GetObjectItem(o, "pwm_duty"))) {
                if (!cJSON_IsNumber(v) || !aux_pwm_duty_from_pct(v->valuedouble, &cfg->aux_pwm[a].duty_pm))
                    aux_err = "pwm_duty must be a percentage from 0 to 100";
            }
            if (!aux_err && (v = cJSON_GetObjectItem(o, "out_name"))) {
                if (!cJSON_IsString(v) || strlen(v->valuestring) > APP_CFG_IO_NAME_MAX ||
                    !app_config_utf8_ok(v->valuestring))
                    aux_err = "out_name must be text of at most 20 bytes (UTF-8)";
                else
                    strlcpy(cfg->aux_out[a].name, v->valuestring, sizeof(cfg->aux_out[a].name));
            }
            if (!aux_err && (v = cJSON_GetObjectItem(o, "out_invert"))) {
                if (!cJSON_IsBool(v)) aux_err = "out_invert must be true or false";
                else cfg->aux_out[a].invert = cJSON_IsTrue(v);
            }
        }
    }
    return aux_err;
}

void web_aux_state_json(cJSON *root)
{
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
}

/* POST /api/io/output for channels 8..10: pad a, while it is an output. */
esp_err_t web_aux_output(httpd_req_t *req, uint8_t a, const cJSON *root)
{
    cJSON *tog = cJSON_GetObjectItem(root, "toggle");
    cJSON *val = cJSON_GetObjectItem(root, "value");
    esp_err_t r;
    if (cJSON_IsTrue(tog))      r = aux_io_out_toggle(a);
    else if (cJSON_IsBool(val)) r = aux_io_out_set(a, cJSON_IsTrue(val));
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value or toggle required");
        return ESP_OK;
    }
    if (r == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "this pad is not set to output");
        return ESP_OK;
    }
    if (r != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "output write failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
#endif

/* --------------------------------------------------------- configuration fields */

/* api_config_post() takes a field that is sent only when it is valid, and
   refuses the whole save otherwise. A value out of range or of the wrong
   type used to be skipped while the page said "Saved": register 70000 made
   the row poll register 0, unit 300 polled unit 1. A field that is missing,
   or null -- what the page sends for an empty number box -- leaves the
   stored value. The first bad field is kept in *bad for the 400. */
void web_cfg_bad(web_cfg_err_t *bad, const char *where, const char *key)
{
    if (bad && !bad->field[0]) snprintf(bad->field, sizeof(bad->field), "%s%s", where, key);
}

bool web_cfg_uint(const cJSON *obj, const char *where, const char *key,
                  uint32_t lo, uint32_t hi, uint32_t *out, web_cfg_err_t *bad)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v || cJSON_IsNull(v)) return false;
    if (web_json_uint(v, lo, hi, out)) return true;
    web_cfg_bad(bad, where, key);
    return false;
}

/* A string is taken whole or not at all. strlcpy() cut an over-long one
   short -- a different name, and when the cut fell inside a character, an
   MQTT topic that is not UTF-8, over which the broker closes the
   connection, again on every reconnect. The page's maxlength counts
   characters; the limit here is bytes. */
bool web_cfg_str(const cJSON *obj, const char *where, const char *key,
                 char *dst, size_t cap, web_cfg_err_t *bad)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v || cJSON_IsNull(v)) return false;
    size_t len = cJSON_IsString(v) ? strlen(v->valuestring) : 0;
    if (!cJSON_IsString(v) || len >= cap || !app_config_utf8_ok(v->valuestring)) {
        web_cfg_bad(bad, where, key);
        return false;
    }
    memcpy(dst, v->valuestring, len + 1);
    return true;
}

/* ------------------------------------------------------------------ poll table */

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

/* Indexed by MBM_MATTER_*. */
static const char *const k_mbm_matter[MBM_MATTER_MAX + 1] = {
    "none", "temperature", "humidity", "pressure", "flow", "power", "energy",
};

void web_can_config_json(cJSON *root, const app_config_t *cfg)
{
    cJSON *can = cJSON_AddObjectToObject(root, "can");
    cJSON_AddNumberToObject(can, "mode",           cfg->can.mode);
    cJSON_AddNumberToObject(can, "n2k_addr",       cfg->can.n2k_addr);
    cJSON_AddNumberToObject(can, "base_id",        cfg->can.base_id);
    cJSON_AddNumberToObject(can, "bitrate",        cfg->can.bitrate);
    cJSON_AddNumberToObject(can, "tx_interval_ms", cfg->can.tx_interval_ms);
}

void web_can_config_parse(const cJSON *can_j, app_config_t *cfg, web_cfg_err_t *bad)
{
    if (!cJSON_IsObject(can_j)) return;
    uint32_t n;
    if (web_cfg_uint(can_j, "can.", "mode", 0, 2, &n, bad))
        cfg->can.mode = (uint8_t)n;
    if (web_cfg_uint(can_j, "can.", "n2k_addr", 1, 251, &n, bad))
        cfg->can.n2k_addr = (uint8_t)n;
    /* base_id .. base_id + 5 are used for TX and RX matching, so the
       whole block has to fit in the 11-bit standard range. */
    if (web_cfg_uint(can_j, "can.", "base_id", 0, APP_CFG_CAN_BASE_ID_MAX, &n, bad))
        cfg->can.base_id = (uint16_t)n;
    /* Outside this range twai_new_node_onchip() refuses the value and
       can_server_init() fails. */
    if (web_cfg_uint(can_j, "can.", "bitrate", 10000, 1000000, &n, bad))
        cfg->can.bitrate = n;
    if (web_cfg_uint(can_j, "can.", "tx_interval_ms", 0, UINT16_MAX, &n, bad))
        cfg->can.tx_interval_ms = (uint16_t)n;
}

void web_mbm_config_json(cJSON *root, const app_config_t *cfg)
{
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
        cJSON_AddStringToObject(o, "matter",      k_mbm_matter[e->matter <= MBM_MATTER_MAX ? e->matter : 0]);
        cJSON_AddItemToArray(mbm, o);
    }
}

const char *web_mbm_config_parse(const cJSON *mbm_j, app_config_t *cfg,
                                 bool network_rows, web_cfg_err_t *bad)
{
    /* The array replaces the table wholesale rather than being merged into
       it: a partial update would leave an entry the user deleted in the
       UI still being polled. Entries past the end are cleared. */
    memset(cfg->mbm, 0, sizeof(cfg->mbm));
    int n = cJSON_GetArraySize(mbm_j);
    if (n > APP_CFG_MBM_COUNT) n = APP_CFG_MBM_COUNT;

    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(mbm_j, i);
        if (!cJSON_IsObject(o)) continue;
        mbm_poll_t *e = &cfg->mbm[i];
        cJSON *v;
        uint32_t u;
        char w[12];
        snprintf(w, sizeof(w), "mbm.%d.", i + 1);

        e->port = MBM_PORT_DEFAULT; e->unit_id = MBM_UNIT_ID_DEFAULT;
        e->fc = MBM_FC_DEFAULT; e->scale = 1.0f; e->interval_ms = MBM_INTERVAL_DEFAULT_MS;

        /* A script writing "enable": 1 rather than true would otherwise
           store a silently disabled entry and get 200 back. */
        if ((v = cJSON_GetObjectItem(o, "enable"))) {
            if (cJSON_IsBool(v))        e->enable = cJSON_IsTrue(v) ? 1 : 0;
            else if (cJSON_IsNumber(v)) e->enable = v->valuedouble != 0 ? 1 : 0;
        }
        /* Only a row that is polled is held to its ranges: what a
           disabled row's hidden boxes hold must not block the save. */
        web_cfg_err_t *rb = e->enable ? bad : NULL;
        web_cfg_str(o, w, "name", e->name, sizeof(e->name), rb);
        web_cfg_str(o, w, "host", e->host, sizeof(e->host), rb);
        if (web_cfg_uint(o, w, "port", 1, 65535, &u, rb))
            e->port = (uint16_t)u;
        if (web_cfg_uint(o, w, "unit_id", 1, 247, &u, rb))
            e->unit_id = (uint8_t)u;
        if ((v = cJSON_GetObjectItem(o, "fc")) && cJSON_IsString(v))
            e->fc = strcmp(v->valuestring, "input") == 0 ? 4 : 3;
        if (web_cfg_uint(o, w, "reg", 0, 65535, &u, rb))
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
        if (web_cfg_uint(o, w, "interval_ms", MBM_INTERVAL_MIN_MS, MBM_INTERVAL_MAX_MS, &u, rb))
            e->interval_ms = u;
        /* Unknown text is a bad field rather than "none": a row meant for a
           sensor endpoint would otherwise quietly get none. */
        if ((v = cJSON_GetObjectItem(o, "matter")) && !cJSON_IsNull(v)) {
            int k = -1;
            if (cJSON_IsString(v))
                for (int j = 0; j <= MBM_MATTER_MAX; j++)
                    if (!strcmp(v->valuestring, k_mbm_matter[j])) { k = j; break; }
            if (k >= 0) e->matter = (uint8_t)k;
            else        web_cfg_bad(rb, w, "matter");
        }

        /* A name that is empty or carries MQTT syntax would make a topic
           nothing can subscribe to, so the entry is refused rather than
           quietly polled into nowhere. */
        if (e->enable) {
            if (!e->name[0])                  return "a Modbus master entry needs a name";
            if (strpbrk(e->name, "#+/"))      return "a Modbus master name must not contain '#', '+' or '/'";
            if (e->transport != MBM_TRANSPORT_RTU && !network_rows)
                                              return "this firmware reads devices on RS-485 only";
            if (!e->host[0] && e->transport != MBM_TRANSPORT_RTU)
                                              return "a Modbus master entry needs a host";
            if (e->scale == 0.0f)             return "a Modbus master scale must be a non-zero finite number";
            /* Two entries of the same name would publish to one topic
               and feed one rule key, last poll winning at random.
               app_config_io_names_ok() does the same for the I/O names. */
            for (int j = 0; j < i; j++)
                if (cfg->mbm[j].enable && !strcmp(cfg->mbm[j].name, e->name))
                    return "two Modbus master entries have the same name";
        }
    }
    return NULL;
}
