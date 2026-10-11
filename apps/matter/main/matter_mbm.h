#pragma once
/* The poll table's values as Matter sensor endpoints -- see matter_mbm.cpp. */
#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "app_config.h"
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <esp_matter.h>
/* Makes an endpoint for each row that names a Matter kind. Before
   esp_matter::start(): the endpoint set is fixed from then on. */
esp_err_t matter_mbm_create(esp_matter::node_t *node);
extern "C" {
#endif

/* A value the poll table read for row index, whose kind (MBM_MATTER_*) is
   as the polling task has it. Any task; reported on the CHIP task. */
void matter_mbm_update(uint8_t index, uint8_t kind, double value);

/* True if the two configurations give different sensor endpoints: a save
   that does needs a reboot to reach Matter. */
bool matter_mbm_endpoints_differ(const app_config_t *a, const app_config_t *b);

#ifdef __cplusplus
}
#endif
