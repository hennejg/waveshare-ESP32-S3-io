#pragma once

/* The poll table: the board reading other people's devices, on the network
 * (Modbus TCP master, CONFIG_MB_TCP_MASTER) or on the RS-485 segment (with
 * RS-485 set to master, CONFIG_MB_RS485_MASTER).
 *
 * Each configured entry names one value -- where it lives and how to read it.
 * What becomes of a value that comes back is the application's: the full
 * firmware publishes it on MQTT under "modbus/<name>" and feeds it to the
 * rule engine, the Matter firmware reports it on a sensor endpoint.
 *
 * Reading only. Writing to somebody else's device is a decision with
 * consequences on the other end, and nothing has asked for it.
 */

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"
#include "esp_err.h"

/* Called on the polling task for every value read, with the entry's index
   in the table and the entry as the task has it. Must not block for long:
   the next entry waits. */
typedef void (*mb_tcp_master_sink_t)(uint8_t index, const mbm_poll_t *e, double value);

/* Creates what start() and reload() share. Call once from app_main(), before
   the network can come up. psram_stack puts the polling task's stack in
   PSRAM: only for a sink that never touches the flash. */
esp_err_t mb_tcp_master_init(mb_tcp_master_sink_t sink, bool psram_stack);

/* True if an enabled, complete entry reads a device on RS-485: such an entry
   needs no network, and the poller is started at boot for it. */
bool mb_tcp_master_has_rtu(void);

/* Starts the polling task if any entry is enabled. Needs a working IP stack,
   so call it once the network is up; calling it again is harmless, from any
   task. */
esp_err_t mb_tcp_master_start(void);

/* Apply a changed configuration. Starts the task if the first entry has just
   been enabled, and forgets what it knew about entries that have been
   repointed. Call after app_config_update(). */
esp_err_t mb_tcp_master_reload(void);

/* What happened to one entry last time, for the status API. */
typedef struct {
    char     name[APP_CFG_MBM_NAME_LEN + 1];  /* as the polling task has it   */
    bool     enabled;
    bool     valid;        /* a value has been read at least once; age_ms says
                              how long ago -- nothing here calls it stale     */
    double   value;        /* already scaled                                 */
    int64_t  age_ms;       /* since that value, -1 if there has never been one */
    uint32_t reads;
    uint32_t errors;
    char     last_error[48];
} mbm_status_t;

/* Fills up to count entries and returns how many were written. */
uint8_t mb_tcp_master_get_status(mbm_status_t *out, uint8_t count);
