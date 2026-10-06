#pragma once

/* Modbus PDU framing, free of any dependency on the Modbus stack, the network
   or the board -- see mb_pdu.c. */

#include <stdbool.h>
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

/* ------------------------------------------------------ reading a value out

   How many registers one of the MB_VAL_* types occupies, and how to turn
   those registers into a number. regs points at the register bytes as they
   came off the wire, big endian. Returns false for a type that does not
   exist. */
uint16_t mb_value_regs(uint8_t type);
bool     mb_value_decode(const uint8_t *regs, uint8_t type, bool word_swap,
                         double *out);

/* ------------------------------------------------- the other side of the wire

   Asking a Modbus TCP device for registers, and checking what comes back. The
   board needs this to read other people's meters; it is the mirror image of
   the two functions above and just as free of dependencies. */

#define MB_MBAP_LEN  7      /* transaction, protocol, length, unit */

/* Writes a complete read request -- MBAP header and PDU -- into out, which
   needs MB_MBAP_LEN + 5 bytes, and returns its length. */
uint16_t mb_build_read_request(uint8_t *out, uint16_t tid, uint8_t uid,
                               uint8_t fc, uint16_t reg, uint16_t count);

/* What came back. */
typedef enum {
    MB_RSP_OK = 0,        /* data points at count*2 bytes inside frame */
    MB_RSP_EXCEPTION,     /* the device refused; *exc holds its code    */
    MB_RSP_MALFORMED,     /* not an answer to this request              */
} mb_rsp_t;

/* Checks a response against the request it should answer: protocol id, length,
   transaction id, unit id, function code, and the byte count against the
   registers that were asked for. Nothing is taken on trust, because a stale
   answer from a previous request is the one failure that silently produces
   plausible wrong numbers. */
mb_rsp_t mb_parse_read_response(const uint8_t *frame, uint16_t len,
                                uint16_t tid, uint8_t uid, uint8_t fc,
                                uint16_t count, const uint8_t **data, uint8_t *exc);
