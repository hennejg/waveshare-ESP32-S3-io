#pragma once
#include "esp_err.h"
#include <stdint.h>

/* Modbus TCP server. Needs a working IP stack, so call it once the network is
   up; calling it again is harmless. */
esp_err_t mb_tcp_server_start(uint16_t port);

/* Counters, for the status API and for telling a busy segment from a broken
   one without a packet capture. */
typedef struct {
    uint32_t accepted;      /* connections taken                            */
    uint32_t refused;       /* connections dropped, no slot free            */
    uint32_t requests;      /* requests answered, exceptions included       */
    uint32_t exceptions;    /* of those, answered with an exception         */
    uint32_t forwarded;     /* of those, sent to the RS-485 segment         */
    uint32_t overloaded;    /* requests turned away, every worker occupied  */
    uint32_t malformed;     /* frames dropped, connection closed            */
} mb_tcp_stats_t;

void mb_tcp_server_get_stats(mb_tcp_stats_t *out);
