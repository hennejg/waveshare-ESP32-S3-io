#include "auth.h"
#include "button.h"
#include "led.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include <nvs.h>

#define TAG    "auth"
#define NVS_NS "auth"
#define K_SALT "salt"
#define K_HASH "hash"

/* ---------------------------------------------------------------- password */

static bool    s_pw_set  = false;
static uint8_t s_pw_salt[16];
static uint8_t s_pw_hash[32];   /* SHA-256(salt || password) */

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = H[b[i] >> 4];
        out[i*2+1] = H[b[i] & 0xF];
    }
    out[n*2] = '\0';
}

/* SHA-256(salt || password) using the classic mbedTLS MD API.
   This avoids PSA-layer constructors that disrupted the I2C peripheral. */
static void compute_hash(const uint8_t *salt, const char *pw, uint8_t out[32])
{
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 0);   /* 0 = no HMAC */
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, salt, 16);
    mbedtls_md_update(&ctx, (const uint8_t *)pw, strlen(pw));
    mbedtls_md_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

esp_err_t auth_init(void)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (r == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (r != ESP_OK) return r;

    size_t sz = sizeof(s_pw_salt);
    if (nvs_get_blob(h, K_SALT, s_pw_salt, &sz) == ESP_OK) {
        sz = sizeof(s_pw_hash);
        if (nvs_get_blob(h, K_HASH, s_pw_hash, &sz) == ESP_OK) s_pw_set = true;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Password %s", s_pw_set ? "loaded" : "not set");
    return ESP_OK;
}

bool auth_is_password_set(void) { return s_pw_set; }

bool auth_check_password(const char *pw)
{
    if (!s_pw_set) return true;
    uint8_t test[32];
    compute_hash(s_pw_salt, pw, test);
    return (memcmp(test, s_pw_hash, 32) == 0);
}

esp_err_t auth_set_password(const char *pw)
{
    esp_fill_random(s_pw_salt, sizeof(s_pw_salt));
    compute_hash(s_pw_salt, pw, s_pw_hash);

    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    nvs_set_blob(h, K_SALT, s_pw_salt, sizeof(s_pw_salt));
    nvs_set_blob(h, K_HASH, s_pw_hash, sizeof(s_pw_hash));
    nvs_commit(h);
    nvs_close(h);
    s_pw_set = true;
    ESP_LOGI(TAG, "Password updated");
    return ESP_OK;
}

/* ---------------------------------------------------------------- token */

static auth_tok_state_t    s_state   = AUTH_TOK_IDLE;
static char                s_session[9];
static char                s_token[33];
static esp_timer_handle_t  s_timeout_timer = NULL;
static esp_timer_handle_t  s_blink_timer   = NULL;
static bool                s_blink_on      = false;

static void stop_timers(void)
{
    if (s_timeout_timer) {
        esp_timer_stop(s_timeout_timer);
        esp_timer_delete(s_timeout_timer);
        s_timeout_timer = NULL;
    }
    if (s_blink_timer) {
        esp_timer_stop(s_blink_timer);
        esp_timer_delete(s_blink_timer);
        s_blink_timer = NULL;
    }
    led_force_set(0, 0, 0);
}

static void on_timeout(void *arg)
{
    if (s_state == AUTH_TOK_WAITING) {
        s_state = AUTH_TOK_TIMEOUT;
        ESP_LOGW(TAG, "Token request timed out");
        button_on_short_press(NULL);
    }
    stop_timers();
}

static void on_blink(void *arg)
{
    s_blink_on = !s_blink_on;
    led_force_set(s_blink_on ? 100 : 0, s_blink_on ? 100 : 0, 0);
}

esp_err_t auth_token_begin(char session_out[9])
{
    if (s_state == AUTH_TOK_WAITING) return ESP_ERR_INVALID_STATE;

    uint8_t rnd[20];
    esp_fill_random(rnd, sizeof(rnd));
    to_hex(rnd,     4, s_session);
    to_hex(rnd + 4, 16, s_token);
    memcpy(session_out, s_session, 9);

    s_state = AUTH_TOK_WAITING;
    stop_timers();

    esp_timer_create_args_t ta = { .callback = on_timeout, .name = "auth_to" };
    esp_timer_create(&ta, &s_timeout_timer);
    esp_timer_start_once(s_timeout_timer, 30ULL * 1000000ULL);

    esp_timer_create_args_t ba = { .callback = on_blink, .name = "auth_blink" };
    esp_timer_create(&ba, &s_blink_timer);
    esp_timer_start_periodic(s_blink_timer, 150000ULL);

    button_on_short_press(auth_on_button_press);
    ESP_LOGI(TAG, "Token flow started — press BOOT button within 30 s");
    return ESP_OK;
}

void auth_on_button_press(void)
{
    if (s_state == AUTH_TOK_WAITING) {
        s_state = AUTH_TOK_READY;
        ESP_LOGI(TAG, "Button pressed — token ready");
        stop_timers();
    }
}

auth_tok_state_t auth_token_status(const char *session, char token_out[33])
{
    if (strcmp(session, s_session) != 0) return AUTH_TOK_IDLE;
    if (s_state == AUTH_TOK_READY && token_out) memcpy(token_out, s_token, 33);
    return s_state;
}

bool auth_token_consume(const char *token)
{
    if (s_state != AUTH_TOK_READY) return false;
    if (strcmp(token, s_token) != 0) return false;
    s_state = AUTH_TOK_IDLE;
    memset(s_token, 0, sizeof(s_token));
    stop_timers();
    return true;
}

void auth_token_cancel(void)
{
    s_state = AUTH_TOK_IDLE;
    button_on_short_press(NULL);
    stop_timers();
}
