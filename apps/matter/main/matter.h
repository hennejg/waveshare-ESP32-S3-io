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
/* Outputs (bit n = DO n+1) changed: reported to the controllers from the
   CHIP task. Any task; outside the output lock. */
void        matter_do_changed(uint8_t mask);
/* Header pad a changed outside Matter (web UI): report its output or PWM
   state to the controllers. Inputs go through matter_di_update(8 + a). */
void        matter_aux_update(uint8_t a);
/* Header pads changed (bit a = pad a): reported from the CHIP task, like
   matter_do_changed(). Any task. */
void        matter_aux_changed(uint8_t mask);
void        matter_decommission(void);
void        matter_set_node_label(const char *name);
const char *matter_get_qr_code(void);
const char *matter_get_manual_code(void);
bool        matter_is_commissioned(void);
/* Provided by the application: called on the CHIP task a few seconds after
   commissioning has completed with a fabric. */
void        matter_on_commissioned(void);

#else /* CONFIG_APP_MATTER_ENABLE not set */

static inline esp_err_t   matter_init(void)                       { return ESP_OK; }
static inline void        matter_di_update(uint8_t c, bool s)     { (void)c; (void)s; }
static inline void        matter_do_update(uint8_t c, bool s)     { (void)c; (void)s; }
static inline void        matter_do_changed(uint8_t m)            { (void)m; }
static inline void        matter_aux_update(uint8_t a)            { (void)a; }
static inline void        matter_aux_changed(uint8_t m)           { (void)m; }
static inline void        matter_decommission(void)               {}
static inline void        matter_set_node_label(const char *n)    { (void)n; }
static inline const char *matter_get_qr_code(void)                { return ""; }
static inline const char *matter_get_manual_code(void)            { return ""; }
static inline bool        matter_is_commissioned(void)            { return false; }

#endif /* CONFIG_APP_MATTER_ENABLE */

#ifdef __cplusplus
}
#endif
