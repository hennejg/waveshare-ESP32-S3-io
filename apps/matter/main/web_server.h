#pragma once
#include "esp_err.h"

esp_err_t web_server_start(void);
void      web_server_stop(void);

/* Provided by main.c: the poll table was saved changed. */
void      app_modbus_table_changed(void);
