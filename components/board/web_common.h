#pragma once
/* Web API parts both firmwares share -- see web_common.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include "cJSON.h"

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
