#include "mb_server.h"
#include "app_config.h"
#include "di.h"
#include "dout.h"
#include "led.h"
#include "buzzer.h"
#include "scripting.h"

#include "mbcontroller.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG         "mb_server"
#define MB_UART     UART_NUM_1
#define MB_TX_GPIO  GPIO_NUM_17
#define MB_RX_GPIO  GPIO_NUM_18

/* ---------------------------------------------------------------- data stores */

/* Coils (RW): DO1-DO8, bit 0 = DO1 */
static struct { uint8_t b[1]; } s_coils;

/* Discrete inputs (RO): DI1-DI8, bit 0 = DI1 */
static struct { uint8_t b[1]; } s_di;

/* Holding registers (RW):
   HR40001 [0] = LED colour RGB252
   HR40002 [1] = Buzzer frequency (Hz); write triggers 200 ms beep */
static struct { uint16_t r[2]; } s_hr;

static void *s_handle = NULL;
static esp_timer_handle_t s_update_timer;

/* ---------------------------------------------------------------- colour decode */

/* RGB252: bits[15:14]=R(2), bits[13:9]=G(5), bits[8:7]=B(2), bits[6:0]=unused */
static void apply_rgb252(uint16_t reg)
{
    uint8_t r_raw = (reg >> 14) & 0x03;
    uint8_t g_raw = (reg >>  9) & 0x1F;
    uint8_t b_raw = (reg >>  7) & 0x03;
    uint8_t r = r_raw * 85;                        /* 0,85,170,255 */
    uint8_t g = (g_raw << 3) | (g_raw >> 2);       /* 5-bit → 8-bit */
    uint8_t b = b_raw * 85;
    led_set_rgb(r, g, b);
}

/* ---------------------------------------------------------------- timer + task */

static void update_timer_cb(void *arg)
{
    mbc_slave_lock(s_handle);

    uint8_t di = 0, co = 0;
    for (int i = 0; i < 8; i++) {
        if (di_get(i))   di |= (uint8_t)(1u << i);
        if (dout_get(i)) co |= (uint8_t)(1u << i);
    }
    s_di.b[0]    = di;
    s_coils.b[0] = co;

    mbc_slave_unlock(s_handle);
}

/* Handles write events from the Modbus master. */
static void event_task(void *arg)
{
    const mb_event_group_t WATCH =
        (mb_event_group_t)(MB_EVENT_COILS_WR | MB_EVENT_HOLDING_REG_WR);

    for (;;) {
        mb_event_group_t ev = mbc_slave_check_event(s_handle, WATCH);
        if (!ev) continue;

        mb_param_info_t info;
        if (mbc_slave_get_param_info(s_handle, &info, 10) != ESP_OK) continue;

        /* A coil/holding-register write is an upstream control command — feed the rule
         * engine's MODBUS command-health source (modbus(ms) in the DSL). */
        scripting_on_modbus_activity();

        if (ev & MB_EVENT_COILS_WR) {
            mbc_slave_lock(s_handle);
            uint8_t co = s_coils.b[0];
            mbc_slave_unlock(s_handle);
            for (uint8_t i = 0; i < 8; i++) {
                bool bit = (co >> i) & 1;
                if (bit != dout_get(i)) dout_set(i, bit);
            }
            ESP_LOGD(TAG, "Coil write: 0x%02x", co);
        }

        if (ev & MB_EVENT_HOLDING_REG_WR) {
            mbc_slave_lock(s_handle);
            uint16_t led_val    = s_hr.r[0];
            uint16_t buzzer_val = s_hr.r[1];
            mbc_slave_unlock(s_handle);

            if (info.mb_offset == 0) {
                apply_rgb252(led_val);
                ESP_LOGD(TAG, "HR40001 LED: 0x%04x", led_val);
            } else if (info.mb_offset == 1 && buzzer_val > 0) {
                buzzer_beep_once(buzzer_val, 200);
                ESP_LOGD(TAG, "HR40002 Buzzer: %u Hz", buzzer_val);
            }
        }
    }
}

/* ---------------------------------------------------------------- public */

esp_err_t mb_server_init(void)
{
    const app_config_t *cfg = app_config_get();
    if (!cfg->modbus.enable) {
        ESP_LOGI(TAG, "Modbus RTU disabled");
        return ESP_OK;
    }

    mb_communication_info_t comm = {
        .ser_opts.mode      = MB_RTU,
        .ser_opts.port      = MB_UART,
        .ser_opts.uid       = cfg->modbus.address,
        .ser_opts.baudrate  = cfg->modbus.baudrate,
        .ser_opts.parity    = MB_PARITY_NONE,
        .ser_opts.data_bits = UART_DATA_8_BITS,
        .ser_opts.stop_bits = UART_STOP_BITS_1,
    };

    ESP_RETURN_ON_ERROR(mbc_slave_create_serial(&comm, &s_handle),
                        TAG, "create serial slave");

    /* GPIO assignment and RS-485 half-duplex mode must be set after
       create but before start — the controller installs the UART driver
       internally; uart_set_pin/mode patch it afterwards. */
    ESP_RETURN_ON_ERROR(
        uart_set_pin(MB_UART, MB_TX_GPIO, MB_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG, "uart_set_pin");
    ESP_RETURN_ON_ERROR(
        uart_set_mode(MB_UART, UART_MODE_RS485_HALF_DUPLEX),
        TAG, "uart_set_mode");

    /* Register data areas */
    mb_register_area_descriptor_t area = {0};

    area.type = MB_PARAM_COIL;  area.start_offset = 0;
    area.address = &s_coils;    area.size = sizeof(s_coils);
    area.access  = MB_ACCESS_RW;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "coil desc");

    area.type = MB_PARAM_DISCRETE;  area.start_offset = 0;
    area.address = &s_di;           area.size = sizeof(s_di);
    area.access  = MB_ACCESS_RO;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "di desc");

    area.type = MB_PARAM_HOLDING;  area.start_offset = 0;
    area.address = &s_hr;          area.size = sizeof(s_hr);
    area.access  = MB_ACCESS_RW;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "hr desc");

    ESP_RETURN_ON_ERROR(mbc_slave_start(s_handle), TAG, "start");

    esp_timer_create_args_t ta = { .callback = update_timer_cb, .name = "mb_update" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_update_timer), TAG, "timer create");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_update_timer, 10000), TAG, "timer start");

    xTaskCreate(event_task, "mb_event", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Modbus RTU slave started — addr=%u baud=%"PRIu32,
             cfg->modbus.address, cfg->modbus.baudrate);
    return ESP_OK;
}
