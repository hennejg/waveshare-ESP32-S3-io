#include "auth.h"
#include "button.h"
#include "led.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include <nvs.h>

#define TAG    "auth"
#define NVS_NS "auth"
#define K_CRED "cred"    /* salt and hash together, one blob, one write */

/* Salt and hash are stored as ONE blob. NVS writes every nvs_set_*() to
   flash at once and nvs_commit() is a no-op, so two separate writes could
   be torn by a failure or a power cut in between -- and a new salt beside
   an old hash matches no password at all, which locks the device for good
   because the factory reset sits behind the login. One blob is one write. */
typedef struct {
    uint8_t salt[16];
    uint8_t hash[32];
} auth_cred_t;

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

static esp_err_t token_init(void);

esp_err_t auth_init(void)
{
    /* The token flow first: it works without a password, too. */
    esp_err_t r = token_init();
    if (r != ESP_OK) return r;

    nvs_handle_t h;
    r = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (r == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (r != ESP_OK) return r;

    /* Only the blob is read. A device written by firmware that stored the
       pair as two keys comes up without a password and is set up again;
       there is no migration, by decision. */
    auth_cred_t c;
    size_t sz = sizeof(c);
    if (nvs_get_blob(h, K_CRED, &c, &sz) == ESP_OK && sz == sizeof(c)) {
        memcpy(s_pw_salt, c.salt, sizeof(s_pw_salt));
        memcpy(s_pw_hash, c.hash, sizeof(s_pw_hash));
        s_pw_set = true;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Password %s", s_pw_set ? "loaded" : "not set");
    return ESP_OK;
}

bool auth_is_password_set(void) { return s_pw_set; }

bool auth_check_password(const char *pw)
{
    if (!s_pw_set) return false;      /* nothing to match against -- see auth.h */
    uint8_t test[32];
    compute_hash(s_pw_salt, pw, test);
    return (memcmp(test, s_pw_hash, 32) == 0);
}

esp_err_t auth_set_password(const char *pw)
{
    /* Into locals first, and into the live state only once the flash has
       taken it: a write that fails must leave memory and flash agreeing on
       the old password, or the device answers to one password until the
       next reboot and to another after it, while the caller was told ok. */
    auth_cred_t c;
    esp_fill_random(c.salt, sizeof(c.salt));
    compute_hash(c.salt, pw, c.hash);

    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_blob(h, K_CRED, &c, sizeof(c));     /* the one write that counts */
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "storing the password failed: %s", esp_err_to_name(r));
        return r;
    }
    memcpy(s_pw_salt, c.salt, sizeof(s_pw_salt));
    memcpy(s_pw_hash, c.hash, sizeof(s_pw_hash));
    s_pw_set = true;
    ESP_LOGI(TAG, "Password updated");
    return ESP_OK;
}

/* ---------------------------------------------------------------- token */

/* The token flow is driven from three tasks: the HTTP handlers (begin,
   status, consume), the button task (auth_on_button_press) and the
   esp_timer task (on_timeout, on_blink). All of its state is touched under
   s_tok_lock only. Without it, a begin that ran on the other core between
   the timeout setting TIMEOUT and the timeout tearing down its timers had
   its own button handler and timers removed by the old callback: the new
   flow sat in WAITING, every further begin got "already pending", and the
   button did nothing until a reboot.

   The two timers are created once, in auth_init(), and only ever stopped
   and restarted -- never deleted, so a callback already on its way cannot
   find its timer gone. A timeout that was on its way when its flow was
   ended or re-armed is told apart by s_deadline: it only acts once the
   deadline of the arming it belongs to has passed. */
static SemaphoreHandle_t   s_tok_lock;
static StaticSemaphore_t   s_tok_lock_buf;
static auth_tok_state_t    s_state   = AUTH_TOK_IDLE;
static char                s_session[9];
static char                s_token[33];
static esp_timer_handle_t  s_timeout_timer = NULL;
static esp_timer_handle_t  s_blink_timer   = NULL;
static int64_t             s_deadline      = 0;      /* 0: no timeout armed */
static bool                s_blink_on      = false;

#define AUTH_WAIT_US   (30ULL  * 1000000ULL)   /* time to reach the button */
#define AUTH_READY_US  (120ULL * 1000000ULL)   /* time to type the password */

/* All with s_tok_lock held. */
static void arm_timeout(uint64_t us)
{
    esp_timer_stop(s_timeout_timer);        /* ESP_ERR_INVALID_STATE if idle */
    s_deadline = esp_timer_get_time() + (int64_t)us;
    esp_timer_start_once(s_timeout_timer, us);
}

static void stop_blink(void)
{
    esp_timer_stop(s_blink_timer);
    s_blink_on = false;
    led_force_set(0, 0, 0);
}

static void stop_timers(void)
{
    esp_timer_stop(s_timeout_timer);
    s_deadline = 0;
    stop_blink();
}

/* Both WAITING and READY expire. A READY token that nobody collects -- the
   browser was closed after the button press -- used to live until the next
   power cycle and answered every new setup attempt with "already pending". */
static void on_timeout(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    /* Fired for an arming that has since been stopped or replaced. */
    if (s_deadline == 0 || esp_timer_get_time() < s_deadline) {
        xSemaphoreGive(s_tok_lock);
        return;
    }
    if (s_state == AUTH_TOK_WAITING || s_state == AUTH_TOK_READY) {
        s_state = AUTH_TOK_TIMEOUT;
        memset(s_token, 0, sizeof(s_token));
        ESP_LOGW(TAG, "Token timed out");
        button_on_short_press(NULL);
    }
    stop_timers();
    xSemaphoreGive(s_tok_lock);
}

static void on_blink(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    /* A tick on its way when the blinking was stopped must not light the
       LED again after stop_blink() put it out. */
    if (s_state == AUTH_TOK_WAITING) {
        s_blink_on = !s_blink_on;
        led_force_set(s_blink_on ? 100 : 0, s_blink_on ? 100 : 0, 0);
    }
    xSemaphoreGive(s_tok_lock);
}

static esp_err_t token_init(void)
{
    s_tok_lock = xSemaphoreCreateMutexStatic(&s_tok_lock_buf);
    esp_timer_create_args_t ta = { .callback = on_timeout, .name = "auth_to" };
    esp_err_t r = esp_timer_create(&ta, &s_timeout_timer);
    if (r != ESP_OK) return r;
    esp_timer_create_args_t ba = { .callback = on_blink, .name = "auth_blink" };
    return esp_timer_create(&ba, &s_blink_timer);
}

esp_err_t auth_token_begin(char session_out[9])
{
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    if (s_state == AUTH_TOK_WAITING || s_state == AUTH_TOK_READY) {
        xSemaphoreGive(s_tok_lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t rnd[20];
    esp_fill_random(rnd, sizeof(rnd));
    to_hex(rnd,     4, s_session);
    to_hex(rnd + 4, 16, s_token);
    memcpy(session_out, s_session, 9);

    s_state = AUTH_TOK_WAITING;
    stop_timers();
    arm_timeout(AUTH_WAIT_US);
    esp_timer_start_periodic(s_blink_timer, 150000ULL);
    button_on_short_press(auth_on_button_press);
    xSemaphoreGive(s_tok_lock);
    ESP_LOGI(TAG, "Token flow started — press BOOT button within 30 s");
    return ESP_OK;
}

void auth_on_button_press(void)
{
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    if (s_state == AUTH_TOK_WAITING) {
        s_state = AUTH_TOK_READY;
        ESP_LOGI(TAG, "Button pressed — token ready");
        stop_blink();
        arm_timeout(AUTH_READY_US);
    }
    xSemaphoreGive(s_tok_lock);
}

auth_tok_state_t auth_token_status(const char *session, char token_out[33])
{
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    auth_tok_state_t st = s_state;
    if (strcmp(session, s_session) != 0) st = AUTH_TOK_IDLE;
    else if (st == AUTH_TOK_READY && token_out) memcpy(token_out, s_token, 33);
    xSemaphoreGive(s_tok_lock);
    return st;
}

bool auth_token_consume(const char *token)
{
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    bool ok = s_state == AUTH_TOK_READY && strcmp(token, s_token) == 0;
    if (ok) {
        s_state = AUTH_TOK_IDLE;
        memset(s_token, 0, sizeof(s_token));
        stop_timers();
    }
    xSemaphoreGive(s_tok_lock);
    return ok;
}

void auth_token_cancel(void)
{
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    s_state = AUTH_TOK_IDLE;
    button_on_short_press(NULL);
    stop_timers();
    xSemaphoreGive(s_tok_lock);
}
