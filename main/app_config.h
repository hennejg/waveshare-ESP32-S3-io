#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_CFG_DEVICE_NAME_LEN   32
#define APP_CFG_MQTT_URL_LEN     128
#define APP_CFG_MQTT_USER_LEN     64
#define APP_CFG_MQTT_PASS_LEN     64
#define APP_CFG_MQTT_TOPIC_LEN    64
#define APP_CFG_DI_COUNT           8
#define APP_CFG_DO_COUNT           8
#define APP_CFG_IO_NAME_MAX        20   /* max visible chars; empty = use index number */

/* Per-input / per-output configuration (shared layout for DI and DO).
   Stored as a fixed-size NVS blob.  If the blob size changes the old data is
   silently discarded and defaults apply. */
typedef struct {
    bool    invert;                        /* true = invert logical state */
    char    name[APP_CFG_IO_NAME_MAX + 1]; /* MQTT topic fragment, no '/', empty = "1".."8" */
    uint8_t _reserved[2];                  /* pad to 24 bytes */
} di_config_t;

/* To add a scalar field: extend this struct, then mirror the change in
   app_config.c (nvs_load / nvs_save) and web_server.c (json GET / POST). */
typedef struct {
    char         device_name[APP_CFG_DEVICE_NAME_LEN];
    char         mqtt_url[APP_CFG_MQTT_URL_LEN];
    char         mqtt_user[APP_CFG_MQTT_USER_LEN];
    char         mqtt_password[APP_CFG_MQTT_PASS_LEN];
    char         mqtt_topic_prefix[APP_CFG_MQTT_TOPIC_LEN];
    di_config_t  di[APP_CFG_DI_COUNT];

    /* Per-output configuration — same layout as di_config_t for consistency. */
    di_config_t  dout[APP_CFG_DO_COUNT];
} app_config_t;

esp_err_t           app_config_init(void);
const app_config_t *app_config_get(void);
esp_err_t           app_config_update(const app_config_t *cfg);
