#include "mb_gateway.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"

#define TAG "mb_gw"

/* A read can ask for 2000 coils (250 bytes) or 125 registers (250 bytes), a
   write for 1968 coils or 123 registers. One buffer covers all four. */
#define GW_BUF_BYTES 256

/* How long a device on the segment gets to answer. The component's default is
   five seconds, which suits a master that polls on its own schedule; here a
   TCP client is waiting for the answer and the slave task is blocked until it
   comes, so a silent device has to be given up on quickly. 500 ms is well
   above the wire time of the longest request at 9600 baud. */
#define GW_RESPONSE_TOUT_MS 500

static void *s_master = NULL;

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

esp_err_t mb_gateway_start(uart_port_t uart, uint32_t baudrate,
                           int tx_gpio, int rx_gpio, int rts_gpio)
{
    ESP_RETURN_ON_FALSE(!s_master, ESP_ERR_INVALID_STATE, TAG, "already running");

    mb_communication_info_t comm = {
        .ser_opts.mode      = MB_RTU,
        .ser_opts.port      = uart,
        .ser_opts.uid       = 0,            /* a master has no address of its own */
        .ser_opts.baudrate  = baudrate,
        .ser_opts.parity    = MB_PARITY_NONE,
        .ser_opts.data_bits = UART_DATA_8_BITS,
        .ser_opts.stop_bits = UART_STOP_BITS_1,
        .ser_opts.response_tout_ms = GW_RESPONSE_TOUT_MS,
    };

    ESP_RETURN_ON_ERROR(mbc_master_create_serial(&comm, &s_master),
                        TAG, "create serial master");

    /* Same reason as on the slave side: the controller installs the UART
       driver itself, so the pins and the half-duplex direction control have
       to be patched in afterwards. Without RTS on the transceiver's driver
       enable the board holds the bus and hears nothing. */
    ESP_RETURN_ON_ERROR(uart_set_pin(uart, tx_gpio, rx_gpio, rts_gpio,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");
    ESP_RETURN_ON_ERROR(uart_set_mode(uart, UART_MODE_RS485_HALF_DUPLEX),
                        TAG, "uart_set_mode");

    ESP_RETURN_ON_ERROR(mbc_master_set_descriptor(s_master, s_unused_descr, 1),
                        TAG, "descriptor");
    ESP_RETURN_ON_ERROR(mbc_master_start(s_master), TAG, "start");

    ESP_LOGI(TAG, "RTU master on UART%d, %"PRIu32" baud", (int)uart, baudrate);
    return ESP_OK;
}

/* ------------------------------------------------------------- forwarding */

/* Turns the outcome of one RTU transaction into what the TCP client should
   see. Three cases, and they have to stay apart:

   - the device answered with an exception: that code goes back unchanged. A
     client that asked for a register the device does not have must be told
     "illegal data address", not something about the gateway.
   - the device said nothing: 0x0B, gateway target device failed to respond.
     This is the code the protocol reserves for exactly this.
   - the gateway itself could not even try (busy, bad argument): 0x0A,
     gateway path unavailable.

   MB_ERR_EXCEPTION() is what carries a verbatim code out of a register
   callback; the plain mb_err_enum_t return values can only produce 0x02,
   0x04 and 0x06. */
static mb_err_enum_t to_wire(esp_err_t err, uint8_t uid, uint8_t fc)
{
    if (err == ESP_OK) return MB_ENOERR;

    uint8_t ex = 0;
    if (mbc_master_get_last_exception(s_master, &ex) == ESP_OK && ex != MB_EX_NONE) {
        ESP_LOGD(TAG, "uid %u fc %u: slave exception 0x%02x passed on", uid, fc, ex);
        return MB_ERR_EXCEPTION(ex);
    }

    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGD(TAG, "uid %u fc %u: no answer", uid, fc);
        return MB_ERR_EXCEPTION(MB_EX_GATEWAY_TGT_FAILED);
    }

    ESP_LOGW(TAG, "uid %u fc %u: forwarding failed: %s", uid, fc, esp_err_to_name(err));
    return MB_ERR_EXCEPTION(MB_EX_GATEWAY_PATH_FAILED);
}

static mb_err_enum_t forward(uint8_t uid, uint8_t fc, uint16_t addr,
                             uint16_t count, void *data)
{
    if (!s_master) return MB_ERR_EXCEPTION(MB_EX_GATEWAY_PATH_FAILED);

    mb_param_request_t req = {
        .slave_addr = uid,
        .command    = fc,
        .reg_start  = addr,
        .reg_size   = count,
    };
    return to_wire(mbc_master_send_request(s_master, &req, data), uid, fc);
}

mb_err_enum_t mb_gateway_coils(uint8_t uid, uint8_t *buf, uint16_t addr,
                               uint16_t count, mb_reg_mode_enum_t mode)
{
    size_t bytes = (size_t)((count + 7u) / 8u);
    if (bytes > GW_BUF_BYTES) return MB_ERR_EXCEPTION(MB_EX_ILLEGAL_DATA_VALUE);

    /* The master's own callbacks copy into a buffer of their own, so the
       slave's response slice cannot be handed to them directly. Both sides
       use the same packed layout though -- bit 0 is the first coil of the
       request -- so the copy is a straight memcpy. */
    uint8_t tmp[GW_BUF_BYTES];

    if (mode == MB_REG_READ) {
        mb_err_enum_t err = forward(uid, MB_FUNC_READ_COILS, addr, count, tmp);
        if (err == MB_ENOERR) memcpy(buf, tmp, bytes);
        return err;
    }

    memcpy(tmp, buf, bytes);
    if (count == 1) {
        /* A single coil goes out as FC05. Some devices implement it and not
           FC15, and the client asked for one coil, so sending one is also the
           more faithful relay. The stack reads the value as a uint16: 0xFF00
           on, 0x0000 off. */
        uint16_t v = (tmp[0] & 1u) ? 0xFF00u : 0x0000u;
        return forward(uid, MB_FUNC_WRITE_SINGLE_COIL, addr, 1, &v);
    }
    return forward(uid, MB_FUNC_WRITE_MULTIPLE_COILS, addr, count, tmp);
}

mb_err_enum_t mb_gateway_discrete(uint8_t uid, uint8_t *buf, uint16_t addr,
                                  uint16_t count)
{
    size_t bytes = (size_t)((count + 7u) / 8u);
    if (bytes > GW_BUF_BYTES) return MB_ERR_EXCEPTION(MB_EX_ILLEGAL_DATA_VALUE);

    uint8_t tmp[GW_BUF_BYTES];
    mb_err_enum_t err = forward(uid, MB_FUNC_READ_DISCRETE_INPUTS, addr, count, tmp);
    if (err == MB_ENOERR) memcpy(buf, tmp, bytes);
    return err;
}

/* The master hands registers over as host-order uint16, the slave frame wants
   them big endian, so neither direction is a memcpy. */
static void regs_to_frame(uint8_t *frame, const uint16_t *regs, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        frame[i * 2]     = (uint8_t)(regs[i] >> 8);
        frame[i * 2 + 1] = (uint8_t)(regs[i] & 0xFF);
    }
}

static void frame_to_regs(uint16_t *regs, const uint8_t *frame, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++)
        regs[i] = (uint16_t)((frame[i * 2] << 8) | frame[i * 2 + 1]);
}

static mb_err_enum_t forward_regs(uint8_t uid, uint8_t *buf, uint16_t addr,
                                  uint16_t count, mb_reg_mode_enum_t mode,
                                  uint8_t read_fc)
{
    if ((size_t)count * 2u > GW_BUF_BYTES)
        return MB_ERR_EXCEPTION(MB_EX_ILLEGAL_DATA_VALUE);

    uint16_t tmp[GW_BUF_BYTES / 2];

    if (mode == MB_REG_READ) {
        mb_err_enum_t err = forward(uid, read_fc, addr, count, tmp);
        if (err == MB_ENOERR) regs_to_frame(buf, tmp, count);
        return err;
    }

    frame_to_regs(tmp, buf, count);
    if (count == 1)                     /* see the coil path for why FC06 */
        return forward(uid, MB_FUNC_WRITE_REGISTER, addr, 1, &tmp[0]);
    return forward(uid, MB_FUNC_WRITE_MULTIPLE_REGISTERS, addr, count, tmp);
}

mb_err_enum_t mb_gateway_holding(uint8_t uid, uint8_t *buf, uint16_t addr,
                                 uint16_t count, mb_reg_mode_enum_t mode)
{
    return forward_regs(uid, buf, addr, count, mode, MB_FUNC_READ_HOLDING_REGISTER);
}

mb_err_enum_t mb_gateway_input(uint8_t uid, uint8_t *buf, uint16_t addr,
                               uint16_t count)
{
    return forward_regs(uid, buf, addr, count, MB_REG_READ,
                        MB_FUNC_READ_INPUT_REGISTER);
}
