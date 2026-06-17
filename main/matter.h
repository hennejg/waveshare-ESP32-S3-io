#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t matter_init(void);
void matter_di_update(uint8_t channel, bool active);
void matter_do_update(uint8_t channel, bool state);

#ifdef __cplusplus
}
#endif
