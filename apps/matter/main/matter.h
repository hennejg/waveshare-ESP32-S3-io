#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_APP_MATTER_ENABLE

esp_err_t   matter_init(void);
void        matter_di_update(uint8_t channel, bool active);
void        matter_do_update(uint8_t channel, bool state);
/* Header pad a changed outside Matter (web UI): report its output or PWM
   state to the controllers. Inputs go through matter_di_update(8 + a). */
void        matter_aux_update(uint8_t a);
void        matter_decommission(void);
void        matter_set_node_label(const char *name);
const char *matter_get_qr_code(void);
const char *matter_get_manual_code(void);
bool        matter_is_commissioned(void);

#else /* CONFIG_APP_MATTER_ENABLE not set */

static inline esp_err_t   matter_init(void)                       { return ESP_OK; }
static inline void        matter_di_update(uint8_t c, bool s)     { (void)c; (void)s; }
static inline void        matter_do_update(uint8_t c, bool s)     { (void)c; (void)s; }
static inline void        matter_aux_update(uint8_t a)            { (void)a; }
static inline void        matter_decommission(void)               {}
static inline void        matter_set_node_label(const char *n)    { (void)n; }
static inline const char *matter_get_qr_code(void)                { return ""; }
static inline const char *matter_get_manual_code(void)            { return ""; }
static inline bool        matter_is_commissioned(void)            { return false; }

#endif /* CONFIG_APP_MATTER_ENABLE */

#ifdef __cplusplus
}
#endif
