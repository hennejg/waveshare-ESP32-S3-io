#pragma once
#include "esp_err.h"

esp_err_t web_server_start(void);
void      web_server_stop(void);

/* The stored rule script, NUL-terminated, in a buffer the caller frees.
   ESP_ERR_NVS_NOT_FOUND when none is stored (the demo runs); any other
   error means one is stored and could not be read. */
esp_err_t rules_nvs_read(char **out);
