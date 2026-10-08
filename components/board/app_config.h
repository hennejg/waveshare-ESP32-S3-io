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
#define MB_ROLE_MASTER  1   /* drives the segment: polls devices (Modbus
                               master table) and, with modbus.gateway on,
                               forwards Modbus TCP requests to them        */

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

#define MB_UID_MIN                   1   /* unit ids the protocol allows a device */
#define MB_UID_MAX                 247

/* Defaults of a Modbus TCP master entry. A blob stored by firmware that did
   not know a field reads it as zero; app_config_init() turns every such zero
   into these, so no reader has to. */
#define MBM_PORT_DEFAULT           502
#define MBM_UNIT_ID_DEFAULT          1
#define MBM_FC_DEFAULT               3
#define MBM_INTERVAL_DEFAULT_MS   5000

/* Modbus TCP master: registers this board reads from other people's devices.
   Each entry is one value -- a name, where to get it, and how to read it. */
#define APP_CFG_MBM_COUNT      8
#define APP_CFG_MBM_NAME_LEN  20
#define APP_CFG_MBM_HOST_LEN  48

/* How a register pair is to be read. These are the MB_VAL_* values from
   mb_request.h, repeated here because this header belongs to the board and
   must not depend on the application; mb_tcp_master.c asserts that the two
   still agree. */
#define MBM_VAL_U16   0
#define MBM_VAL_S16   1
#define MBM_VAL_U32   2
#define MBM_VAL_S32   3
#define MBM_VAL_F32   4

#define MBM_INTERVAL_MIN_MS   200

/* Where a polled device is: on the network (host, port) or on the RS-485
   segment, which needs the RS-485 side set to master. */
#define MBM_TRANSPORT_TCP     0
#define MBM_TRANSPORT_RTU     1
#define MBM_INTERVAL_MAX_MS 86400000   /* a day */

typedef struct {
    uint8_t  enable;
    uint8_t  unit_id;      /* 1–247                                          */
    uint8_t  fc;           /* 3 = holding register, 4 = input register       */
    uint8_t  type;         /* MBM_VAL_*                                      */
    uint8_t  word_swap;    /* 1 = the two registers of a 32-bit value swapped */
    uint8_t  transport;    /* MBM_TRANSPORT_*; was padding, so 0 = TCP       */
    uint16_t reg;          /* zero-based, as on the wire                     */
    uint16_t port;         /* usually 502                                    */
    uint16_t _pad2;
    uint32_t interval_ms;
    float    scale;        /* the raw value is multiplied by this            */
    char     name[APP_CFG_MBM_NAME_LEN + 1];  /* MQTT topic and rule key     */
    char     host[APP_CFG_MBM_HOST_LEN + 1];  /* address or hostname         */
    uint8_t  _pad3[2];
} mbm_poll_t;

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
    uint8_t aux_mode[3]; /* IO1/RXD/TXD pads: aux_mode_t, see aux_io.h
                            (APP_AUX_DI_ENABLE); 0 = standard function */

    /* CAN bus */
    /* Basic mode sends and matches base_id .. base_id + 5, all of it in the
       11-bit standard range. */
#define APP_CFG_CAN_BASE_ID_DEFAULT 0x100
#define APP_CFG_CAN_BASE_ID_MAX     (0x7FF - 5)
    struct {
        uint8_t  mode;             /* 0=off  1=basic (11-bit)  2=NMEA2000 (29-bit) */
        uint8_t  n2k_addr;         /* NMEA2000 preferred source address (1–251) */
        uint16_t base_id;          /* 11-bit base CAN ID for basic mode, <= APP_CFG_CAN_BASE_ID_MAX */
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
        uint8_t  gateway;    /* master: forward other TCP unit IDs to RS-485 */
        uint16_t rs485_tout_ms;  /* master: how long a device may take, 50–10000 */
        uint8_t  cfg_ver;    /* layout of this block, see app_config_init() */
        uint8_t  _pad3;
    } modbus;

    /* Modbus TCP master. Stored as its own blob, so it grows and shrinks
       without touching anything else. */
    mbm_poll_t mbm[APP_CFG_MBM_COUNT];

    /* SNTP time synchronisation */
    struct {
        uint8_t enable;                          /* 1 = enabled (default) */
        uint8_t _pad[3];
        char    server[APP_CFG_SNTP_SERVER_LEN]; /* NTP server hostname */
    } sntp;

    /* POSIX TZ string for local time (cron + UI display). Empty = UTC.
       e.g. "CET-1CEST,M3.5.0,M10.5.0/3" for Europe/Berlin. */
    char tz[APP_CFG_TZ_LEN];

    /* Name and invert of the IO1/RXD/TXD pads as outputs (aux_mode output),
       apart from their input entries in di[8..10]: a pad is one or the other,
       and its output name is an MQTT topic among DO1-DO8's. */
    di_config_t aux_out[3];

    /* PWM of the IO1/RXD/TXD pads, used while aux_mode is pwm. */
    struct {
        uint32_t freq_hz;   /* AUX_PWM_FREQ_MIN..AUX_PWM_FREQ_MAX */
        uint16_t duty_pm;   /* tenths of a percent, 0..1000       */
        uint16_t _pad;
    } aux_pwm[3];
} app_config_t;

esp_err_t           app_config_init(void);
const app_config_t *app_config_get(void);
esp_err_t           app_config_update(const app_config_t *cfg);

/* The I/O names are MQTT topic fragments: input/<name>, output/<name> and
   output/<name>/set, a channel without a name using its number instead
   (inputs 1..11, outputs 1..8 and the pads 9..11). Checked on what the
   topics come out as, numbers included:
     - no '/', '+' or '#';
     - not a command topic of the same space -- an output called "set"
       published its state to output/set, the bulk command, and switched
       every relay; "read" made each publish trigger the next;
     - unique within the space: DO1 called "2" and an unnamed DO2 both
       listened on output/2/set, and the handler only ever switched DO1.
   False with a message for the first problem. */
bool                app_config_io_names_ok(const app_config_t *cfg, const char **err_msg);
