#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_APP_MATTER_ENABLE

esp_err_t matter_init(void);
void matter_di_update(uint8_t channel, bool active);
void matter_do_update(uint8_t channel, bool state);

#else /* CONFIG_APP_MATTER_ENABLE not set */

static inline esp_err_t matter_init(void)                       { return ESP_OK; }
static inline void matter_di_update(uint8_t c, bool s)          { (void)c; (void)s; }
static inline void matter_do_update(uint8_t c, bool s)          { (void)c; (void)s; }

#endif /* CONFIG_APP_MATTER_ENABLE */

#ifdef __cplusplus
}
#endif
