#pragma once

/* The RS-485 half of the Modbus TCP gateway.
 *
 * Requests that are not for this board belong to a device on the segment.
 * This module owns the RTU master that talks to it, turns a parsed request
 * into an RTU transaction and the answer back into response data.
 */

#include <stdbool.h>
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "mb_request.h"

/* Creates and starts the RTU master on the given UART. The caller owns the
   decision that the port is free: the RTU slave and this master cannot both
   have it. */
esp_err_t mb_gateway_start(uart_port_t uart, uint32_t baudrate,
                           int tx_gpio, int rx_gpio, int rts_gpio);

bool mb_gateway_is_running(void);

/* Forwards one request to uid and waits for the answer. Safe to call from
   several tasks at once -- the segment carries one transaction at a time, so
   they are serialised here, each waiting its turn.

   Returns 0 with resp filled, or the exception code that belongs on the wire:
   the device's own where it sent one, 0x0B where it stayed silent, 0x0A where
   the gateway could not even try. */
uint8_t mb_gateway_handle(uint8_t uid, const mb_request_t *req,
                          uint8_t *resp, uint16_t *resp_len);
