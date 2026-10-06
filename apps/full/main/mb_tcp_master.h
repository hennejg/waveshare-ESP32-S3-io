#pragma once

/* Modbus TCP master: the board reading other people's devices.
 *
 * Each configured entry names one value -- where it lives and how to read it.
 * A value that comes back is published on MQTT under "modbus/<name>" and fed
 * to the rule engine under the same name, so a rule can act on it with
 * mqtt("modbus/<name>") whether or not a broker is connected.
 *
 * Reading only. Writing to somebody else's device is a decision with
 * consequences on the other end, and nothing has asked for it.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Starts the polling task if any entry is enabled. Needs a working IP stack,
   so call it once the network is up; calling it again is harmless. */
esp_err_t mb_tcp_master_start(void);

/* Apply a changed configuration. Starts the task if the first entry has just
   been enabled, and forgets what it knew about entries that have been
   repointed. Call after app_config_update(). */
esp_err_t mb_tcp_master_reload(void);

/* What happened to one entry last time, for the status API. */
typedef struct {
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
