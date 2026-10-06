#include "mb_server.h"

#include <string.h>
#include "app_config.h"
#include "di.h"
#include "dout.h"
#include "led.h"
#include "buzzer.h"
#include "scripting.h"
#include "mb_gateway.h"
#include "mb_tcp_server.h"

#include "mbcontroller.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

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

/* ------------------------------------------------------------ register layout */

#define MB_NUM_COILS     8     /* DO1-DO8, bit 0 = DO1            */
#define MB_NUM_DISCRETE  8     /* DI1-DI8, bit 0 = DI1            */
#define MB_NUM_HOLDING   2     /* [0] LED colour RGB252, [1] buzzer Hz */

/* The RTU stack still wants descriptors for the areas it serves, but they are
   not the data path: the access callbacks below are overridden, so nothing
   reads or writes these buffers. Reads are answered from the live I/O state
   and writes are captured as commands. */
static struct { uint8_t  b[1]; } s_coils_unused;
static struct { uint8_t  b[1]; } s_di_unused;
static struct { uint16_t r[MB_NUM_HOLDING]; } s_hr_unused;

static void *s_handle = NULL;        /* the RTU slave instance, if any */

/* ---------------------------------------------------------------- commands */

/* A write as the master sent it: which area, which registers, and a copy of
   the values. A pointer into the request frame would not do -- the frame is
   reused for the next request, so by the time the command ran the values
   could already belong to a later one. */
typedef struct {
    uint8_t  area;                      /* MB_PARAM_COIL or MB_PARAM_HOLDING */
    uint16_t offset;                    /* zero-based first register/coil     */
    uint16_t count;
    uint8_t  bits;                      /* coils: absolute bit positions      */
    uint16_t regs[MB_NUM_HOLDING];      /* holding: regs[i] is offset + i     */
} mb_cmd_t;

#define MB_CMD_QUEUE_DEPTH 8
static QueueHandle_t s_cmd_q;

/* What the most recent write asked for, so a read gives back what was
   written even before the command task has carried it out.
 *
 * Holding registers need this because nothing else remembers them. Coils need
 * it for a different reason: a read goes to the live hardware, so a client
 * that writes a coil and reads it straight back would be shown the state its
 * own write is about to change. s_do_pending counts, per coil, the writes
 * still waiting; a read takes the shadow for those coils and the live state
 * for the rest, so a change made over MQTT or by a rule is still seen at once.
 *
 * The mutex covers the shadows AND the enqueue, as one step. Two tasks can
 * reach here -- the RTU slave's and the TCP server's -- and if they could
 * interleave, the queue order and the shadow order could end up opposite, and
 * the registers would report one value for ever while the relays held
 * another. */
static uint16_t          s_hr_shadow[MB_NUM_HOLDING];
static uint8_t           s_do_shadow;
static uint8_t           s_do_pending[MB_NUM_COILS];
static SemaphoreHandle_t s_cmd_mux;

#define CMD_LOCK()   xSemaphoreTake(s_cmd_mux, portMAX_DELAY)
#define CMD_UNLOCK() xSemaphoreGive(s_cmd_mux)

/* ---------------------------------------------------------------- colour decode */

/* RGB252: bits[15:14]=R(2), bits[13:9]=G(5), bits[8:7]=B(2), bits[6:0]=unused */
static void apply_rgb252(uint16_t reg)
{
    uint8_t r = (reg >> 14) & 0x03;
    uint8_t g = (reg >> 9)  & 0x1F;
    uint8_t b = (reg >> 7)  & 0x03;
    led_set_rgb((uint8_t)(r * 85), (uint8_t)(g * 8), (uint8_t)(b * 85));
}

/* ------------------------------------------------------------ local I/O access

   The semantics of this board's own registers, in one place. Both ways in --
   the RTU slave's register callbacks and the TCP server's request handler --
   go through these, so the two can never drift apart.

   None of them touches I2C or publishes anything: dout_get_all() and di_get()
   are cached reads behind a short mutex, and a write becomes a queued command
   that the command task carries out. That is what makes them safe to call
   from several connection workers at once. */

/* Called with the mutex held. */
static uint8_t enqueue(const mb_cmd_t *cmd, const char *what)
{
    /* The queue is created before anything can deliver a request, but an
       out-of-memory failure at start-up leaves it absent while the TCP server
       still comes up. Sending to a null queue is an abort, not a refusal. */
    if (!s_cmd_q) {
        ESP_LOGE(TAG, "no command queue, refused %s", what);
        return MB_EXC_DEVICE_FAILURE;
    }
    if (xQueueSend(s_cmd_q, cmd, 0) == pdTRUE) return MB_EXC_NONE;

    /* "Slave device busy" is the canonical "I could not take this, try
       again". A broadcast gets no response at all, so for those this log line
       is the only trace; say so rather than pretend it was carried out. */
    ESP_LOGW(TAG, "command queue full, refused %s (a broadcast would be lost silently)",
             what);
    return MB_EXC_DEVICE_BUSY;
}

static uint8_t local_read_coils(uint16_t addr, uint16_t count, uint8_t *out)
{
    if ((uint32_t)addr + count > MB_NUM_COILS) return MB_EXC_ILLEGAL_ADDR;

    memset(out, 0, (size_t)((count + 7u) / 8u));
    uint8_t live = dout_get_all();

    CMD_LOCK();
    for (uint16_t i = 0; i < count; i++) {
        uint8_t ch = (uint8_t)(addr + i);
        uint8_t v  = s_do_pending[ch] ? (s_do_shadow & (1u << ch))
                                      : (live       & (1u << ch));
        if (v) out[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    CMD_UNLOCK();
    return MB_EXC_NONE;
}

static uint8_t local_read_discrete(uint16_t addr, uint16_t count, uint8_t *out)
{
    if ((uint32_t)addr + count > MB_NUM_DISCRETE) return MB_EXC_ILLEGAL_ADDR;

    memset(out, 0, (size_t)((count + 7u) / 8u));
    for (uint16_t i = 0; i < count; i++)
        if (di_get((uint8_t)(addr + i))) out[i >> 3] |= (uint8_t)(1u << (i & 7));
    return MB_EXC_NONE;
}

static uint8_t local_read_holding(uint16_t addr, uint16_t count, uint8_t *out)
{
    if ((uint32_t)addr + count > MB_NUM_HOLDING) return MB_EXC_ILLEGAL_ADDR;

    CMD_LOCK();
    for (uint16_t i = 0; i < count; i++) {           /* big endian on the wire */
        uint16_t v = s_hr_shadow[addr + i];
        out[i * 2]     = (uint8_t)(v >> 8);
        out[i * 2 + 1] = (uint8_t)(v & 0xFF);
    }
    CMD_UNLOCK();
    return MB_EXC_NONE;
}

/* bits is packed with bit 0 = the coil at addr, which is how both the FC15
   payload and a single coil reduced to one byte arrive. */
static uint8_t local_write_coils(uint16_t addr, uint16_t count, const uint8_t *bits)
{
    if ((uint32_t)addr + count > MB_NUM_COILS) return MB_EXC_ILLEGAL_ADDR;

    mb_cmd_t cmd = { .area = MB_PARAM_COIL, .offset = addr, .count = count };
    for (uint16_t i = 0; i < count; i++)
        if (bits[i >> 3] & (1u << (i & 7)))
            cmd.bits |= (uint8_t)(1u << (addr + i));

    CMD_LOCK();
    uint8_t exc = enqueue(&cmd, "coil write");
    if (exc == MB_EXC_NONE) {
        for (uint16_t i = 0; i < count; i++) {
            uint8_t ch = (uint8_t)(addr + i);
            if (cmd.bits & (1u << ch)) s_do_shadow |=  (uint8_t)(1u << ch);
            else                       s_do_shadow &= (uint8_t)~(1u << ch);
            s_do_pending[ch]++;
        }
    }
    CMD_UNLOCK();
    return exc;
}

static uint8_t local_write_holding(uint16_t addr, uint16_t count, const uint8_t *regs_be)
{
    if ((uint32_t)addr + count > MB_NUM_HOLDING) return MB_EXC_ILLEGAL_ADDR;

    mb_cmd_t cmd = { .area = MB_PARAM_HOLDING, .offset = addr, .count = count };
    for (uint16_t i = 0; i < count; i++)
        cmd.regs[i] = (uint16_t)((regs_be[i * 2] << 8) | regs_be[i * 2 + 1]);

    CMD_LOCK();
    uint8_t exc = enqueue(&cmd, "holding register write");
    if (exc == MB_EXC_NONE)                       /* read-back follows the command */
        for (uint16_t i = 0; i < count; i++) s_hr_shadow[addr + i] = cmd.regs[i];
    CMD_UNLOCK();
    return exc;
}

/* ------------------------------------------------------------ request handler */

uint8_t mb_server_handle(const mb_request_t *req, uint8_t *resp, uint16_t *resp_len)
{
    *resp_len = 0;

    uint8_t exc;

    switch (req->fc) {
    /* The length is set only once the read succeeded. An exception response
       carries no data, and a caller that framed resp anyway would put
       whatever was on its stack onto the wire. */
    case MB_FUNC_READ_COILS:
        exc = local_read_coils(req->addr, req->count, resp);
        if (!exc) *resp_len = (uint16_t)((req->count + 7u) / 8u);
        return exc;

    case MB_FUNC_READ_DISCRETE_INPUTS:
        exc = local_read_discrete(req->addr, req->count, resp);
        if (!exc) *resp_len = (uint16_t)((req->count + 7u) / 8u);
        return exc;

    case MB_FUNC_READ_HOLDING_REGISTER:
        exc = local_read_holding(req->addr, req->count, resp);
        if (!exc) *resp_len = (uint16_t)(req->count * 2u);
        return exc;

    case MB_FUNC_READ_INPUT_REGISTER:
        return MB_EXC_ILLEGAL_ADDR;     /* no input registers on this device */

    case MB_FUNC_WRITE_SINGLE_COIL: {
        /* The value arrives as the two bytes of the request: 0xFF00 on,
           0x0000 off, and nothing else is a valid single-coil write. */
        uint8_t bit = (req->data[0] == 0xFF) ? 1u : 0u;
        return local_write_coils(req->addr, 1, &bit);
    }

    case MB_FUNC_WRITE_MULTIPLE_COILS:
        return local_write_coils(req->addr, req->count, req->data);

    case MB_FUNC_WRITE_REGISTER:
    case MB_FUNC_WRITE_MULTIPLE_REGISTERS:
        return local_write_holding(req->addr, req->count, req->data);

    default:
        return MB_EXC_ILLEGAL_FUNC;
    }
}

/* ----------------------------------------- RTU slave register access hooks

   mbc_reg_*_slave_cb are declared weak by the component, so these replace the
   default implementations. They are thin: everything they do is in the local
   access functions above, which the TCP path uses as well.

   Note that the default implementations are also what fed the stack's
   parameter FIFO. With them replaced, nothing queues parameter records at
   all, so that queue can no longer fill up and stall responses. */

/* The stack maps a callback's error onto an exception itself, and reaches
   only three codes. The local handlers produce only those three. */
static mb_err_enum_t exc_to_err(uint8_t exc)
{
    switch (exc) {
    case MB_EXC_NONE:         return MB_ENOERR;
    case MB_EXC_ILLEGAL_ADDR: return MB_ENOREG;
    case MB_EXC_DEVICE_BUSY:  return MB_ETIMEDOUT;
    default:                  return MB_EIO;
    }
}

mb_err_enum_t mbc_reg_coils_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                     uint16_t address, uint16_t n_coils,
                                     mb_reg_mode_enum_t mode)
{
    (void)inst;
    if (!reg_buffer) return MB_EINVAL;
    address--;                                   /* the stack passes it +1 */

    /* On a read the stack hands out a slice of the response frame without
       clearing it; the local reader zeroes the padding bits itself. */
    if (mode == MB_REG_READ)
        return exc_to_err(local_read_coils(address, n_coils, reg_buffer));
    return exc_to_err(local_write_coils(address, n_coils, reg_buffer));
}

mb_err_enum_t mbc_reg_discrete_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                        uint16_t address, uint16_t n_discrete)
{
    (void)inst;
    if (!reg_buffer) return MB_EINVAL;
    address--;
    return exc_to_err(local_read_discrete(address, n_discrete, reg_buffer));
}

mb_err_enum_t mbc_reg_holding_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                       uint16_t address, uint16_t n_regs,
                                       mb_reg_mode_enum_t mode)
{
    (void)inst;
    if (!reg_buffer) return MB_EINVAL;
    address--;

    if (mode == MB_REG_READ)
        return exc_to_err(local_read_holding(address, n_regs, reg_buffer));
    return exc_to_err(local_write_holding(address, n_regs, reg_buffer));
}

mb_err_enum_t mbc_reg_input_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                     uint16_t address, uint16_t n_regs)
{
    (void)inst; (void)reg_buffer; (void)address; (void)n_regs;
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

    CMD_LOCK();
    uint8_t live = dout_get_all();
    for (uint16_t i = 0; i < cmd->count; i++) {
        uint8_t ch = (uint8_t)(cmd->offset + i);
        if (s_do_pending[ch]) s_do_pending[ch]--;
        /* A write the hardware refused must not leave the shadow claiming it
           happened: once no write is outstanding for this coil, the shadow is
           put back in step with the relay. */
        if (!s_do_pending[ch]) {
            if (live & (1u << ch)) s_do_shadow |=  (uint8_t)(1u << ch);
            else                   s_do_shadow &= (uint8_t)~(1u << ch);
        }
    }
    CMD_UNLOCK();
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

/* Carries out accepted commands in the order they arrived. Runs outside both
   stacks entirely, so the I2C transfer and any MQTT publishing it triggers
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

static esp_err_t start_command_task(void)
{
    if (s_cmd_q) return ESP_OK;

    s_cmd_mux = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_cmd_mux, ESP_ERR_NO_MEM, TAG, "command mutex");
    s_do_shadow = dout_get_all();       /* start in step with the relays */

    s_cmd_q = xQueueCreate(MB_CMD_QUEUE_DEPTH, sizeof(mb_cmd_t));
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_NO_MEM, TAG, "command queue");

    if (xTaskCreate(command_task, "mb_cmd", 4096, NULL, 5, NULL) != pdPASS) {
        /* The queue has to go with it. Left behind, the guard above would
           report success on the next call and every write would be accepted
           into a queue nobody empties -- answered positively, read back from
           the shadow registers as if it had happened, and never carried out. */
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
        vSemaphoreDelete(s_cmd_mux);
        s_cmd_mux = NULL;
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "command task");
    }
    return ESP_OK;
}

/* The board on the RS-485 segment: either answering as a slave, or driving it
   as a master on behalf of TCP clients. One UART and one master per segment,
   so it is one or the other. */
static esp_err_t start_rtu_slave(const app_config_t *cfg)
{
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

    mb_register_area_descriptor_t area = {0};

    area.type = MB_PARAM_COIL;      area.start_offset = 0;
    area.address = &s_coils_unused; area.size = sizeof(s_coils_unused);
    area.access  = MB_ACCESS_RW;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "coil desc");

    area.type = MB_PARAM_DISCRETE;  area.start_offset = 0;
    area.address = &s_di_unused;    area.size = sizeof(s_di_unused);
    area.access  = MB_ACCESS_RO;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "di desc");

    area.type = MB_PARAM_HOLDING;   area.start_offset = 0;
    area.address = &s_hr_unused;    area.size = sizeof(s_hr_unused);
    area.access  = MB_ACCESS_RW;
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(s_handle, area), TAG, "hr desc");

    ESP_RETURN_ON_ERROR(mbc_slave_start(s_handle), TAG, "start");

    ESP_LOGI(TAG, "RS-485: RTU slave, address %u, %"PRIu32" baud",
             cfg->modbus.address, cfg->modbus.baudrate);
    return ESP_OK;
}

esp_err_t mb_server_init(void)
{
    const app_config_t *cfg = app_config_get();
    if (!cfg->modbus.enable) {
        ESP_LOGI(TAG, "Modbus disabled");
        return ESP_OK;
    }

    /* Ready to accept commands before anything can deliver one. */
    ESP_RETURN_ON_ERROR(start_command_task(), TAG, "command task");

    if (cfg->modbus.rs485_role == MB_ROLE_MASTER) {
        /* Nothing but the TCP server ever asks the master for anything, so
           say plainly that the segment will sit idle rather than leave
           someone wondering why their meters are not being read. */
        if (!cfg->modbus.tcp_server)
            ESP_LOGW(TAG, "RS-485 is set to master but the TCP server is off — "
                          "nothing will drive the segment");

        /* The TCP server that feeds it needs a working IP stack, which
           app_main() does not have yet; mb_server_net_start() finishes up. */
        uint16_t tout = cfg->modbus.rs485_tout_ms;
        if (tout < MB_RS485_TOUT_MIN_MS || tout > MB_RS485_TOUT_MAX_MS)
            tout = MB_RS485_TOUT_DEFAULT_MS;   /* stored before the field existed */

        return mb_gateway_start(MB_UART, cfg->modbus.baudrate,
                                MB_TX_GPIO, MB_RX_GPIO, MB_RTS_GPIO, tout);
    }

    return start_rtu_slave(cfg);
}

esp_err_t mb_server_net_start(void)
{
    const app_config_t *cfg = app_config_get();
    if (!cfg->modbus.enable || !cfg->modbus.tcp_server) return ESP_OK;

    uint8_t uid = cfg->modbus.tcp_uid;
    if (uid < 1 || uid > 247) uid = MB_TCP_UID_DEFAULT;   /* stored before the field existed */

    esp_err_t ret = mb_tcp_server_start(MB_TCP_PORT, uid);
    if (ret != ESP_OK) return ret;

    if (cfg->modbus.rs485_role == MB_ROLE_MASTER)
        ESP_LOGI(TAG, "Modbus TCP: own I/Os at unit ID %u, every other unit ID "
                      "forwarded to RS-485 at %"PRIu32" baud",
                 uid, cfg->modbus.baudrate);
    else
        ESP_LOGI(TAG, "Modbus TCP: own I/Os at unit ID %u; no gateway, the "
                      "RS-485 side is a slave", uid);
    return ESP_OK;
}
