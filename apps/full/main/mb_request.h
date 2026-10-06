#pragma once

/* One Modbus request, already taken apart.
 *
 * The framing differs between TCP and RTU, and the two things that can answer
 * a request -- this board's own I/Os and a device on the RS-485 segment --
 * have nothing else in common. Parsing once and handing the pieces to whoever
 * answers keeps the function-code arithmetic in a single place.
 *
 * Addresses are zero-based, as they are on the wire.
 */

#include <stdint.h>

typedef struct {
    uint8_t        fc;        /* function code                                 */
    uint16_t       addr;      /* first coil or register, zero-based            */
    uint16_t       count;     /* how many; 1 for FC05 and FC06                 */
    const uint8_t *data;      /* write payload: packed bits, or big-endian
                                 registers, or the two value bytes of FC05/06  */
    uint16_t       data_len;
} mb_request_t;

/* What a handler returns: 0 for success, otherwise the Modbus exception code
   to put on the wire. On a read it fills resp with the data bytes alone --
   packed bits or big-endian registers -- and sets *resp_len to their count;
   the function code and byte count are the framing's business. A write
   produces no data, and its echo is built from the request. */
typedef uint8_t (*mb_handler_fn)(const mb_request_t *req,
                                 uint8_t *resp, uint16_t *resp_len);

/* Modbus exception codes, named where the stack's own enum is not in scope. */
#define MB_EXC_NONE             0x00
#define MB_EXC_ILLEGAL_FUNC     0x01
#define MB_EXC_ILLEGAL_ADDR     0x02
#define MB_EXC_ILLEGAL_VALUE    0x03
#define MB_EXC_DEVICE_FAILURE   0x04
#define MB_EXC_DEVICE_BUSY      0x06
#define MB_EXC_GW_PATH          0x0A   /* no route to the target            */
#define MB_EXC_GW_TARGET        0x0B   /* target did not answer             */

/* Function codes. Numerically the same as the component's own enumerators,
   under names of their own so the two can be included side by side -- and so
   that the framing can be built and tested without the component at all. */
#define MB_FC_READ_COILS        0x01
#define MB_FC_READ_DISCRETE     0x02
#define MB_FC_READ_HOLDING      0x03
#define MB_FC_READ_INPUT        0x04
#define MB_FC_WRITE_COIL        0x05
#define MB_FC_WRITE_REGISTER    0x06
#define MB_FC_WRITE_COILS       0x0F
#define MB_FC_WRITE_REGISTERS   0x10

/* How a register pair is to be read. Meters disagree about this more than
   about anything else in the protocol, so it is per value, not per device.
   A 32-bit quantity occupies two registers; "word swapped" is the CDAB order
   some devices use, where the two registers are the other way round while the
   bytes inside each stay big endian. */
#define MB_VAL_U16   0
#define MB_VAL_S16   1
#define MB_VAL_U32   2
#define MB_VAL_S32   3
#define MB_VAL_F32   4
#define MB_VAL_COUNT 5

/* The largest PDU the protocol allows, and the largest data part inside one. */
#define MB_PDU_MAX              253
#define MB_DATA_MAX             250
