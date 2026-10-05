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
/* The isolated RS-485 transceiver is not auto-direction: the board brings its
   driver enable out on GPIO21, documented by Waveshare as "RS485 UART RTS pin".
   UART_MODE_RS485_HALF_DUPLEX makes the UART drive RTS as the direction
   signal, but only if RTS is actually routed to that pin. Left unrouted the
   pin floats, the driver sits enabled, and the board holds the whole bus --
   it can then neither answer nor hear anyone else. */
#define MB_RTS_GPIO GPIO_NUM_21

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

/* What the refresher last stored in the coil image, so it can tell its own
   value apart from one a master has written since. */
static uint8_t s_coils_mirror;
static bool    s_coils_mirror_valid;

static void update_timer_cb(void *arg)
{
    uint8_t di = 0;
    for (int i = 0; i < 8; i++) if (di_get(i)) di |= (uint8_t)(1u << i);
    uint8_t co = dout_get_all();   /* one consistent snapshot, not eight reads */

    mbc_slave_lock(s_handle);
    s_di.b[0] = di;
    /* The stack writes a master's command into this same byte and only then
       signals the event, so event_task() may not have read it yet. Refreshing
       unconditionally dropped commands the master had already been told were
       accepted -- measured at 3 of 40 writes on the bench. Only refresh while
       the image still holds what this function last put there. */
    if (!s_coils_mirror_valid || s_coils.b[0] == s_coils_mirror) {
        s_coils.b[0]         = co;
        s_coils_mirror       = co;
        s_coils_mirror_valid = true;
    }
    mbc_slave_unlock(s_handle);
}

static void apply_coil_write(const mb_param_info_t *info)
{
    mbc_slave_lock(s_handle);
    uint8_t co = s_coils.b[0];
    mbc_slave_unlock(s_handle);

    /* Apply only the coils this request actually addressed. The buffer always
       holds all eight bits, but the ones outside the request are a snapshot the
       refresher left behind: if CAN, a rule or the web UI moved an output since,
       writing them back would silently revert that change. FC05 therefore
       touches exactly one output, FC15 exactly its range. */
    unsigned first = info->mb_offset;
    unsigned last  = first + (info->size ? info->size : 1u);
    if (last > 8u) last = 8u;

    /* Build the masks for the addressed range and apply them in one go, so the
       coils of a single request switch together and the state cannot move
       between reading it and writing it. */
    uint8_t touched = 0;
    for (unsigned i = first; i < last; i++) touched |= (uint8_t)(1u << i);
    dout_modify((uint8_t)(co & touched), (uint8_t)(~co & touched), 0u);

    /* Re-sync the image with what the outputs actually took — a write that
       failed must not keep being reported back as the coil state — and hand
       the refresher a fresh reference value. */
    uint8_t actual = dout_get_all();
    mbc_slave_lock(s_handle);
    /* Only put the actual state back if nobody has written the image since we
       read it. The stack writes a new command straight into this byte and only
       afterwards queues its record, so an unconditional write-back erases a
       command the master has already been told was accepted. If it differs,
       leave it alone -- its record is queued and will be handled next. */
    if (s_coils.b[0] == co) {
        s_coils.b[0]         = actual;
        s_coils_mirror       = actual;
        s_coils_mirror_valid = true;
    }
    mbc_slave_unlock(s_handle);

    ESP_LOGD(TAG, "Coil write: coils %u..%u from 0x%02x, outputs 0x%02x",
             first, last - 1u, co, actual);
}

static void apply_hr_write(const mb_param_info_t *info)
{
    mbc_slave_lock(s_handle);
    uint16_t led_val    = s_hr.r[0];
    uint16_t buzzer_val = s_hr.r[1];
    mbc_slave_unlock(s_handle);

    /* One FC16 can cover both registers, so test each register against the
       written range instead of dispatching on the start offset alone. With
       the old if/else-if a write of HR40001+HR40002 only ever drove the LED. */
    unsigned first = info->mb_offset;
    unsigned last  = first + (info->size ? info->size : 1u);   /* [first, last) */

    if (first <= 0u && 0u < last) {
        apply_rgb252(led_val);
        ESP_LOGD(TAG, "HR40001 LED: 0x%04x", led_val);
    }
    if (first <= 1u && 1u < last && buzzer_val > 0) {
        buzzer_beep_once(buzzer_val, 200);
        ESP_LOGD(TAG, "HR40002 Buzzer: %u Hz", buzzer_val);
    }
}

/* Drains the stack's parameter FIFO and carries out the writes it reports. */
static void event_task(void *arg)
{
    for (;;) {
        mb_param_info_t info;

        /* Block on the parameter FIFO itself, not on the write event bits.
           Every access the stack serves queues a record here -- the six read
           types included -- and nothing else drains them. Waiting for write
           bits alone left the 20-deep queue permanently full after twenty
           requests from a master that only polls, and from then on the stack
           blocks for MB_PAR_INFO_TOUT before sending each further response:
           that constant is 10 *ticks*, which is 100 ms at the configured
           CONFIG_FREERTOS_HZ=100, so throughput collapses to about ten
           requests per second.

           Draining here also means a write is always dispatched on its own
           record instead of on whatever happened to be at the front of the
           queue, and it keeps the gap between the stack writing a command into
           the shared buffer and this task reading it down to the scheduler's
           latency. */
        if (mbc_slave_get_param_info(s_handle, &info, 1000) != ESP_OK) continue;

        if (info.type & MB_EVENT_HOLDING_REG_WR) {
            apply_hr_write(&info);
        } else if (info.type & MB_EVENT_COILS_WR) {
            apply_coil_write(&info);
        } else {
            continue;   /* a read: consumed so the queue cannot fill up */
        }

        /* An upstream control command — feed the rule engine's MODBUS
           command-health source (modbus(ms) in the DSL). */
        scripting_on_modbus_activity();
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
                     MB_RTS_GPIO, UART_PIN_NO_CHANGE),
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
