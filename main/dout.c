#include "dout.h"
#include "app_config.h"
#include "app_mqtt.h"

#include <string.h>
#include <ctype.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG          "dout"
#define NUM_DO       8
#define TCA9554_ADDR 0x20
#define I2C_SDA_PIN  GPIO_NUM_42
#define I2C_SCL_PIN  GPIO_NUM_41

/* TCA9554 register map */
#define REG_OUTPUT   0x01
#define REG_CONFIG   0x03

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool                    s_state[NUM_DO];  /* logical state */

/* ------------------------------------------------------------------ helpers */

/* Accepted on-payloads: true 1 high on   (case-insensitive)
   Accepted off-payloads: false 0 low off */
static bool parse_payload(const char *data, size_t len, bool *out)
{
    char buf[8];
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, data, len);
    buf[len] = '\0';
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)buf[i]);

    if (!strcmp(buf, "true") || !strcmp(buf, "1") ||
        !strcmp(buf, "high") || !strcmp(buf, "on"))  { *out = true;  return true; }
    if (!strcmp(buf, "false") || !strcmp(buf, "0") ||
        !strcmp(buf, "low")   || !strcmp(buf, "off")) { *out = false; return true; }
    return false;
}

/* Build the physical output byte and write it to the TCA9554. */
static esp_err_t write_outputs(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    const app_config_t *cfg = app_config_get();
    uint8_t byte = 0;
    for (uint8_t i = 0; i < NUM_DO; i++) {
        bool physical = s_state[i] ^ cfg->dout[i].invert;
        if (physical) byte |= (1u << i);
    }
    uint8_t buf[2] = {REG_OUTPUT, byte};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 10);
}

static void publish_one(uint8_t n)
{
    char topic[16];
    snprintf(topic, sizeof(topic), "output/%u", n + 1);
    app_mqtt_publish(topic, s_state[n] ? "true" : "false", -1, 0, false);
}

/* ------------------------------------------------------------------ public */

esp_err_t dout_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = -1,          /* auto-select */
        .sda_io_num        = I2C_SDA_PIN,
        .scl_io_num        = I2C_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus init");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TCA9554_ADDR,
        .scl_speed_hz    = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev),
                        TAG, "TCA9554 device add");

    /* Configure all 8 pins as outputs (config register = 0x00) */
    uint8_t cfg_cmd[2] = {REG_CONFIG, 0x00};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, cfg_cmd, sizeof(cfg_cmd), 10),
                        TAG, "TCA9554 config");

    /* Drive all outputs to their initial (off) state */
    ESP_RETURN_ON_ERROR(write_outputs(), TAG, "initial write");

    ESP_LOGI(TAG, "Initialized %d outputs via TCA9554 (I2C addr 0x%02X)", NUM_DO, TCA9554_ADDR);
    return ESP_OK;
}

bool dout_get(uint8_t n)
{
    return (n < NUM_DO) ? s_state[n] : false;
}

esp_err_t dout_set(uint8_t n, bool state)
{
    if (n >= NUM_DO) return ESP_ERR_INVALID_ARG;
    s_state[n] = state;
    esp_err_t ret = write_outputs();
    if (ret == ESP_OK && app_mqtt_is_connected()) publish_one(n);
    return ret;
}

void dout_publish_all(void)
{
    write_outputs();    /* re-apply to hardware — picks up invert changes */
    for (uint8_t i = 0; i < NUM_DO; i++) publish_one(i);
}

void dout_on_mqtt_connected(void)
{
    for (uint8_t i = 1; i <= NUM_DO; i++) {
        char topic[16];
        snprintf(topic, sizeof(topic), "output/%u", i);
        app_mqtt_subscribe(topic, 0);
    }
    app_mqtt_subscribe("output/read", 0);
    dout_publish_all();
}

void dout_on_mqtt_message(const char *topic, size_t tlen,
                           const char *data,  size_t dlen)
{
    /* Match suffix "output/read" */
    static const char READ_SUFFIX[] = "output/read";
    const size_t rs = sizeof(READ_SUFFIX) - 1;
    if (tlen >= rs && memcmp(topic + tlen - rs, READ_SUFFIX, rs) == 0) {
        dout_publish_all();
        return;
    }

    /* Match suffix "output/N" where N is '1'..'8' */
    static const char OUT_SUFFIX[] = "output/";
    const size_t os = sizeof(OUT_SUFFIX) - 1;
    if (tlen >= os + 1) {
        uint8_t ch = (uint8_t)topic[tlen - 1];
        if (ch >= '1' && ch <= '8' &&
            memcmp(topic + tlen - os - 1, OUT_SUFFIX, os) == 0) {
            bool state;
            if (parse_payload(data, dlen, &state)) {
                dout_set((uint8_t)(ch - '1'), state);
            } else {
                ESP_LOGW(TAG, "Unrecognised payload for output/%c", ch);
            }
        }
    }
}
