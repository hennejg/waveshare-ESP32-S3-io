#pragma once
#include <stdint.h>
#include "esp_err.h"

#define APP_CFG_DEVICE_NAME_LEN 32

/* Add new fields here and mirror them in app_config.c (nvs_load/nvs_save/json). */
typedef struct {
    char device_name[APP_CFG_DEVICE_NAME_LEN];
} app_config_t;

esp_err_t        app_config_init(void);
const app_config_t *app_config_get(void);
esp_err_t        app_config_update(const app_config_t *cfg);
