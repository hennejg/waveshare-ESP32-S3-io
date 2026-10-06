#pragma once

/* Modbus PDU framing, free of any dependency on the Modbus stack, the network
   or the board -- see mb_pdu.c. */

#include <stdint.h>
#include "mb_request.h"

uint16_t mb_be16(const uint8_t *p);

/* Takes a request PDU apart. Returns 0 and fills req, or the exception code
   to answer with. Every length is checked against the function code, so a
   handler never sees a count that does not match the bytes behind it.
   req->data points into pdu and lives exactly as long as it does. */
uint8_t mb_parse_pdu(const uint8_t *pdu, uint16_t len, mb_request_t *req);

/* Builds the response PDU into out, which needs room for 2 + MB_DATA_MAX
   bytes, and returns its length. data/data_len are what a read handler
   produced and are ignored for a write or an exception. */
uint16_t mb_build_response(const mb_request_t *req, const uint8_t *pdu,
                           const uint8_t *data, uint16_t data_len,
                           uint8_t exc, uint8_t *out);
