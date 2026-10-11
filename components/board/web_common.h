#pragma once
/* Web API parts both firmwares share -- see web_common.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include "cJSON.h"
#include "app_config.h"

/* Longest password whose HTTP Basic header still fits web_check_auth()'s
   160-byte buffer: "Basic " + base64(":" + password) must stay within 159
   characters, which 113 does at 158 and 114 exceeds at 162. */
#define WEB_PASSWORD_MAX 113

/* Content type for a file name, by its extension. */
const char *web_mime_type(const char *path);
/* The whole request body into buf (content_len + 1 bytes, NUL-terminated);
   its length, or -1. */
int  web_recv_body(httpd_req_t *req, char *buf, size_t cap);
/* A JSON number as an unsigned integer within [lo, hi]. */
bool web_json_uint(const cJSON *v, uint32_t lo, uint32_t hi, uint32_t *out);

/* Fields of POST /api/config: taken only when valid. A field that is
   missing or null leaves the stored value; one of the wrong type or out of
   range is not taken and the first such field is named in *bad (may be
   NULL), for the 400 the caller sends. */
typedef struct { char field[40]; } web_cfg_err_t;
void web_cfg_bad(web_cfg_err_t *bad, const char *where, const char *key);
bool web_cfg_uint(const cJSON *obj, const char *where, const char *key,
                  uint32_t lo, uint32_t hi, uint32_t *out, web_cfg_err_t *bad);
bool web_cfg_str(const cJSON *obj, const char *where, const char *key,
                 char *dst, size_t cap, web_cfg_err_t *bad);

/* The poll table ("mbm") in GET /api/config, and the "mbm" array of a POST
   into cfg->mbm, replacing the table. network_rows: whether this firmware
   reads devices over Modbus TCP. Returns why the request is refused, or
   NULL; a field of the wrong type or range is named in *bad instead. */
void        web_mbm_config_json(cJSON *root, const app_config_t *cfg);
const char *web_mbm_config_parse(const cJSON *mbm, app_config_t *cfg,
                                 bool network_rows, web_cfg_err_t *bad);

/* True if the request may pass: a password is set, it is not cross-site,
   and it carries the password. Every handler outside the setup flow opens
   with: if (!web_check_auth(req)) return web_send_denied(req); */
bool      web_check_auth(httpd_req_t *req);
esp_err_t web_send_denied(httpd_req_t *req);
/* A JSON answer with its status line, e.g. "400 Bad Request". */
esp_err_t web_send_json(httpd_req_t *req, const char *status, const char *body);

/* The password setup flow, open without a password:
   GET /api/auth/status, POST /api/auth/begin, GET /api/auth/token,
   POST /api/auth/set-password. */
esp_err_t web_api_auth_status(httpd_req_t *req);
esp_err_t web_api_auth_begin(httpd_req_t *req);
esp_err_t web_api_auth_token(httpd_req_t *req);
esp_err_t web_api_auth_set_password(httpd_req_t *req);

/* GET /api/time: system clock, RTC and local time. POST /api/time
   {"epoch": <UTC seconds>}: sets the clock by hand and writes the RTC. */
esp_err_t web_api_time_get(httpd_req_t *req);
esp_err_t web_api_time_post(httpd_req_t *req);
/* POST /api/io/buzzer {"freq":440,"duration":200}: one beep. */
esp_err_t web_api_io_buzzer(httpd_req_t *req);

#if APP_CFG_DI_AUX_COUNT
/* The header pads IO1/RXD/TXD in the web API, the same in every firmware:
   - web_aux_config_json(): "aux" in GET /api/config;
   - web_aux_config_parse(): "aux" of a POST /api/config into *cfg, only
     what is sent; the first refusal as text, or NULL;
   - web_aux_state_json(): "di_active" and "aux" in GET /api/io/state;
   - web_aux_output(): POST /api/io/output for pad a (channel 8 + a);
     answers the request. root stays the caller's. */
void        web_aux_config_json(cJSON *root, const app_config_t *cfg);
const char *web_aux_config_parse(const cJSON *root, app_config_t *cfg);
void        web_aux_state_json(cJSON *root);
esp_err_t   web_aux_output(httpd_req_t *req, uint8_t a, const cJSON *root);
#endif
