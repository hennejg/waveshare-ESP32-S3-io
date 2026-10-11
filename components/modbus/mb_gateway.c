#include "mb_gateway.h"
#include "mb_tcp_server.h"
#include "mb_pdu.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_config.h"
#include "mbcontroller.h"

#define TAG "mb_gw"

/* How long a request waits for its turn on the segment before being turned
   away, derived from the timeout actually configured rather than from the
   ceiling the configuration allows. It has to clear the worst case of every
   other connection worker ahead of it timing out first, plus the stack's
   cooldown; beyond that the client is told to retry rather than left hanging
   for longer than any Modbus client waits. Against the compile-time ceiling
   this came to 40 s, which no client would still be listening for.

   Derived rather than written out: the other connection workers and the
   Modbus master's poll task (mb_gateway_read_regs) are what can queue here,
   so raising MB_TCP_WORKERS without this following would make the wait too
   short and hand out "busy" for requests that would have been served, with
   nothing to say so. The poll task was left out of the count before: four
   silent devices ahead came to 5 s against a 4.25 s wait. */
#define GW_QUEUE_AHEAD MB_TCP_WORKERS
static uint32_t s_bus_wait_ms = 4000;

static void             *s_master = NULL;
static SemaphoreHandle_t s_bus;

/* mbc_master_start() refuses to start without a parameter descriptor table,
   and the table is only consulted by mbc_master_get/set_parameter(). The
   gateway never uses those -- it builds each request itself and calls
   mbc_master_send_request() -- so one placeholder entry is enough to get the
   stack running. Nothing ever looks it up. */
static const mb_parameter_descriptor_t s_unused_descr[] = {
    {
        .cid           = 0,
        .param_key     = "unused",
        .mb_slave_addr = 1,
        .mb_param_type = MB_PARAM_HOLDING,
        .mb_reg_start  = 0,
        .mb_size       = 1,
        .param_type    = PARAM_TYPE_U16,
        .param_size    = 2,
        .access        = PAR_PERMS_READ,
    },
};

bool mb_gateway_is_running(void)
{
    return s_master != NULL;
}

static bool s_forwarding;

void mb_gateway_set_forwarding(bool on) { s_forwarding = on; }
bool mb_gateway_forwarding(void)        { return s_forwarding && s_master != NULL; }

/* Takes back a half-finished start. This matters more than it looks: the
   controller fills in s_master at create time, so leaving it set after a
   later step failed would make mb_gateway_is_running() say yes and the TCP
   server forward requests into a master that was never started -- or whose
   UART pins were never applied, which leaves the board holding the bus. */
static void gateway_unwind(void)
{
    /* The controller installs the UART driver at create time, so dropping the
       pointer is not enough: the port would stay claimed and neither this
       master nor the RTU slave could ever have it again this boot. Delete
       before clearing, so nothing can find a half-freed object. */
    if (s_master) { (void)mbc_master_delete(s_master); s_master = NULL; }
    if (s_bus)    { vSemaphoreDelete(s_bus);           s_bus    = NULL; }
}

#define GW_START_CHECK(expr, what)                                   \
    do {                                                             \
        esp_err_t _e = (expr);                                       \
        if (_e != ESP_OK) {                                          \
            ESP_LOGE(TAG, "%s: %s", (what), esp_err_to_name(_e));    \
            gateway_unwind();                                        \
            return _e;                                               \
        }                                                            \
    } while (0)

esp_err_t mb_gateway_start(uart_port_t uart, uint32_t baudrate,
                           int tx_gpio, int rx_gpio, int rts_gpio,
                           uint16_t response_tout_ms)
{
    ESP_RETURN_ON_FALSE(!s_master, ESP_ERR_INVALID_STATE, TAG, "already running");

    s_bus = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_bus, ESP_ERR_NO_MEM, TAG, "bus mutex");

    mb_communication_info_t comm = {
        .ser_opts.mode             = MB_RTU,
        .ser_opts.port             = uart,
        .ser_opts.uid              = 0,      /* a master has no address of its own */
        .ser_opts.baudrate         = baudrate,
        .ser_opts.parity           = MB_PARITY_NONE,
        .ser_opts.data_bits        = UART_DATA_8_BITS,
        .ser_opts.stop_bits        = UART_STOP_BITS_1,
        .ser_opts.response_tout_ms = response_tout_ms,
    };

    GW_START_CHECK(mbc_master_create_serial(&comm, &s_master), "create serial master");

    /* Same reason as on the slave side: the controller installs the UART
       driver itself, so the pins and the half-duplex direction control have
       to be patched in afterwards. Without RTS on the transceiver's driver
       enable the board holds the bus and hears nothing. */
    GW_START_CHECK(uart_set_pin(uart, tx_gpio, rx_gpio, rts_gpio, UART_PIN_NO_CHANGE),
                   "uart_set_pin");
    GW_START_CHECK(uart_set_mode(uart, UART_MODE_RS485_HALF_DUPLEX), "uart_set_mode");
    GW_START_CHECK(mbc_master_set_descriptor(s_master, s_unused_descr, 1), "descriptor");
    GW_START_CHECK(mbc_master_start(s_master), "start");

    s_bus_wait_ms = ((uint32_t)response_tout_ms + 250u) * GW_QUEUE_AHEAD + 500u;

    ESP_LOGI(TAG, "RTU master on UART%d, %"PRIu32" baud, %u ms response timeout",
             (int)uart, baudrate, response_tout_ms);
    return ESP_OK;
}

/* ------------------------------------------------------------- forwarding */

/* One transaction, start to finish, with the segment held for its duration.
 *
 * The lock is not only about the wire. mbc_master_send_request() serialises
 * itself, but the exception code it leaves behind lives on the master object
 * and the next transaction clears it -- read outside the lock, a request
 * could end up reporting the exception from somebody else's.
 *
 * Three outcomes, and they have to stay apart:
 *   - the device answered with an exception: that code goes back unchanged. A
 *     client that asked for a register the device does not have must be told
 *     "illegal data address", not something about the gateway.
 *   - the device said nothing, or nothing that made sense (bad CRC, wrong
 *     address, wrong length -- ESP_ERR_INVALID_RESPONSE): 0x0B, gateway
 *     target device failed to respond. The path is fine; 0x0A made clients
 *     and operators look for a routing fault and stop retrying.
 *   - the gateway could not even try: 0x0A, gateway path unavailable.
 *   - the segment never came free: MB_GW_BUS_BUSY, which only the poll
 *     table sees; mb_gateway_handle() puts 0x06 on the wire for it.
 */
static uint8_t transact(uint8_t uid, uint8_t fc, uint16_t addr,
                        uint16_t count, void *data)
{
    if (!s_master) return MB_EXC_GW_PATH;

    if (xSemaphoreTake(s_bus, pdMS_TO_TICKS(s_bus_wait_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "uid %u fc %u: segment busy for %"PRIu32" ms, turned away",
                 uid, fc, s_bus_wait_ms);
        return MB_GW_BUS_BUSY;
    }

    mb_param_request_t req = {
        .slave_addr = uid,
        .command    = fc,
        .reg_start  = addr,
        .reg_size   = count,
    };
    esp_err_t err = mbc_master_send_request(s_master, &req, data);

    uint8_t exc = MB_EXC_NONE;
    if (err != ESP_OK) {
        uint8_t ex = 0;
        if (mbc_master_get_last_exception(s_master, &ex) == ESP_OK && ex != 0) {
            exc = ex;
        } else if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE) {
            exc = MB_EXC_GW_TARGET;
        } else {
            exc = MB_EXC_GW_PATH;
        }
    }
    xSemaphoreGive(s_bus);

    if (exc != MB_EXC_NONE)
        ESP_LOGD(TAG, "uid %u fc %u @%u x%u -> exception 0x%02x",
                 uid, fc, addr, count, exc);
    return exc;
}

/* The master hands registers over as host-order uint16, the wire wants them
   big endian, so neither direction is a memcpy. */
static void regs_to_wire(uint8_t *wire, const uint16_t *regs, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) mb_put16(&wire[i * 2], regs[i]);
}

static void wire_to_regs(uint16_t *regs, const uint8_t *wire, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++)
        regs[i] = mb_be16(&wire[i * 2]);
}

uint8_t mb_gateway_read_regs(uint8_t uid, uint8_t fc, uint16_t addr,
                             uint16_t count, uint16_t *regs)
{
    if (fc != MB_FC_READ_HOLDING && fc != MB_FC_READ_INPUT) return MB_EXC_ILLEGAL_FUNC;
    return transact(uid, fc, addr, count, regs);
}

static uint8_t handle(uint8_t uid, const mb_request_t *req,
                      uint8_t *resp, uint16_t *resp_len)
{
    *resp_len = 0;

    switch (req->fc) {

    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE: {
        size_t bytes = (size_t)MB_BIT_BYTES(req->count);
        if (bytes > MB_DATA_MAX) return MB_EXC_ILLEGAL_VALUE;
        /* The master copies into a buffer of its own, so the response slice
           cannot be handed to it directly. Both sides use the same packed
           layout though -- bit 0 is the first coil of the request -- so the
           copy back is a straight memcpy. */
        uint8_t tmp[MB_DATA_MAX];       /* the largest data part a PDU can carry */
        /* The master sets each bit on its own, read-modify-write, and leaves
           the rest of the last byte as it found it: without this the padding
           bits carried whatever the previous job left on this stack. */
        memset(tmp, 0, bytes);
        uint8_t exc = transact(uid, req->fc, req->addr, req->count, tmp);
        if (exc == MB_EXC_NONE) { memcpy(resp, tmp, bytes); *resp_len = bytes; }
        return exc;
    }

    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT: {
        if ((size_t)req->count * 2u > MB_DATA_MAX) return MB_EXC_ILLEGAL_VALUE;
        uint16_t tmp[MB_DATA_MAX / 2];
        uint8_t exc = transact(uid, req->fc, req->addr, req->count, tmp);
        if (exc == MB_EXC_NONE) {
            regs_to_wire(resp, tmp, req->count);
            *resp_len = (uint16_t)(req->count * 2u);
        }
        return exc;
    }

    case MB_FC_WRITE_COIL: {
        /* The stack takes the value as a uint16 and puts it on the wire as
           0xFF00 or 0x0000; the request already carries exactly those bytes. */
        uint16_t v = mb_be16(req->data);
        return transact(uid, req->fc, req->addr, 1, &v);
    }

    case MB_FC_WRITE_REGISTER: {
        uint16_t v = mb_be16(req->data);
        return transact(uid, req->fc, req->addr, 1, &v);
    }

    case MB_FC_WRITE_COILS: {
        size_t bytes = (size_t)MB_BIT_BYTES(req->count);
        if (bytes > MB_DATA_MAX) return MB_EXC_ILLEGAL_VALUE;
        uint8_t tmp[MB_DATA_MAX];       /* the largest data part a PDU can carry */
        memcpy(tmp, req->data, bytes);
        return transact(uid, req->fc, req->addr, req->count, tmp);
    }

    case MB_FC_WRITE_REGISTERS: {
        if ((size_t)req->count * 2u > MB_DATA_MAX) return MB_EXC_ILLEGAL_VALUE;
        uint16_t tmp[MB_DATA_MAX / 2];
        wire_to_regs(tmp, req->data, req->count);
        return transact(uid, req->fc, req->addr, req->count, tmp);
    }

    default:
        /* The controller's request API only reaches the eight function codes
           above. Relaying anything else would mean driving the stack's frame
           buffer directly, which is not worth it until something asks for it. */
        ESP_LOGD(TAG, "uid %u: function code %u cannot be relayed", uid, req->fc);
        return MB_EXC_ILLEGAL_FUNC;
    }
}

uint8_t mb_gateway_handle(uint8_t uid, const mb_request_t *req,
                          uint8_t *resp, uint16_t *resp_len)
{
    uint8_t exc = handle(uid, req, resp, resp_len);
    return exc == MB_GW_BUS_BUSY ? MB_EXC_DEVICE_BUSY : exc;   /* the wire's word for it */
}
