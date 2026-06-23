#include "i2c_bus.h"

#include "esp_check.h"
#include "esp_log.h"

#define TAG          "i2c_bus"
#define I2C_SDA_PIN  GPIO_NUM_42
#define I2C_SCL_PIN  GPIO_NUM_41

static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t i2c_bus_init(void)
{
    if (s_bus) return ESP_OK;   // already created

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = -1,          /* auto-select */
        .sda_io_num        = I2C_SDA_PIN,
        .scl_io_num        = I2C_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus init");
    ESP_LOGI(TAG, "I2C master bus ready (SDA=%d SCL=%d)", I2C_SDA_PIN, I2C_SCL_PIN);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_handle(void)
{
    return s_bus;
}
