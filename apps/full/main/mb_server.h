#pragma once
#include "esp_err.h"
#include "mb_request.h"

/* SPIKE. Hard-coded until the configuration model lands. With MB_GW_SPIKE set
   the board serves Modbus TCP and drives RS-485 as a master, instead of
   sitting on the segment as an RTU slave. */
#define MB_GW_SPIKE   1
#define MB_LOCAL_UID  247      /* the unit ID this board answers to over TCP */
#define MB_TCP_PORT   502

/* Initialise the Modbus server if enabled in app_config. Call after di_init(),
   dout_init(), led_init(), buzzer_init(). Configuration changes take effect on
   the next reboot. */
esp_err_t mb_server_init(void);

/* Start the parts that need a working IP stack. Call once the network is up;
   calling it again is harmless. */
esp_err_t mb_server_net_start(void);

/* Answer a request from this board's own I/Os. Safe to call from several
   tasks at once: reads come from cached I/O state behind a short mutex, and
   writes are queued rather than carried out here. */
uint8_t mb_server_handle(const mb_request_t *req, uint8_t *resp, uint16_t *resp_len);
