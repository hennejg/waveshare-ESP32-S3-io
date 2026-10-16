#pragma once
/* GET /api/modbus/status, the same in both firmwares: the TCP server's
   counters ("tcp_server") and each poll-table row's last value and error
   ("tcp_master"), added to root. */
#include "cJSON.h"

void mb_status_json(cJSON *root);
