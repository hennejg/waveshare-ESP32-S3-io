#include "mb_server.h"
#include "mb_pdu.h"

#include <string.h>
#include "app_config.h"
#include "di.h"
#include "dout.h"
#include "led.h"
#include "buzzer.h"
/* The rule engine's modbus(ms) watchdog; declared here rather than included,
   so this component does not need the scripting component (board does the
   same). Without the engine nothing watches Modbus activity. */
#ifdef CONFIG_APP_SCRIPTING_ENABLE
void scripting_on_modbus_activity(void);
#else
static inline void scripting_on_modbus_activity(void) {}
#endif
#include "mb_gateway.h"
#include "mb_tcp_server.h"
#include "mb_ident.h"
#include "aux_io.h"

#include "mbcontroller.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
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

/* DO1-DO8, then the header pads as coils 9-11 when the board offers them; a
   pad that is not an output reads 0 and refuses a write with 0x03. */
#define MB_NUM_DO        8
#define MB_NUM_COILS     (MB_NUM_DO + APP_CFG_DI_AUX_COUNT)
/* DI1-DI8, then AUX1-AUX3 (DI 10009-10011) when the board offers the pads;
   a pad that is not an input reads 0. */
#define MB_NUM_DISCRETE  APP_CFG_DI_COUNT
/* [0] LED colour RGB252, [1] buzzer Hz, then per pad 1..3 three registers:
   duty cycle in 0.1 %, frequency in Hz high word, low word. They act on the
   running PWM and are not stored -- see aux_io_pwm_set(). */
#define MB_HR_BASE       2
#define MB_HR_PER_PAD    3
#define MB_NUM_HOLDING   (MB_HR_BASE + MB_HR_PER_PAD * APP_CFG_DI_AUX_COUNT)
/* Input registers 100.. after the identification block: AUX1 analog mV, then
   the mode of pads 1..3 (0 standard, 1 input, 2 PWM, 3 analog). */
#define MB_IR_AUX_BASE   100
#define MB_IR_AUX_COUNT  (APP_CFG_DI_AUX_COUNT ? 1 + APP_CFG_DI_AUX_COUNT : 0)

/* The RTU stack still wants descriptors for the areas it serves, but they are
   not the data path: the access callbacks below are overridden, so nothing
   reads or writes these buffers. Reads are answered from the live I/O state
   and writes are captured as commands. */
static struct { uint8_t  b[(MB_NUM_COILS + 7) / 8]; } s_coils_unused;
static struct { uint8_t  b[(MB_NUM_DISCRETE + 7) / 8]; } s_di_unused;
static struct { uint16_t r[MB_NUM_HOLDING]; } s_hr_unused;
static struct { uint16_t r[MB_IR_AUX_COUNT ? MB_IR_AUX_BASE + MB_IR_AUX_COUNT : MB_IDENT_REG_COUNT]; } s_ir_unused;

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

/* True once the command task, its queue and the mutex exist.
 *
 * mb_server_init() runs at boot from the configuration as it was then;
 * mb_server_net_start() runs later, from the network-ready callback, and
 * reads the configuration as it is by then. Those are not the same thing: a
 * board booted with Modbus off, then configured and saved, then reconnecting
 * its WiFi, would have started the TCP listener on top of a local side that
 * was never built -- and the first request would have taken a null mutex,
 * which FreeRTOS answers with an abort. The listener now refuses instead, and
 * the accessors below refuse too, so neither depends on the other being
 * right. */
static bool s_local_ready;

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

   None of them touches I2C or publishes anything: dout_get_all() is a
   lock-free snapshot, di_get() a cached read behind a short mutex, and a
   write becomes a queued command
   that the command task carries out. That is what makes them safe to call
   from several connection workers at once. */

/* Called with the mutex held. */
static uint8_t enqueue(const mb_cmd_t *cmd, const char *what)
{
    /* Only ever reached through an accessor that has already refused unless
       s_local_ready, which is set once the queue and the mutex both exist. */
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
    if (!s_local_ready) return MB_EXC_DEVICE_FAILURE;
    if ((uint32_t)addr + count > MB_NUM_COILS) return MB_EXC_ILLEGAL_ADDR;

    memset(out, 0, (size_t)MB_BIT_BYTES(count));

    CMD_LOCK();
    /* Inside the lock, not before it. The command task stores the new relay
       state before it takes this lock to clear the pending count, so a read
       that finds the count cleared also finds the new state. Taken outside,
       a read could hold the state from before the write and find the count
       already cleared -- and a client would read OFF straight after its ON
       had been acknowledged. */
    uint8_t live = dout_get_all();
    for (uint16_t i = 0; i < count; i++) {
        uint8_t ch = (uint8_t)(addr + i);
        bool    v;
        if (ch >= MB_NUM_DO) {
            v = false;                           /* a pad: as it runs, 0 if not an output */
            aux_io_out_get((uint8_t)(ch - MB_NUM_DO), &v);
        } else {
            v = s_do_pending[ch] ? (s_do_shadow & (1u << ch)) : (live & (1u << ch));
        }
        if (v) out[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    CMD_UNLOCK();
    return MB_EXC_NONE;
}

static uint8_t local_read_discrete(uint16_t addr, uint16_t count, uint8_t *out)
{
    if (!s_local_ready) return MB_EXC_DEVICE_FAILURE;
    if ((uint32_t)addr + count > MB_NUM_DISCRETE) return MB_EXC_ILLEGAL_ADDR;

    memset(out, 0, (size_t)MB_BIT_BYTES(count));
    for (uint16_t i = 0; i < count; i++)
        if (di_get((uint8_t)(addr + i))) out[i >> 3] |= (uint8_t)(1u << (i & 7));
    return MB_EXC_NONE;
}

static uint8_t local_read_holding(uint16_t addr, uint16_t count, uint8_t *out)
{
    if (!s_local_ready) return MB_EXC_DEVICE_FAILURE;
    if ((uint32_t)addr + count > MB_NUM_HOLDING) return MB_EXC_ILLEGAL_ADDR;

    /* The PWM as it runs, one snapshot per pad: read register by register,
       the two frequency words could come from two different settings. A pad
       that is not a PWM output reads 0. */
    uint32_t f[AUX_PAD_COUNT] = {0};
    uint16_t d[AUX_PAD_COUNT] = {0};
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++)
        if (!aux_io_pwm_info(a, &f[a], &d[a], NULL)) { f[a] = 0; d[a] = 0; }

    CMD_LOCK();
    for (uint16_t i = 0; i < count; i++) {           /* big endian on the wire */
        uint16_t reg = (uint16_t)(addr + i);
        uint16_t v;
        if (reg < MB_HR_BASE) {
            v = s_hr_shadow[reg];
        } else {
            uint8_t a = (uint8_t)((reg - MB_HR_BASE) / MB_HR_PER_PAD);
            uint8_t k = (uint8_t)((reg - MB_HR_BASE) % MB_HR_PER_PAD);
            v = (k == 0) ? d[a] : (k == 1) ? (uint16_t)(f[a] >> 16) : (uint16_t)f[a];
        }
        mb_put16(&out[i * 2], v);
    }
    CMD_UNLOCK();
    return MB_EXC_NONE;
}

/* bits is packed with bit 0 = the coil at addr, which is how both the FC15
   payload and a single coil reduced to one byte arrive. */
static uint8_t local_write_coils(uint16_t addr, uint16_t count, const uint8_t *bits)
{
    if (!s_local_ready) return MB_EXC_DEVICE_FAILURE;
    if ((uint32_t)addr + count > MB_NUM_COILS) return MB_EXC_ILLEGAL_ADDR;

    /* Everything is checked before anything moves: each pad must be an
       output, and -- if the request also carries DO1-DO8 -- the command queue
       must have room for them. The pads are set at once (a GPIO, no I2C) and
       DO1-DO8 queued after, all under CMD_LOCK, which every enqueue holds: no
       one can take the free slot in between, so a request refused as busy has
       switched nothing. */
    for (uint16_t i = 0; i < count; i++) {
        uint16_t ch = (uint16_t)(addr + i);
        if (ch >= MB_NUM_DO && !aux_io_out_get((uint8_t)(ch - MB_NUM_DO), NULL))
            return MB_EXC_ILLEGAL_VALUE;
    }
    uint16_t do_n = (addr < MB_NUM_DO) ? (uint16_t)(MB_NUM_DO - addr) : 0;
    if (do_n > count) do_n = count;

    CMD_LOCK();
    if (do_n && uxQueueSpacesAvailable(s_cmd_q) == 0) {
        CMD_UNLOCK();
        ESP_LOGW(TAG, "command queue full, refused coil write");
        return MB_EXC_DEVICE_BUSY;
    }
    /* The pads in one call: checked again and set under the pad driver's
       lock, so a mode changed since the check above refuses the whole
       request rather than leaving the first pads switched. */
    uint8_t pad_mask = 0, pad_on = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t ch = (uint16_t)(addr + i);
        if (ch < MB_NUM_DO) continue;
        pad_mask |= (uint8_t)(1u << (ch - MB_NUM_DO));
        if ((bits[i >> 3] >> (i & 7)) & 1u) pad_on |= (uint8_t)(1u << (ch - MB_NUM_DO));
    }
    if (pad_mask) {
        if (aux_io_out_set_many(pad_mask, pad_on) != ESP_OK) {
            CMD_UNLOCK();
            return MB_EXC_ILLEGAL_VALUE;              /* switched mode since the check */
        }
        scripting_on_modbus_activity();
    }
    if (!do_n) { CMD_UNLOCK(); return MB_EXC_NONE; }

    /* DO1-DO8 through the command task, as before. */
    count = do_n;
    mb_cmd_t cmd = { .area = MB_PARAM_COIL, .offset = addr, .count = count };
    for (uint16_t i = 0; i < count; i++)
        if (bits[i >> 3] & (1u << (i & 7)))
            cmd.bits |= (uint8_t)(1u << (addr + i));

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

/* The PWM part of a holding write, checked as a whole before anything moves:
   every pad it touches must be a PWM output, and the values it writes must be
   in range. Only what is written changes: an unwritten duty cycle stays as it
   runs, an unwritten frequency too. A write of only one frequency word
   combines with the other word of the frequency as it was set; FC 16 over
   both words changes the frequency in one step. This check answers early;
   aux_io_pwm_write() makes it again under the pad driver's lock and applies
   the write as a whole, or nothing of it. */
static uint8_t plan_pwm(uint16_t addr, uint16_t count, const uint8_t *regs_be,
                        aux_pwm_write_t req[AUX_PAD_COUNT])
{
    for (uint16_t i = 0; i < count; i++) {
        uint16_t reg = (uint16_t)(addr + i);
        if (reg < MB_HR_BASE) continue;
        uint8_t  a = (uint8_t)((reg - MB_HR_BASE) / MB_HR_PER_PAD);
        uint8_t  k = (uint8_t)((reg - MB_HR_BASE) % MB_HR_PER_PAD);
        uint16_t v = mb_be16(&regs_be[i * 2]);
        if      (k == 0) { req[a].duty_set = true; req[a].duty = v; }
        else if (k == 1) { req[a].hi_set   = true; req[a].hi   = v; }
        else             { req[a].lo_set   = true; req[a].lo   = v; }
    }
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) {
        if (!req[a].duty_set && !req[a].hi_set && !req[a].lo_set) continue;
        uint32_t set_f;
        if (!aux_io_pwm_info(a, &set_f, NULL, NULL)) return MB_EXC_ILLEGAL_VALUE;   /* not PWM */
        if (req[a].duty_set && req[a].duty > AUX_PWM_DUTY_MAX) return MB_EXC_ILLEGAL_VALUE;
        if (req[a].hi_set || req[a].lo_set) {
            uint32_t hi = req[a].hi_set ? req[a].hi : (uint16_t)(set_f >> 16);
            uint32_t lo = req[a].lo_set ? req[a].lo : (uint16_t)set_f;
            uint32_t f  = (hi << 16) | lo;
            if (f < AUX_PWM_FREQ_MIN || f > AUX_PWM_FREQ_MAX) return MB_EXC_ILLEGAL_VALUE;
        }
    }
    return MB_EXC_NONE;
}

static uint8_t local_write_holding(uint16_t addr, uint16_t count, const uint8_t *regs_be)
{
    if (!s_local_ready) return MB_EXC_DEVICE_FAILURE;
    if ((uint32_t)addr + count > MB_NUM_HOLDING) return MB_EXC_ILLEGAL_ADDR;

    aux_pwm_write_t pwm[AUX_PAD_COUNT] = {0};
    uint8_t exc = plan_pwm(addr, count, regs_be, pwm);
    if (exc != MB_EXC_NONE) return exc;

    /* Capacity before anything moves: if the request carries LED or buzzer
       registers, the command queue must have room for them. Checked, the PWM
       applied at once and the command queued, all under CMD_LOCK -- which
       every enqueue holds -- so no one takes the free slot in between, and a
       request refused as busy has changed nothing. */
    uint16_t base_n = (addr < MB_HR_BASE) ? (uint16_t)(MB_HR_BASE - addr) : 0;
    if (base_n > count) base_n = count;

    CMD_LOCK();
    if (base_n && uxQueueSpacesAvailable(s_cmd_q) == 0) {
        CMD_UNLOCK();
        ESP_LOGW(TAG, "command queue full, refused holding register write");
        return MB_EXC_DEVICE_BUSY;
    }
    bool any = false;
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++)
        any |= pwm[a].duty_set || pwm[a].hi_set || pwm[a].lo_set;
    if (any) {
        esp_err_t r = aux_io_pwm_write(pwm);     /* all pads, or none */
        if (r != ESP_OK) {
            CMD_UNLOCK();
            return (r == ESP_ERR_INVALID_STATE || r == ESP_ERR_INVALID_ARG)
                   ? MB_EXC_ILLEGAL_VALUE : MB_EXC_DEVICE_FAILURE;
        }
        scripting_on_modbus_activity();
    }

    /* LED and buzzer go through the command task as before. */
    if (base_n) {
        mb_cmd_t cmd = { .area = MB_PARAM_HOLDING, .offset = addr, .count = base_n };
        for (uint16_t i = 0; i < base_n; i++)
            cmd.regs[i] = mb_be16(&regs_be[i * 2]);
        exc = enqueue(&cmd, "holding register write");   /* room checked above */
        if (exc == MB_EXC_NONE)                   /* read-back follows the command */
            for (uint16_t i = 0; i < base_n; i++) s_hr_shadow[addr + i] = cmd.regs[i];
    }
    CMD_UNLOCK();
    return exc;
}

/* Input registers: the identification block, then the pads. */
static uint8_t local_read_input(uint16_t addr, uint16_t count, uint32_t local_ip, uint8_t *out)
{
    if (addr < MB_IR_AUX_BASE)
        return mb_ident_read_input(addr, count, local_ip, out);
    if ((uint32_t)addr + count > (uint32_t)MB_IR_AUX_BASE + MB_IR_AUX_COUNT)
        return MB_EXC_ILLEGAL_ADDR;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t r = (uint16_t)(addr + i - MB_IR_AUX_BASE);
        uint16_t v = 0;
        if (r == 0) {
            int mv;
            if (aux_io_analog_mv(0, &mv) == ESP_OK) v = (uint16_t)(mv < 0 ? 0 : mv > 65535 ? 65535 : mv);
        } else {
            v = (uint16_t)aux_io_mode((uint8_t)(r - 1));
        }
        mb_put16(&out[i * 2], v);
    }
    return MB_EXC_NONE;
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
    case MB_FC_READ_COILS:
        exc = local_read_coils(req->addr, req->count, resp);
        if (!exc) *resp_len = (uint16_t)MB_BIT_BYTES(req->count);
        return exc;

    case MB_FC_READ_DISCRETE:
        exc = local_read_discrete(req->addr, req->count, resp);
        if (!exc) *resp_len = (uint16_t)MB_BIT_BYTES(req->count);
        return exc;

    case MB_FC_READ_HOLDING:
        exc = local_read_holding(req->addr, req->count, resp);
        if (!exc) *resp_len = (uint16_t)(req->count * 2u);
        return exc;

    case MB_FC_READ_INPUT:
        exc = local_read_input(req->addr, req->count, req->local_ip, resp);
        if (!exc) *resp_len = (uint16_t)(req->count * 2u);
        return exc;

    case MB_FC_DEVICE_ID:
        return mb_ident_device_id(req, resp, resp_len);

    case MB_FC_WRITE_COIL: {
        /* The value arrives as the two bytes of the request: 0xFF00 on,
           0x0000 off, and nothing else is a valid single-coil write. */
        uint8_t bit = (req->data[0] == 0xFF) ? 1u : 0u;
        return local_write_coils(req->addr, 1, &bit);
    }

    case MB_FC_WRITE_COILS:
        return local_write_coils(req->addr, req->count, req->data);

    case MB_FC_WRITE_REGISTER:
    case MB_FC_WRITE_REGISTERS:
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

/* The stack maps a callback's error onto an exception itself and reaches
   only 0x02, 0x04 and 0x06; 0x03, which the PWM registers return, goes
   through the fork's MB_ERR_EXCEPTION. */
static mb_err_enum_t exc_to_err(uint8_t exc)
{
    switch (exc) {
    case MB_EXC_NONE:         return MB_ENOERR;
    case MB_EXC_ILLEGAL_ADDR: return MB_ENOREG;
    case MB_EXC_DEVICE_BUSY:  return MB_ETIMEDOUT;
    /* The stack's own mapping knows only 0x02, 0x04 and 0x06; the fork lets
       a callback put any code on the wire (mb_types.h). */
    case MB_EXC_ILLEGAL_VALUE: return MB_ERR_EXCEPTION(MB_EXC_ILLEGAL_VALUE);
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
    (void)inst;
    if (!reg_buffer) return MB_EINVAL;
    address--;
    return exc_to_err(local_read_input(address, n_regs, 0, reg_buffer));
}

/* FC 43 on RS-485. The stack has no idea of Read Device Identification but
   lets a function code be given its own handler: the handler gets the request
   PDU in the stack's frame buffer, leaves the response PDU in its place, and
   returns the exception that the stack then frames itself. The same parser,
   encoder and response builder as over TCP, so the two cannot drift apart. */
static mb_exception_t rtu_device_id_handler(void *inst, uint8_t *frame, uint16_t *len)
{
    (void)inst;
    if (!frame || !len) return MB_EX_SLAVE_DEVICE_FAILURE;

    mb_request_t req;
    uint8_t  data[MB_DATA_MAX];
    uint16_t dlen = 0;
    uint8_t exc = mb_parse_pdu(frame, *len, &req);
    if (!exc) exc = mb_ident_device_id(&req, data, &dlen);
    if (exc) return (mb_exception_t)exc;        /* same numbering on both sides */

    /* Built straight into the stack's frame buffer: for FC 43 the response
       builder reads only the function code of the request, which it has
       already taken from the frame. */
    *len = mb_build_response(&req, frame, data, dlen, MB_EXC_NONE, frame);
    return MB_EX_NONE;
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
    if (s_local_ready) return ESP_OK;

    s_cmd_mux = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_cmd_mux, ESP_ERR_NO_MEM, TAG, "command mutex");
    s_do_shadow = dout_get_all();       /* start in step with the relays */

    s_cmd_q = xQueueCreate(MB_CMD_QUEUE_DEPTH, sizeof(mb_cmd_t));
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_NO_MEM, TAG, "command queue");

    /* Stack in PSRAM: the commands switch outputs, the LED, the buzzer and
       the pads -- I2C, RMT, LEDC -- and never write the flash. */
    if (xTaskCreateWithCaps(command_task, "mb_cmd", 4096, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        /* The queue and the mutex go with it: s_local_ready stays false, so
           nothing would ever empty them again. */
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
        vSemaphoreDelete(s_cmd_mux);
        s_cmd_mux = NULL;
        ESP_LOGE(TAG, "command task could not be created");
        return ESP_ERR_NO_MEM;
    }

    s_local_ready = true;
    return ESP_OK;
}

/* Takes back a half-finished start, as gateway_unwind() does for the master:
   the controller installs the UART driver at create time, and a start that
   failed after it left the port claimed for the rest of the boot. */
static void rtu_slave_unwind(void)
{
    if (s_handle) { (void)mbc_slave_delete(s_handle); s_handle = NULL; }
}

#define RTU_START_CHECK(expr, what)                                  \
    do {                                                             \
        esp_err_t _e = (expr);                                       \
        if (_e != ESP_OK) {                                          \
            ESP_LOGE(TAG, "%s: %s", (what), esp_err_to_name(_e));    \
            rtu_slave_unwind();                                      \
            return _e;                                               \
        }                                                            \
    } while (0)

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

    RTU_START_CHECK(mbc_slave_create_serial(&comm, &s_handle), "create serial slave");

    /* GPIO assignment and RS-485 half-duplex mode must be set after
       create but before start — the controller installs the UART driver
       internally; uart_set_pin/mode patch it afterwards. */
    RTU_START_CHECK(uart_set_pin(MB_UART, MB_TX_GPIO, MB_RX_GPIO,
                                 MB_RTS_GPIO, UART_PIN_NO_CHANGE), "uart_set_pin");
    RTU_START_CHECK(uart_set_mode(MB_UART, UART_MODE_RS485_HALF_DUPLEX), "uart_set_mode");

    mb_register_area_descriptor_t area = {0};

    area.type = MB_PARAM_COIL;      area.start_offset = 0;
    area.address = &s_coils_unused; area.size = sizeof(s_coils_unused);
    area.access  = MB_ACCESS_RW;
    RTU_START_CHECK(mbc_slave_set_descriptor(s_handle, area), "coil desc");

    area.type = MB_PARAM_DISCRETE;  area.start_offset = 0;
    area.address = &s_di_unused;    area.size = sizeof(s_di_unused);
    area.access  = MB_ACCESS_RO;
    RTU_START_CHECK(mbc_slave_set_descriptor(s_handle, area), "di desc");

    area.type = MB_PARAM_HOLDING;   area.start_offset = 0;
    area.address = &s_hr_unused;    area.size = sizeof(s_hr_unused);
    area.access  = MB_ACCESS_RW;
    RTU_START_CHECK(mbc_slave_set_descriptor(s_handle, area), "hr desc");

    area.type = MB_PARAM_INPUT;     area.start_offset = 0;
    area.address = &s_ir_unused;    area.size = sizeof(s_ir_unused);
    area.access  = MB_ACCESS_RO;
    RTU_START_CHECK(mbc_slave_set_descriptor(s_handle, area), "ir desc");

    RTU_START_CHECK(mbc_set_handler(s_handle, MB_FC_DEVICE_ID, rtu_device_id_handler), "fc43 handler");

    RTU_START_CHECK(mbc_slave_start(s_handle), "start");

    ESP_LOGI(TAG, "RS-485: RTU slave, address %u, %"PRIu32" baud",
             cfg->modbus.address, cfg->modbus.baudrate);
    return ESP_OK;
}

esp_err_t mb_server_init(void)
{
    const app_config_t *cfg = app_config_get();
    mb_ident_init();
    if (!cfg->modbus.enable) {
        ESP_LOGI(TAG, "Modbus disabled");
        return ESP_OK;
    }

    /* Ready to accept commands before anything can deliver one. */
    ESP_RETURN_ON_ERROR(start_command_task(), TAG, "command task");

#if !CONFIG_MB_RS485_MASTER
    if (cfg->modbus.rs485_role == MB_ROLE_MASTER)
        ESP_LOGW(TAG, "RS-485 master is not part of this firmware — running as RTU slave");
#else
    if (cfg->modbus.rs485_role == MB_ROLE_MASTER) {
        /* The master serves two clients: the poll table, which reads devices
           on the segment by itself, and -- with the gateway on -- Modbus TCP
           clients asking for other unit IDs. Forwarding needs the TCP server,
           which needs a working IP stack app_main() does not have yet;
           mb_server_net_start() finishes that part. */
        bool fwd = cfg->modbus.gateway && cfg->modbus.tcp_server;
        if (cfg->modbus.gateway && !cfg->modbus.tcp_server)
            ESP_LOGW(TAG, "gateway is on but the Modbus TCP server is off — nothing to forward");
        esp_err_t r = mb_gateway_start(MB_UART, cfg->modbus.baudrate,
                                       MB_TX_GPIO, MB_RX_GPIO, MB_RTS_GPIO,
                                       cfg->modbus.rs485_tout_ms);
        mb_gateway_set_forwarding(r == ESP_OK && fwd);
        return r;
    }
#endif

    return start_rtu_slave(cfg);
}

bool mb_server_running(void)
{
    return s_local_ready;
}

esp_err_t mb_server_net_start(void)
{
    const app_config_t *cfg = app_config_get();
    if (!cfg->modbus.enable || !cfg->modbus.tcp_server) return ESP_OK;

    /* The saved configuration may have been changed since boot. Serving
       requests needs the local side that mb_server_init() builds, and that
       ran under the old configuration. */
    if (!s_local_ready) {
        ESP_LOGW(TAG, "Modbus TCP not started: Modbus was off when the board "
                      "came up — reboot to apply");
        return ESP_OK;
    }

    esp_err_t ret = mb_tcp_server_start(MB_TCP_PORT, cfg->modbus.tcp_uid);
    if (ret != ESP_OK) return ret;

    if (mb_gateway_forwarding())
        ESP_LOGI(TAG, "Modbus TCP: own I/Os at unit ID %u, every other unit ID "
                      "forwarded to RS-485 at %"PRIu32" baud",
                 cfg->modbus.tcp_uid, cfg->modbus.baudrate);
    else
        ESP_LOGI(TAG, "Modbus TCP: own I/Os at unit ID %u; no gateway, other unit "
                      "IDs answer 0x0A", cfg->modbus.tcp_uid);
    return ESP_OK;
}
