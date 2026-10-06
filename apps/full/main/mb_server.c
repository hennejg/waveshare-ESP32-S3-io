#include "mb_server.h"

#include <string.h>
#include "app_config.h"
#include "di.h"
#include "dout.h"
#include "led.h"
#include "buzzer.h"
#include "scripting.h"
#include "mb_gateway.h"

#include "mbcontroller.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

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

/* ------------------------------------------------------------------ routing

   SPIKE. Hard-coded while the mechanism is being proved on hardware; the
   configuration model that replaces these three constants is the next step.
   With MB_GW_SPIKE set the board serves Modbus TCP on port 502 and drives the
   RS-485 segment as a master, instead of sitting on it as a slave.

   A TCP slave created with uid 0 accepts every unit ID the client sends --
   that is the stack's wildcard, and without it the gateway would never see a
   request meant for anyone else. Which device a request belongs to is then
   decided per request, from the unit ID:

     MB_LOCAL_UID, 0 or 255 -> this board's own I/Os
     anything else          -> forwarded to that address on RS-485

   0 and 255 count as local because a TCP client with nothing to address
   sends one of them, and on TCP neither means broadcast. */
#define MB_GW_SPIKE   1
#define MB_LOCAL_UID  247
#define MB_TCP_PORT   502

/* ------------------------------------------------------------ register layout */

#define MB_NUM_COILS     8     /* DO1-DO8, bit 0 = DO1            */
#define MB_NUM_DISCRETE  8     /* DI1-DI8, bit 0 = DI1            */
#define MB_NUM_HOLDING   2     /* [0] LED colour RGB252, [1] buzzer Hz */

/* The stack still wants descriptors for the areas it serves, but they are no
   longer the data path: the access callbacks below are overridden, so nothing
   reads or writes these buffers. Reads are answered from the live I/O state
   and writes are captured as commands. */
static struct { uint8_t  b[1]; } s_coils_unused;
static struct { uint8_t  b[1]; } s_di_unused;
static struct { uint16_t r[MB_NUM_HOLDING]; } s_hr_unused;

static void *s_handle = NULL;        /* the slave instance serving requests */

/* Which device the request being processed is addressed to. Returns true for
   this board, and otherwise leaves the foreign unit ID in *uid.

   This runs inside a register callback, so "the request being processed" is
   well defined: the stack is single-threaded per slave instance and has not
   touched the next frame yet. */
static bool request_is_local(uint8_t *uid)
{
    *uid = MB_LOCAL_UID;
    if (!mb_gateway_is_running()) return true;   /* nowhere to forward to */
    if (mbc_slave_get_request_uid(s_handle, uid) != ESP_OK) return true;
    return (*uid == MB_LOCAL_UID) || (*uid == 0) || (*uid == 255);
}

/* ---------------------------------------------------------------- commands */

/* A write as the master sent it: which area, which registers, and a copy of
   the values. A pointer into the register image would not do -- the stack
   writes the next command into that same memory, so by the time the command
   ran the values could already belong to a later request. */
typedef struct {
    uint8_t  area;                      /* MB_PARAM_COIL or MB_PARAM_HOLDING */
    uint16_t offset;                    /* zero-based first register/coil     */
    uint16_t count;
    uint8_t  bits;                      /* coils: absolute bit positions      */
    uint16_t regs[MB_NUM_HOLDING];      /* holding: regs[i] is offset + i     */
} mb_cmd_t;

#define MB_CMD_QUEUE_DEPTH 8
static QueueHandle_t s_cmd_q;

/* Last values accepted for the holding registers, so a read gives back what
   was written. Written and read on the Modbus task only. */
static uint16_t s_hr_shadow[MB_NUM_HOLDING];

/* ---------------------------------------------------------------- colour decode */

/* RGB252: bits[15:14]=R(2), bits[13:9]=G(5), bits[8:7]=B(2), bits[6:0]=unused */
static void apply_rgb252(uint16_t reg)
{
    uint8_t r = (reg >> 14) & 0x03;
    uint8_t g = (reg >> 9)  & 0x1F;
    uint8_t b = (reg >> 7)  & 0x03;
    led_set_rgb((uint8_t)(r * 85), (uint8_t)(g * 8), (uint8_t)(b * 85));
}

/* ------------------------------------------------------- register access hooks

   mbc_reg_*_slave_cb are declared weak by the component, so these replace the
   default implementations. That matters for three reasons:

   - A write is copied out here, while the master's frame is still the only
     thing that has touched it, and queued as a command. Nothing can overwrite
     it afterwards, and commands are executed in the order they arrived.
   - Reads are answered from the live I/O state instead of from a buffer that
     a timer has to keep refreshed, so there is no shared image for a refresh
     and a command to fight over.
   - If the command queue is full the master is told so, instead of receiving a
     normal positive response for a command that was dropped.

   These run on the Modbus port task. They take no I2C and publish nothing;
   dout_get_all() and di_get() are cached reads behind a short mutex. The
   switching itself happens later, on the command task.

   Note that the default implementations are also what fed the stack's
   parameter FIFO. With them replaced, nothing queues parameter records at all,
   so that queue can no longer fill up and stall responses. */

static mb_err_enum_t enqueue(const mb_cmd_t *cmd, const char *what)
{
    if (xQueueSend(s_cmd_q, cmd, 0) == pdTRUE) return MB_ENOERR;

    /* MB_ETIMEDOUT is the one error the stack turns into exception 6, "slave
       device busy" -- the canonical "I could not take this, try again". A
       broadcast gets no response at all, so for those this log line is the
       only trace; say so rather than pretend the command was carried out. */
    ESP_LOGW(TAG, "command queue full, refused %s (a broadcast would be lost silently)",
             what);
    return MB_ETIMEDOUT;
}

mb_err_enum_t mbc_reg_coils_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                     uint16_t address, uint16_t n_coils,
                                     mb_reg_mode_enum_t mode)
{
    (void)inst;
    if (!reg_buffer) return MB_EINVAL;
    address--;                                   /* the stack passes it +1 */

    uint8_t uid;
    if (!request_is_local(&uid))
        return mb_gateway_coils(uid, reg_buffer, address, n_coils, mode);

    if ((uint32_t)address + n_coils > MB_NUM_COILS) return MB_ENOREG;

    if (mode == MB_REG_READ) {
        /* The stack hands us a slice of the response frame without clearing
           it, so the padding bits of the last byte would otherwise carry
           leftovers of an earlier request. Modbus wants them zero. */
        memset(reg_buffer, 0, (size_t)((n_coils + 7u) / 8u));
        uint8_t live = dout_get_all();
        for (uint16_t i = 0; i < n_coils; i++)
            if (live & (1u << (address + i))) reg_buffer[i >> 3] |= (uint8_t)(1u << (i & 7));
        return MB_ENOERR;
    }

    mb_cmd_t cmd = { .area = MB_PARAM_COIL, .offset = address, .count = n_coils };
    for (uint16_t i = 0; i < n_coils; i++)
        if (reg_buffer[i >> 3] & (1u << (i & 7)))
            cmd.bits |= (uint8_t)(1u << (address + i));
    return enqueue(&cmd, "coil write");
}

mb_err_enum_t mbc_reg_discrete_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                        uint16_t address, uint16_t n_discrete)
{
    (void)inst;
    if (!reg_buffer) return MB_EINVAL;
    address--;

    uint8_t uid;
    if (!request_is_local(&uid))
        return mb_gateway_discrete(uid, reg_buffer, address, n_discrete);

    if ((uint32_t)address + n_discrete > MB_NUM_DISCRETE) return MB_ENOREG;

    memset(reg_buffer, 0, (size_t)((n_discrete + 7u) / 8u));   /* see the coil path */
    for (uint16_t i = 0; i < n_discrete; i++)
        if (di_get((uint8_t)(address + i))) reg_buffer[i >> 3] |= (uint8_t)(1u << (i & 7));
    return MB_ENOERR;
}

mb_err_enum_t mbc_reg_holding_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                       uint16_t address, uint16_t n_regs,
                                       mb_reg_mode_enum_t mode)
{
    (void)inst;
    if (!reg_buffer) return MB_EINVAL;
    address--;

    uint8_t uid;
    if (!request_is_local(&uid))
        return mb_gateway_holding(uid, reg_buffer, address, n_regs, mode);

    if ((uint32_t)address + n_regs > MB_NUM_HOLDING) return MB_ENOREG;

    if (mode == MB_REG_READ) {
        for (uint16_t i = 0; i < n_regs; i++) {           /* big endian on the wire */
            uint16_t v = s_hr_shadow[address + i];
            reg_buffer[i * 2]     = (uint8_t)(v >> 8);
            reg_buffer[i * 2 + 1] = (uint8_t)(v & 0xFF);
        }
        return MB_ENOERR;
    }

    mb_cmd_t cmd = { .area = MB_PARAM_HOLDING, .offset = address, .count = n_regs };
    for (uint16_t i = 0; i < n_regs; i++)
        cmd.regs[i] = (uint16_t)((reg_buffer[i * 2] << 8) | reg_buffer[i * 2 + 1]);

    mb_err_enum_t err = enqueue(&cmd, "holding register write");
    if (err == MB_ENOERR)                                  /* read-back follows the command */
        for (uint16_t i = 0; i < n_regs; i++) s_hr_shadow[address + i] = cmd.regs[i];
    return err;
}

mb_err_enum_t mbc_reg_input_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                     uint16_t address, uint16_t n_regs)
{
    (void)inst;
    if (!reg_buffer) return MB_EINVAL;
    address--;

    uint8_t uid;
    if (!request_is_local(&uid))
        return mb_gateway_input(uid, reg_buffer, address, n_regs);

    return MB_ENOREG;          /* no input registers on this device */
}

/* ---------------------------------------------------------------- command task */

static void run_coil_cmd(const mb_cmd_t *cmd)
{
    uint8_t mask = 0;
    for (uint16_t i = 0; i < cmd->count; i++) mask |= (uint8_t)(1u << (cmd->offset + i));

    /* Only the addressed coils, and all of them in one transfer. */
    esp_err_t ret = dout_modify((uint8_t)(cmd->bits & mask),
                                (uint8_t)(~cmd->bits & mask), 0u);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "coil write %u..%u failed: %s",
                 cmd->offset + 1u, cmd->offset + cmd->count, esp_err_to_name(ret));
}

static void run_holding_cmd(const mb_cmd_t *cmd)
{
    /* A single FC16 can cover both registers, so test each one against the
       written range rather than dispatching on the start offset alone. */
    for (uint16_t i = 0; i < cmd->count; i++) {
        uint16_t reg = (uint16_t)(cmd->offset + i);
        uint16_t val = cmd->regs[i];
        if (reg == 0) {
            apply_rgb252(val);
            ESP_LOGD(TAG, "HR40001 LED: 0x%04x", val);
        } else if (reg == 1 && val > 0) {
            buzzer_beep_once(val, 200);
            ESP_LOGD(TAG, "HR40002 Buzzer: %u Hz", val);
        }
    }
}

/* Carries out accepted commands in the order they arrived. Runs outside the
   stack entirely, so the I2C transfer and any MQTT publishing it triggers
   cannot delay a Modbus response or sit inside one of its locked sections. */
static void command_task(void *arg)
{
    (void)arg;
    for (;;) {
        mb_cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) continue;

        if (cmd.area == MB_PARAM_COIL) run_coil_cmd(&cmd);
        else                           run_holding_cmd(&cmd);

        /* An upstream control command — feed the rule engine's MODBUS
           command-health source (modbus(ms) in the DSL). */
        scripting_on_modbus_activity();
    }
}

/* ---------------------------------------------------------------- public */

/* The stack still wants these even though the access callbacks are
   overridden, and they have to be registered on each slave instance
   separately. */
static esp_err_t register_areas(void)
{
    mb_register_area_descriptor_t area = {0};

    area.type = MB_PARAM_COIL;     area.start_offset = 0;
    area.address = &s_coils_unused; area.size = sizeof(s_coils_unused);
    area.access  = MB_ACCESS_RW;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "coil desc");

    area.type = MB_PARAM_DISCRETE;  area.start_offset = 0;
    area.address = &s_di_unused;    area.size = sizeof(s_di_unused);
    area.access  = MB_ACCESS_RO;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "di desc");

    area.type = MB_PARAM_HOLDING;  area.start_offset = 0;
    area.address = &s_hr_unused;   area.size = sizeof(s_hr_unused);
    area.access  = MB_ACCESS_RW;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "hr desc");
    return ESP_OK;
}

static esp_err_t start_command_task(void)
{
    if (s_cmd_q) return ESP_OK;
    s_cmd_q = xQueueCreate(MB_CMD_QUEUE_DEPTH, sizeof(mb_cmd_t));
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_NO_MEM, TAG, "command queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(command_task, "mb_cmd", 4096, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "command task");
    return ESP_OK;
}

esp_err_t mb_server_init(void)
{
    const app_config_t *cfg = app_config_get();
    if (!cfg->modbus.enable) {
        ESP_LOGI(TAG, "Modbus disabled");
        return ESP_OK;
    }

    /* Ready to accept commands before any stack can deliver one. */
    ESP_RETURN_ON_ERROR(start_command_task(), TAG, "command task");

#if MB_GW_SPIKE
    /* The RS-485 port belongs to the master now; the two cannot share it. The
       TCP slave that feeds it cannot be created yet -- creating it opens a
       listening socket, and lwIP is only brought up much later in app_main().
       mb_server_net_start() finishes the job from the network-ready path. */
    ESP_RETURN_ON_ERROR(mb_gateway_start(MB_UART, cfg->modbus.baudrate,
                                         MB_TX_GPIO, MB_RX_GPIO, MB_RTS_GPIO),
                        TAG, "gateway master");
    return ESP_OK;
#else
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

    ESP_RETURN_ON_ERROR(register_areas(), TAG, "areas");
    ESP_RETURN_ON_ERROR(mbc_slave_start(s_handle), TAG, "start");

    ESP_LOGI(TAG, "Modbus RTU slave started — addr=%u baud=%"PRIu32,
             cfg->modbus.address, cfg->modbus.baudrate);
    return ESP_OK;
#endif
}

#if MB_GW_SPIKE

/* Brings the TCP slave up. Everything here wants a working IP stack, which is
   why it cannot run from mb_server_init(), and it is given a task of its own
   rather than running on the caller's: the network-ready callback arrives on
   the system event task, whose stack is sized for short handlers and which
   creating a Modbus TCP slave overflows outright. */
static void net_start_task(void *arg)
{
    (void)arg;
    const app_config_t *cfg = app_config_get();

    mb_communication_info_t comm = {
        .tcp_opts.mode          = MB_TCP,
        .tcp_opts.port          = MB_TCP_PORT,
        .tcp_opts.uid           = 0,     /* wildcard: take every unit ID */
        .tcp_opts.addr_type     = MB_IPV4,
        .tcp_opts.ip_addr_table = NULL,
        /* A slave only listens, so it needs no interface of its own: the netif
           pointer is used for mDNS and for binding a master's outgoing socket,
           neither of which applies here. */
        .tcp_opts.ip_netif_ptr  = NULL,
    };

    esp_err_t err = mbc_slave_create_tcp(&comm, &s_handle);
    if (err == ESP_OK) err = register_areas();
    if (err == ESP_OK) err = mbc_slave_start(s_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modbus TCP start failed: %s", esp_err_to_name(err));
        s_handle = NULL;            /* keep request routing out of a dead stack */
    } else {
        ESP_LOGI(TAG, "Modbus TCP on port %u — own I/Os at unit ID %u, "
                      "every other unit ID forwarded to RS-485 at %"PRIu32" baud",
                 MB_TCP_PORT, MB_LOCAL_UID, cfg->modbus.baudrate);
    }
    vTaskDelete(NULL);
}

esp_err_t mb_server_net_start(void)
{
    const app_config_t *cfg = app_config_get();
    if (!cfg->modbus.enable) return ESP_OK;

    /* Both interfaces report ready on the same task, so a plain flag is enough
       to keep the second report from starting a second slave. */
    static bool started = false;
    if (started) return ESP_OK;
    started = true;

    ESP_RETURN_ON_FALSE(
        xTaskCreate(net_start_task, "mb_tcp_up", 6144, NULL, 5, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "tcp start task");
    return ESP_OK;
}

#else

esp_err_t mb_server_net_start(void) { return ESP_OK; }

#endif
