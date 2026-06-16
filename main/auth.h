#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Load saved password state from NVS. Call once at boot. */
esp_err_t auth_init(void);

/* True if a password has been configured. */
bool auth_is_password_set(void);

/* Verify password. Returns true if correct OR if no password is set. */
bool auth_check_password(const char *password);

/* Hash and persist a new password. Replaces any existing one. */
esp_err_t auth_set_password(const char *password);

/* ---- Physical-button token flow (non-blocking, polled via HTTP) ---- *
 *
 * 1. Call auth_token_begin() → returns 8-char session id, starts LED blink
 *    and 30 s timer.  Registers button short-press callback automatically.
 * 2. Caller polls auth_token_status() until AUTH_TOK_READY or AUTH_TOK_TIMEOUT.
 *    When READY, token_out contains the 32-char hex token.
 * 3. Call auth_token_consume() with the token to validate + clear it, then
 *    use auth_set_password() to persist the new password.
 */
typedef enum { AUTH_TOK_IDLE, AUTH_TOK_WAITING, AUTH_TOK_READY, AUTH_TOK_TIMEOUT } auth_tok_state_t;

esp_err_t        auth_token_begin(char session_out[9]);
auth_tok_state_t auth_token_status(const char *session, char token_out[33]);
bool             auth_token_consume(const char *token);
void             auth_token_cancel(void);

/* Called by button task — do not call directly. */
void auth_on_button_press(void);
