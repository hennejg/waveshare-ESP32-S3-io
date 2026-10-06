#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_CFG_DEVICE_NAME_LEN   32
#define APP_CFG_MQTT_URL_LEN     128
#define APP_CFG_MQTT_USER_LEN     64
#define APP_CFG_MQTT_PASS_LEN     64
#define APP_CFG_MQTT_TOPIC_LEN    64
#define APP_CFG_SNTP_SERVER_LEN   64
#define APP_CFG_TZ_LEN            48   /* POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */
/* DI1-DI8 are optocoupler-isolated; DI9-DI11 (APP_AUX_DI_ENABLE) are bare
   3.3 V GPIOs.  APP_CFG_DI_COUNT stays the total number of inputs. */
#define APP_CFG_DI_ISOLATED_COUNT  8
#ifdef CONFIG_APP_AUX_DI_ENABLE
#define APP_CFG_DI_AUX_COUNT       3
#else
#define APP_CFG_DI_AUX_COUNT       0
#endif
#define APP_CFG_DI_COUNT          (APP_CFG_DI_ISOLATED_COUNT + APP_CFG_DI_AUX_COUNT)
#define APP_CFG_DO_COUNT           8
#define APP_CFG_IO_NAME_MAX        20   /* max visible chars; empty = use index number */

#define LED_MODE_IO     0   /* LED controlled via MQTT and Modbus */
#define LED_MODE_STATUS 1   /* LED shows device connectivity state */

/* What the board does on the RS-485 segment. The two cannot be combined: one
   UART, and a half-duplex segment has exactly one master. */
#define MB_ROLE_SLAVE   0   /* answers requests addressed to it            */
#define MB_ROLE_MASTER  1   /* drives the segment on behalf of TCP clients */

/* The unit ID the board answers to over TCP when nothing else is set. 247 is
   the highest address the protocol allows and is almost never given to a real
   device, so it is unlikely to shadow one on the segment. */
#define MB_TCP_UID_DEFAULT  247

/* How long the master waits for a device on the segment.
   MODBUS over serial line V1.02 §2.4.1 leaves the value to the application
   and calls 1 s to several seconds typical at 9600 bps, so this is the floor
   of that range rather than something faster: the connection workers mean a
   slow device no longer delays anything else, and answering 0x0B for a device
   that would have replied is worse than waiting. The stack adds its own
   cooldown on top (CONFIG_FMB_MASTER_TIMEOUT_COOLDOWN_MS, 150 ms). */
#define MB_RS485_TOUT_DEFAULT_MS  1000
#define MB_RS485_TOUT_MIN_MS        50
#define MB_RS485_TOUT_MAX_MS     10000

/* Per-input / per-output configuration (shared layout for DI and DO).
   Stored as a fixed-size NVS blob.  If the blob size changes the old data is
   silently discarded and defaults apply.
   The array is read with nvs_get_blob() and sizeof() as the in/out length, so
   growing APP_CFG_DI_COUNT is graceful: an 8-entry blob still loads into the
   11-entry array and the new entries keep their defaults.  The downgrade is
   not — 11 entries stored into an 8-entry buffer makes nvs_get_blob() return
   ESP_ERR_NVS_INVALID_LENGTH and every DI name falls back to its default. */
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
    di_config_t  dout[APP_CFG_DO_COUNT];

    uint8_t led_mode;    /* LED_MODE_IO or LED_MODE_STATUS */
    uint8_t _led_pad[3];

    /* CAN bus */
    struct {
        uint8_t  mode;             /* 0=off  1=basic (11-bit)  2=NMEA2000 (29-bit) */
        uint8_t  n2k_addr;         /* NMEA2000 preferred source address (1–251) */
        uint16_t base_id;          /* 11-bit base CAN ID for basic mode */
        uint32_t bitrate;          /* 125000/250000/500000/1000000; N2k forces 250000 */
        uint16_t tx_interval_ms;   /* periodic DI TX interval; 0 = on-change only */
        uint8_t  _pad[2];
    } can;

    /* Modbus.
       The fields after baudrate were added later. They are appended rather
       than inserted on purpose: nvs_get_blob() loads a shorter stored blob
       into the longer struct and leaves the rest at the defaults above, so a
       device that was configured before they existed keeps behaving exactly
       as it did. Going back to a firmware without them is the one direction
       that is not graceful -- the longer blob is refused and every Modbus
       setting falls back to its default, the same as for the DI blob. */
    struct {
        uint8_t  enable;     /* 0 = disabled; changes take effect on reboot */
        uint8_t  address;    /* RTU slave address 1–247 */
        uint8_t  _pad[2];
        uint32_t baudrate;   /* 9600 / 19200 / 38400 / 57600 / 115200 */

        uint8_t  rs485_role; /* MB_ROLE_SLAVE or MB_ROLE_MASTER            */
        uint8_t  tcp_server; /* 1 = answer Modbus TCP on port 502          */
        uint8_t  tcp_uid;    /* unit ID the board answers to over TCP, 1–247 */
        uint8_t  _pad2;
        uint16_t rs485_tout_ms;  /* master: how long a device may take, 50–10000 */
        uint8_t  _pad3[2];
    } modbus;

    /* SNTP time synchronisation */
    struct {
        uint8_t enable;                          /* 1 = enabled (default) */
        uint8_t _pad[3];
        char    server[APP_CFG_SNTP_SERVER_LEN]; /* NTP server hostname */
    } sntp;

    /* POSIX TZ string for local time (cron + UI display). Empty = UTC.
       e.g. "CET-1CEST,M3.5.0,M10.5.0/3" for Europe/Berlin. */
    char tz[APP_CFG_TZ_LEN];
} app_config_t;

esp_err_t           app_config_init(void);
const app_config_t *app_config_get(void);
esp_err_t           app_config_update(const app_config_t *cfg);
