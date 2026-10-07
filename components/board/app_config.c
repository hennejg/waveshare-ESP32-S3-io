#include "app_config.h"
#include <string.h>
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
};

#define NVS_GET_STR(h, key, dst) \
    do { size_t _l = sizeof(dst); nvs_get_str((h), (key), (dst), &_l); } while (0)

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
    sz = sizeof(s_cfg.can);
    nvs_get_blob(h, K_CAN_CFG, &s_cfg.can, &sz);
    sz = sizeof(s_cfg.sntp);
    nvs_get_blob(h, K_SNTP_CFG, &s_cfg.sntp, &sz);
    sz = sizeof(s_cfg.mbm);
    nvs_get_blob(h, K_MBM_CFG, s_cfg.mbm, &sz);

    nvs_close(h);
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
       nvs_set_*() that fails -- a full partition, say -- was swallowed. */
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
