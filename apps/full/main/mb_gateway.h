#pragma once

/* Modbus TCP -> RTU gateway.
 *
 * The board answers Modbus TCP for two different things at once. Requests
 * carrying the local unit ID are its own I/Os and are served by mb_server.c.
 * Every other unit ID belongs to a device on the RS-485 segment: this module
 * owns the RTU master that talks to it and turns the TCP request into an RTU
 * transaction and the answer back into a TCP response.
 *
 * The addresses here are zero-based, as they are on the wire -- not the +1
 * form the stack hands to a register callback.
 */

#include <stdbool.h>
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "mbcontroller.h"

/* Creates and starts the RTU master on the given UART. The caller owns the
   decision that the port is free: the RTU slave and this master cannot both
   have it. */
esp_err_t mb_gateway_start(uart_port_t uart, uint32_t baudrate,
                           int tx_gpio, int rx_gpio, int rts_gpio);

bool mb_gateway_is_running(void);

/* One forwarded request each. Every one of them blocks the calling task until
   the device answers or the master's response timeout expires, and returns
   either MB_ENOERR with buf filled, or MB_ERR_EXCEPTION(code) carrying the
   code that belongs on the wire -- the device's own exception where it sent
   one, 0x0B where it stayed silent. */
mb_err_enum_t mb_gateway_coils(uint8_t uid, uint8_t *buf, uint16_t addr,
                               uint16_t count, mb_reg_mode_enum_t mode);
mb_err_enum_t mb_gateway_discrete(uint8_t uid, uint8_t *buf, uint16_t addr,
                                  uint16_t count);
mb_err_enum_t mb_gateway_holding(uint8_t uid, uint8_t *buf, uint16_t addr,
                                 uint16_t count, mb_reg_mode_enum_t mode);
mb_err_enum_t mb_gateway_input(uint8_t uid, uint8_t *buf, uint16_t addr,
                               uint16_t count);
