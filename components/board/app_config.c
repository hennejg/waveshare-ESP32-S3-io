#include "app_config.h"
#include "aux_io.h"
#include <string.h>
#include <math.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "esp_log.h"

#define TAG "app_config"

/* Not an NVS limit -- the configuration is thirteen blobs, the largest of
   them the Modbus master table at 736 bytes. What this actually guards is the
   stack: api_config_post puts a whole app_config_t on the HTTP task's, which
   is 6 kB. Today the struct is about 1.7 kB. */
_Static_assert(sizeof(app_config_t) < 2600,
               "app_config_t too large for the HTTP task stack");

#define NVS_NS "app_config"

/* NVS key names must be <= 15 chars. */
#define K_DEVICE_NAME   "device_name"
#define K_MQTT_URL      "mqtt_url"
#define K_MQTT_USER     "mqtt_user"
#define K_MQTT_PASS     "mqtt_password"
#define K_MQTT_TOPIC    "mqtt_topic"
#define K_DI_CFG        "di_cfg"
#define K_DOUT_CFG      "dout_cfg"
#define K_MODBUS_CFG    "mb_cfg"
#define K_LED_MODE      "led_mode"
#define K_AUX_MODE      "aux_mode"
#define K_AUX_PWM       "aux_pwm"
#define K_AUX_OUT       "aux_out"
#define K_CAN_CFG       "can_cfg"
#define K_SNTP_CFG      "sntp_cfg"
#define K_MBM_CFG       "mbm_cfg"
#define K_TZ            "tz"

static app_config_t s_cfg = {
    .device_name       = "Waveshare-ESP32",
    .mqtt_url          = "",
    .mqtt_user         = "",
    .mqtt_password     = "",
    .mqtt_topic_prefix = "",
    /* di/dout names and invert default to zero */
    .led_mode = LED_MODE_STATUS,   /* status feedback on by default */
    /* Modbus TCP has no authentication of any kind: anything that can reach
       the board can read its inputs and switch its outputs, and through the
       gateway everything on the RS-485 segment as well. It is therefore off
       until someone turns it on, and the RS-485 role stays what it has always
       been. */
    .modbus = { .enable = 0, .address = 1, .baudrate = 9600,
                .rs485_role = MB_ROLE_SLAVE, .tcp_server = 0,
                .tcp_uid = MB_TCP_UID_DEFAULT,
                .rs485_tout_ms = MB_RS485_TOUT_DEFAULT_MS },
    .can    = { .mode = 0, .n2k_addr = 0x50, .base_id = 0x100,
                .bitrate = 250000, .tx_interval_ms = 1000 },
    .sntp   = { .enable = 1, .server = "pool.ntp.org" },
    /* No entries by default: an empty table starts no task and opens no
       connection. */
    .tz     = "",   /* empty = UTC */
    .aux_pwm = { { .freq_hz = 1000 }, { .freq_hz = 1000 }, { .freq_hz = 1000 } },
};

#define NVS_GET_STR(h, key, dst) \
    do { size_t _l = sizeof(dst); nvs_get_str((h), (key), (dst), &_l); } while (0)

/* Pad modes and PWM settings outside their range -- an older firmware's
   blob, a hand-made request -- fall back to the safe value. */
static void aux_normalize(app_config_t *c)
{
    for (int a = 0; a < 3; a++) {
        aux_mode_t m = (aux_mode_t)c->aux_mode[a];
        if (!aux_mode_allowed((uint8_t)a, m)) c->aux_mode[a] = AUX_MODE_STANDARD;
        if (c->aux_pwm[a].freq_hz < AUX_PWM_FREQ_MIN || c->aux_pwm[a].freq_hz > AUX_PWM_FREQ_MAX)
            c->aux_pwm[a].freq_hz = AUX_PWM_FREQ_DEFAULT;
        if (c->aux_pwm[a].duty_pm > AUX_PWM_DUTY_MAX) c->aux_pwm[a].duty_pm = 0;
        c->aux_out[a].name[sizeof(c->aux_out[a].name) - 1] = '\0';
    }
}

/* A blob written before a field existed reads it as zero, and a blob written
   by a hand-edited request can hold a value outside its range. Settled here,
   once, after loading: every reader then takes the structure as it is. */
static void modbus_normalize(app_config_t *c)
{
    if (c->modbus.tcp_uid < MB_UID_MIN || c->modbus.tcp_uid > MB_UID_MAX)
        c->modbus.tcp_uid = MB_TCP_UID_DEFAULT;
    if (c->modbus.rs485_tout_ms < MB_RS485_TOUT_MIN_MS ||
        c->modbus.rs485_tout_ms > MB_RS485_TOUT_MAX_MS)
        c->modbus.rs485_tout_ms = MB_RS485_TOUT_DEFAULT_MS;
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
        mbm_poll_t *e = &c->mbm[i];
        if (!e->port)                                      e->port        = MBM_PORT_DEFAULT;
        if (e->unit_id < MB_UID_MIN || e->unit_id > MB_UID_MAX) e->unit_id = MBM_UNIT_ID_DEFAULT;
        if (e->fc != 3 && e->fc != 4)                      e->fc          = MBM_FC_DEFAULT;
        if (!e->interval_ms)                               e->interval_ms = MBM_INTERVAL_DEFAULT_MS;
        /* The one value the web API refuses on the way in: an untouched row
           handed out with scale 0 came straight back as a 400 the moment a
           client enabled it without changing anything else. */
        if (!isfinite(e->scale) || e->scale == 0.0f)       e->scale       = 1.0f;
    }
}

esp_err_t app_config_init(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;

    NVS_GET_STR(h, K_DEVICE_NAME, s_cfg.device_name);
    NVS_GET_STR(h, K_MQTT_URL,    s_cfg.mqtt_url);
    NVS_GET_STR(h, K_MQTT_USER,   s_cfg.mqtt_user);
    NVS_GET_STR(h, K_MQTT_PASS,   s_cfg.mqtt_password);
    NVS_GET_STR(h, K_MQTT_TOPIC,  s_cfg.mqtt_topic_prefix);
    NVS_GET_STR(h, K_TZ,          s_cfg.tz);

    /* DI config blob — silently keep defaults if not found or size changed
       (the latter happens when di_config_t grows beyond its reserved bytes). */
    size_t sz = sizeof(s_cfg.di);
    nvs_get_blob(h, K_DI_CFG, s_cfg.di, &sz);
    sz = sizeof(s_cfg.dout);
    nvs_get_blob(h, K_DOUT_CFG, s_cfg.dout, &sz);
    sz = sizeof(s_cfg.modbus);
    nvs_get_blob(h, K_MODBUS_CFG, &s_cfg.modbus, &sz);
    uint8_t led_mode_val = 0;
    nvs_get_u8(h, K_LED_MODE, &led_mode_val);
    s_cfg.led_mode = led_mode_val;
    sz = sizeof(s_cfg.aux_mode);
    if (nvs_get_blob(h, K_AUX_MODE, s_cfg.aux_mode, &sz) != ESP_OK) {
        /* No pad modes stored. A firmware built with APP_AUX_DI_ENABLE before
           the pads became switchable had all three as inputs, always, and
           wrote an 11-entry input table; such a board keeps its inputs. */
        size_t di_len = 0;
        if (nvs_get_blob(h, K_DI_CFG, NULL, &di_len) == ESP_OK &&
            di_len == 11 * sizeof(di_config_t))
            for (int a = 0; a < 3; a++) s_cfg.aux_mode[a] = AUX_MODE_INPUT;
    }
    sz = sizeof(s_cfg.aux_pwm);
    nvs_get_blob(h, K_AUX_PWM, s_cfg.aux_pwm, &sz);
    sz = sizeof(s_cfg.aux_out);
    nvs_get_blob(h, K_AUX_OUT, s_cfg.aux_out, &sz);
    sz = sizeof(s_cfg.can);
    nvs_get_blob(h, K_CAN_CFG, &s_cfg.can, &sz);
    sz = sizeof(s_cfg.sntp);
    nvs_get_blob(h, K_SNTP_CFG, &s_cfg.sntp, &sz);
    sz = sizeof(s_cfg.mbm);
    nvs_get_blob(h, K_MBM_CFG, s_cfg.mbm, &sz);

    nvs_close(h);
    modbus_normalize(&s_cfg);
    aux_normalize(&s_cfg);
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return &s_cfg;
}

esp_err_t app_config_update(const app_config_t *cfg)
{
    /* Flash first, live state second: a write that fails leaves the device
       running the configuration it still has in flash, and the caller is
       told so, instead of running the new one until the next reboot while
       having answered ok. Every step is chained, because a single
       nvs_set_*() that fails -- a full partition, say -- was swallowed.

       Known limit: the keys below are written one after another and NVS
       has no transaction (nvs_commit() is a no-op), so a failure or a
       power cut part-way leaves the flash with some new and some old
       values for the next boot. Nothing locks the device that way -- the
       network settings and the web UI stay reachable, and the next save
       writes everything again -- which is why the per-key layout, and
       with it the graceful growth of each struct, is kept. The one store
       where a torn write would lock the user out, the password, is a
       single blob for that reason (auth.c). */
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

#define PUT(call) do { if (ret == ESP_OK) ret = (call); } while (0)
    PUT(nvs_set_str(h, K_DEVICE_NAME, cfg->device_name));
    PUT(nvs_set_str(h, K_MQTT_URL,    cfg->mqtt_url));
    PUT(nvs_set_str(h, K_MQTT_USER,   cfg->mqtt_user));
    PUT(nvs_set_str(h, K_MQTT_PASS,   cfg->mqtt_password));
    PUT(nvs_set_str(h, K_MQTT_TOPIC,  cfg->mqtt_topic_prefix));
    PUT(nvs_set_str(h, K_TZ,          cfg->tz));
    PUT(nvs_set_blob(h, K_DI_CFG,     cfg->di,   sizeof(cfg->di)));
    PUT(nvs_set_blob(h, K_DOUT_CFG,   cfg->dout, sizeof(cfg->dout)));
    PUT(nvs_set_blob(h, K_MODBUS_CFG, &cfg->modbus, sizeof(cfg->modbus)));
    PUT(nvs_set_u8(h, K_LED_MODE,     cfg->led_mode));
    PUT(nvs_set_blob(h, K_AUX_MODE,   cfg->aux_mode, sizeof(cfg->aux_mode)));
    PUT(nvs_set_blob(h, K_AUX_PWM,    cfg->aux_pwm,  sizeof(cfg->aux_pwm)));
    PUT(nvs_set_blob(h, K_AUX_OUT,    cfg->aux_out,  sizeof(cfg->aux_out)));
    PUT(nvs_set_blob(h, K_CAN_CFG,    &cfg->can, sizeof(cfg->can)));
    PUT(nvs_set_blob(h, K_SNTP_CFG,   &cfg->sntp, sizeof(cfg->sntp)));
    PUT(nvs_set_blob(h, K_MBM_CFG,    cfg->mbm, sizeof(cfg->mbm)));
    PUT(nvs_commit(h));
#undef PUT
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "storing the configuration failed: %s", esp_err_to_name(ret));
        return ret;
    }

    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    return ESP_OK;
}
