/* The parts of the web API both firmwares share: reading a request body,
   the access check, the uniform JSON answers and the password setup flow
   (the /api/auth/ endpoints). They were two copies, in apps/full and
   apps/matter, and a fix to one of them had to be remembered for the other. */
#include "web_common.h"
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include "mbedtls/base64.h"

#define TAG "web"

/* Total time a request body may take to arrive. */
#define BODY_RECV_TIMEOUT_MS 5000

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
