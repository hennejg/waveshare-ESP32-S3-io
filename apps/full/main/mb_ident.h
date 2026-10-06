#pragma once

/* What the board says about itself over Modbus: a block of read-only input
   registers (FC 04) and the Read Device Identification objects (FC 43/14).
   Register layout and object list are documented in docs/modbus.md; the
   layout constants here are the single place the code takes them from. */

#include <stdint.h>
#include "mb_request.h"

/* Input registers, zero based. Numbers first, text behind, text as two
   characters per register with the first in the high byte, NUL padded. The
   whole block fits one FC 04 request. */
#define MB_IDENT_REG_DEVICE_TYPE     0    /* MB_IDENT_TYPE_* */
#define MB_IDENT_REG_FW_MAJOR        1
#define MB_IDENT_REG_FW_MINOR        2
#define MB_IDENT_REG_FW_PATCH        3
#define MB_IDENT_REG_MAC             4    /* 3 registers, six bytes: the base MAC */
#define MB_IDENT_REG_SERIAL          7    /* u32: the last four bytes of the MAC */
#define MB_IDENT_REG_UPTIME          9    /* u32 seconds */
#define MB_IDENT_REG_IP             11    /* 2 registers, four bytes in order */
#define MB_IDENT_REG_NETMASK        13
#define MB_IDENT_REG_GATEWAY        15
#define MB_IDENT_REG_DHCP           17    /* 1 = address from DHCP */
/* 18..19 reserved, read as zero */
#define MB_IDENT_REG_MODEL          20    /* 20 registers */
#define MB_IDENT_REG_DEVICE_NAME    40    /* 16 registers */
#define MB_IDENT_REG_SERIAL_STR     56    /*  8 registers */
#define MB_IDENT_REG_FW_VERSION     64    /* 16 registers */
#define MB_IDENT_REG_COUNT          80

#define MB_IDENT_TYPE_8DI_8DO        1    /* Waveshare-ESP32-S3-POE-ETH-8DI-8DO */

/* Reads the fixed part once. Call before the Modbus server starts. */
void mb_ident_init(void);

/* FC 04 for the local unit: count registers from addr into out, big endian.
   Returns 0 or the exception code. */
uint8_t mb_ident_read_input(uint16_t addr, uint16_t count, uint8_t *out);

/* FC 43/14 for the local unit: the response data after the function code.
   Same contract as a read handler. */
uint8_t mb_ident_device_id(const mb_request_t *req, uint8_t *resp, uint16_t *resp_len);
