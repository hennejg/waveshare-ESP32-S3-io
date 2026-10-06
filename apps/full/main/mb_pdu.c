#include "mb_pdu.h"

#include <stdbool.h>
#include <string.h>

/* Modbus on the wire: taking a request PDU apart, putting a response PDU
   together, and reading a value out of a register pair. Nothing here touches
   hardware, the network or the Modbus stack, which is the point -- these are
   the parts where an off-by-one or a swapped word is both easiest to make and
   hardest to see, so they are kept where they can be tested on their own.
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

/* ------------------------------------------------------ reading a value out */

uint16_t mb_value_regs(uint8_t type)
{
    switch (type) {
    case MB_VAL_U16:
    case MB_VAL_S16: return 1;
    case MB_VAL_U32:
    case MB_VAL_S32:
    case MB_VAL_F32: return 2;
    default:         return 0;
    }
}

bool mb_value_decode(const uint8_t *regs, uint8_t type, bool word_swap, double *out)
{
    if (!regs || !out) return false;

    uint16_t w0 = mb_be16(&regs[0]);
    if (type == MB_VAL_U16) { *out = (double)w0; return true; }
    if (type == MB_VAL_S16) { *out = (double)(int16_t)w0; return true; }
    if (mb_value_regs(type) != 2) return false;

    uint16_t w1 = mb_be16(&regs[2]);
    /* Word swapped means the two registers arrive the other way round; the
       bytes inside each register are big endian either way. */
    uint32_t v = word_swap ? ((uint32_t)w1 << 16) | w0
                           : ((uint32_t)w0 << 16) | w1;

    switch (type) {
    case MB_VAL_U32: *out = (double)v;              return true;
    case MB_VAL_S32: *out = (double)(int32_t)v;     return true;
    case MB_VAL_F32: {
        /* Through memcpy rather than a cast: the two have different alignment
           requirements and punning through a pointer is undefined. */
        float f;
        memcpy(&f, &v, sizeof(f));
        *out = (double)f;
        return true;
    }
    default: return false;
    }
}

/* ------------------------------------------------- the other side of the wire */

uint16_t mb_build_read_request(uint8_t *out, uint16_t tid, uint8_t uid,
                               uint8_t fc, uint16_t reg, uint16_t count)
{
    out[0] = (uint8_t)(tid >> 8);  out[1] = (uint8_t)tid;
    out[2] = 0;                    out[3] = 0;        /* protocol id */
    out[4] = 0;                    out[5] = 6;        /* unit id plus five PDU bytes */
    out[6] = uid;
    out[7] = fc;
    out[8] = (uint8_t)(reg >> 8);   out[9]  = (uint8_t)reg;
    out[10] = (uint8_t)(count >> 8); out[11] = (uint8_t)count;
    return MB_MBAP_LEN + 5;
}

mb_rsp_t mb_parse_read_response(const uint8_t *frame, uint16_t len,
                                uint16_t tid, uint8_t uid, uint8_t fc,
                                uint16_t count, const uint8_t **data, uint8_t *exc)
{
    if (!frame || !data || !exc) return MB_RSP_MALFORMED;
    *data = NULL;
    *exc  = MB_EXC_NONE;

    if (len < MB_MBAP_LEN + 2) return MB_RSP_MALFORMED;
    if (mb_be16(&frame[2]) != 0) return MB_RSP_MALFORMED;      /* protocol id */

    /* The MBAP length counts the unit id and the PDU, and has to agree with
       how many bytes actually arrived. */
    uint16_t mbap_len = mb_be16(&frame[4]);
    if (mbap_len < 2 || (uint32_t)mbap_len + MB_MBAP_LEN - 1 != len) return MB_RSP_MALFORMED;

    if (mb_be16(&frame[0]) != tid) return MB_RSP_MALFORMED;    /* a stale answer */
    if (frame[6] != uid)           return MB_RSP_MALFORMED;

    uint8_t rsp_fc = frame[7];
    if (rsp_fc == (uint8_t)(fc | 0x80u)) {
        if (len != MB_MBAP_LEN + 2) return MB_RSP_MALFORMED;
        *exc = frame[8];
        return MB_RSP_EXCEPTION;
    }
    if (rsp_fc != fc) return MB_RSP_MALFORMED;

    uint8_t byte_cnt = frame[8];
    if (byte_cnt != (uint8_t)(count * 2u)) return MB_RSP_MALFORMED;
    if (len != MB_MBAP_LEN + 2 + byte_cnt) return MB_RSP_MALFORMED;

    *data = &frame[9];
    return MB_RSP_OK;
}
