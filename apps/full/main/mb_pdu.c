#include "mb_pdu.h"

#include <stdbool.h>
#include <string.h>

/* Modbus framing: taking a request PDU apart and putting a response PDU
   together. Nothing here touches hardware, the network or the Modbus stack,
   which is the point -- it is the part where an off-by-one is both easiest to
   make and hardest to see, so it is kept where it can be tested on its own.
   See test/host for that. */


/* Limits from the specification. A frame that breaks one of them is a client
   error, not a device error, and gets the matching exception rather than
   being passed on to something that would have to guess. */
#define MAX_READ_BITS         2000
#define MAX_READ_REGS         125
#define MAX_WRITE_BITS        1968
#define MAX_WRITE_REGS        123

uint16_t mb_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Takes a PDU apart. Returns 0 and fills req, or the exception to answer
   with. Every length is checked against the function code, so a handler
   never sees a count that does not match the bytes behind it. */
uint8_t mb_parse_pdu(const uint8_t *pdu, uint16_t len, mb_request_t *req)
{
    memset(req, 0, sizeof(*req));
    if (len < 1) return MB_EXC_ILLEGAL_FUNC;   /* req->fc stays 0 for the reply */
    req->fc = pdu[0];

    switch (req->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT: {
        if (len != 5) return MB_EXC_ILLEGAL_VALUE;
        req->addr  = mb_be16(&pdu[1]);
        req->count = mb_be16(&pdu[3]);
        uint16_t max = (req->fc == MB_FC_READ_COILS ||
                        req->fc == MB_FC_READ_DISCRETE)
                       ? MAX_READ_BITS : MAX_READ_REGS;
        if (req->count < 1 || req->count > max) return MB_EXC_ILLEGAL_VALUE;
        return MB_EXC_NONE;
    }

    case MB_FC_WRITE_COIL:
        if (len != 5) return MB_EXC_ILLEGAL_VALUE;
        /* Only all-ones and all-zeros mean anything here. */
        if (!((pdu[3] == 0xFF || pdu[3] == 0x00) && pdu[4] == 0x00))
            return MB_EXC_ILLEGAL_VALUE;
        req->addr = mb_be16(&pdu[1]);
        req->count = 1;
        req->data = &pdu[3];
        req->data_len = 2;
        return MB_EXC_NONE;

    case MB_FC_WRITE_REGISTER:
        if (len != 5) return MB_EXC_ILLEGAL_VALUE;
        req->addr = mb_be16(&pdu[1]);
        req->count = 1;
        req->data = &pdu[3];
        req->data_len = 2;
        return MB_EXC_NONE;

    case MB_FC_WRITE_COILS:
    case MB_FC_WRITE_REGISTERS: {
        if (len < 7) return MB_EXC_ILLEGAL_VALUE;
        req->addr  = mb_be16(&pdu[1]);
        req->count = mb_be16(&pdu[3]);
        uint8_t  byte_cnt = pdu[5];
        bool     bits     = (req->fc == MB_FC_WRITE_COILS);
        uint16_t max      = bits ? MAX_WRITE_BITS : MAX_WRITE_REGS;
        uint16_t want     = bits ? (uint16_t)((req->count + 7u) / 8u)
                                 : (uint16_t)(req->count * 2u);
        if (req->count < 1 || req->count > max) return MB_EXC_ILLEGAL_VALUE;
        if (byte_cnt != want || len != 6u + byte_cnt) return MB_EXC_ILLEGAL_VALUE;
        req->data = &pdu[6];
        req->data_len = byte_cnt;
        return MB_EXC_NONE;
    }

    default:
        return MB_EXC_ILLEGAL_FUNC;
    }
}

/* Builds the response PDU. For a read the handler supplied the data bytes;
   for a write the answer is the request's own first five bytes, which is what
   the specification asks for in every one of the four write cases. */
uint16_t mb_build_response(const mb_request_t *req, const uint8_t *pdu,
                               const uint8_t *data, uint16_t data_len,
                               uint8_t exc, uint8_t *out)
{
    if (exc != MB_EXC_NONE) {
        out[0] = (uint8_t)(req->fc | 0x80u);
        out[1] = exc;
        return 2;
    }

    switch (req->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        out[0] = req->fc;
        out[1] = (uint8_t)data_len;
        memcpy(&out[2], data, data_len);
        return (uint16_t)(2 + data_len);

    default:                      /* FC05, FC06, FC15, FC16 all echo five bytes */
        memcpy(out, pdu, 5);
        return 5;
    }
}
