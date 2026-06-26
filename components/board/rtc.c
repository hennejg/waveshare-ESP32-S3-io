#include "app_rtc.h"
#include "i2c_bus.h"

#include "esp_check.h"
#include "esp_log.h"
#include <string.h>

#define TAG             "rtc"
#define PCF85063_ADDR   0x51
#define REG_CTRL_1      0x00
#define REG_SECONDS     0x04   /* sec, min, hour, day, weekday, month, year (7 bytes) */
#define CTRL_1_CAP_SEL  0x01   /* 12.5 pF crystal load (matches the board) */
#define CTRL_1_STOP     0x20   /* 1 = clock stopped */
#define SECONDS_OS      0x80   /* seconds reg bit 7: oscillator stopped → time lost */
#define I2C_TIMEOUT_MS  50
#define YEAR_BASE       1970   /* RTC year register 0-99 ↔ 1970-2069 */

static i2c_master_dev_handle_t s_dev = NULL;

static uint8_t dec2bcd(int v)     { return (uint8_t)((v / 10 * 16) + (v % 10)); }
static int     bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static esp_err_t reg_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

esp_err_t rtc_dev_init(void)
{
    if (s_dev) return ESP_OK;
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF85063_ADDR,
        .scl_speed_hz    = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_handle(), &dev_cfg, &s_dev),
                        TAG, "add device");

    /* Set crystal load and ensure the clock runs. Does NOT clear the oscillator-stop
     * flag, so a previously-set time survives across reboots. */
    uint8_t ctrl1 = CTRL_1_CAP_SEL;
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL_1, &ctrl1, 1), TAG, "ctrl1 write");

    uint8_t back = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_CTRL_1, &back, 1), TAG, "ctrl1 read");
    if (back & CTRL_1_STOP) {
        ESP_LOGW(TAG, "PCF85063 clock stopped (ctrl1=0x%02X)", back);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PCF85063 RTC ready (addr 0x%02X)", PCF85063_ADDR);
    return ESP_OK;
}

esp_err_t rtc_dev_read(struct tm *out, bool *valid)
{
    if (!s_dev || !out) return ESP_ERR_INVALID_STATE;
    uint8_t b[7];
    ESP_RETURN_ON_ERROR(reg_read(REG_SECONDS, b, sizeof(b)), TAG, "read time");

    if (valid) *valid = !(b[0] & SECONDS_OS);   /* OS flag set → time not trustworthy */

    memset(out, 0, sizeof(*out));
    out->tm_sec   = bcd2dec(b[0] & 0x7F);
    out->tm_min   = bcd2dec(b[1] & 0x7F);
    out->tm_hour  = bcd2dec(b[2] & 0x3F);
    out->tm_mday  = bcd2dec(b[3] & 0x3F);
    out->tm_wday  = bcd2dec(b[4] & 0x07);        /* 0 = Sunday */
    out->tm_mon   = bcd2dec(b[5] & 0x1F) - 1;    /* 0-11 */
    out->tm_year  = bcd2dec(b[6]) + YEAR_BASE - 1900;
    out->tm_isdst = 0;
    return ESP_OK;
}

esp_err_t rtc_dev_set(const struct tm *t)
{
    if (!s_dev || !t) return ESP_ERR_INVALID_STATE;
    int year = t->tm_year + 1900;
    if (year < YEAR_BASE || year > YEAR_BASE + 99) return ESP_ERR_INVALID_ARG;

    uint8_t b[7] = {
        (uint8_t)(dec2bcd(t->tm_sec) & 0x7F),   /* bit7=0 also clears the OS flag */
        dec2bcd(t->tm_min),
        dec2bcd(t->tm_hour),
        dec2bcd(t->tm_mday),
        dec2bcd(t->tm_wday),
        dec2bcd(t->tm_mon + 1),
        dec2bcd(year - YEAR_BASE),
    };
    ESP_RETURN_ON_ERROR(reg_write(REG_SECONDS, b, sizeof(b)), TAG, "set time");
    return ESP_OK;
}
