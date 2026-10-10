#include "app_config.h"
#include "aux_io.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "esp_log.h"

#define TAG "app_config"

/* Not an NVS limit -- the configuration is thirteen blobs, the largest of
   them the Modbus master table at 736 bytes. What this actually guards is the
   stack: api_config_post puts a whole app_config_t on the HTTP task's, which
   is 6 kB. Today the struct is about 1.7 kB. */
_Static_assert(sizeof(app_config_t) < 2600,
               "app_config_t too large for the HTTP task stack");

#define NVS_NS "app_config"

/* NVS key names must be <= 15 chars. */
#define K_DEVICE_NAME   "device_name"
#define K_MQTT_URL      "mqtt_url"
#define K_MQTT_USER     "mqtt_user"
#define K_MQTT_PASS     "mqtt_password"
#define K_MQTT_TOPIC    "mqtt_topic"
#define K_DI_CFG        "di_cfg"
#define K_DOUT_CFG      "dout_cfg"
#define K_MODBUS_CFG    "mb_cfg"
#define K_LED_MODE      "led_mode"
#define K_AUX_MODE      "aux_mode"
#define K_AUX_PWM       "aux_pwm"
#define K_AUX_OUT       "aux_out"
#define K_CAN_CFG       "can_cfg"
#define K_SNTP_CFG      "sntp_cfg"
#define K_MBM_CFG       "mbm_cfg"
#define K_TZ            "tz"

static app_config_t s_cfg = {
    .device_name       = "Waveshare-ESP32",
    .mqtt_url          = "",
    .mqtt_user         = "",
    .mqtt_password     = "",
    .mqtt_topic_prefix = "",
    /* di/dout names and invert default to zero */
    .led_mode = LED_MODE_STATUS,   /* status feedback on by default */
    /* Modbus TCP has no authentication of any kind: anything that can reach
       the board can read its inputs and switch its outputs, and through the
       gateway everything on the RS-485 segment as well. It is therefore off
       until someone turns it on, and the RS-485 role stays what it has always
       been. */
    .modbus = { .enable = 0, .address = 1, .baudrate = 9600,
                .rs485_role = MB_ROLE_SLAVE, .tcp_server = 0,
                .tcp_uid = MB_TCP_UID_DEFAULT, .gateway = 0, .cfg_ver = 1,
                .rs485_tout_ms = MB_RS485_TOUT_DEFAULT_MS },
    .can    = { .mode = 0, .n2k_addr = 0x50, .base_id = APP_CFG_CAN_BASE_ID_DEFAULT,
                .bitrate = 250000, .tx_interval_ms = 1000 },
    .sntp   = { .enable = 1, .server = "pool.ntp.org" },
    /* No entries by default: an empty table starts no task and opens no
       connection. */
    .tz     = "",   /* empty = UTC */
    .aux_pwm = { { .freq_hz = 1000 }, { .freq_hz = 1000 }, { .freq_hz = 1000 } },
};

#define NVS_GET_STR(h, key, dst) \
    do { size_t _l = sizeof(dst); nvs_get_str((h), (key), (dst), &_l); } while (0)

/* Pad modes and PWM settings outside their range -- an older firmware's
   blob, a hand-made request -- fall back to the safe value. */
static void aux_normalize(app_config_t *c)
{
    for (int a = 0; a < 3; a++) {
        aux_mode_t m = (aux_mode_t)c->aux_mode[a];
        if (!aux_mode_allowed((uint8_t)a, m)) c->aux_mode[a] = AUX_MODE_STANDARD;
        if (c->aux_pwm[a].freq_hz < AUX_PWM_FREQ_MIN || c->aux_pwm[a].freq_hz > AUX_PWM_FREQ_MAX)
            c->aux_pwm[a].freq_hz = AUX_PWM_FREQ_DEFAULT;
        if (c->aux_pwm[a].duty_pm > AUX_PWM_DUTY_MAX) c->aux_pwm[a].duty_pm = 0;
        c->aux_out[a].name[sizeof(c->aux_out[a].name) - 1] = '\0';
    }
}

/* ---- I/O names as topic fragments (see app_config_io_names_ok) ---- */

#define IO_SPACE_MAX  (APP_CFG_DO_COUNT + 3 > APP_CFG_DI_COUNT ? APP_CFG_DO_COUNT + 3 : APP_CFG_DI_COUNT)

typedef struct {
    char *name[IO_SPACE_MAX];      /* the configured names, in channel order */
    int   count;
    const char *const *reserved;   /* NULL-terminated */
} io_space_t;

static const char *const INPUT_RESERVED[]  = { "read", NULL };
static const char *const OUTPUT_RESERVED[] = { "read", "set", NULL };

/* Inputs: DI1-DI8 and the pads' inputs 9..11. Outputs: DO1-DO8 and the
   pads' outputs 9..11. The cast drops const only for normalisation, which
   is given a mutable configuration. */
static void io_spaces(const app_config_t *c, io_space_t *in, io_space_t *out)
{
    in->count = 0; in->reserved = INPUT_RESERVED;
    for (int i = 0; i < APP_CFG_DI_COUNT; i++) in->name[in->count++] = (char *)c->di[i].name;
    out->count = 0; out->reserved = OUTPUT_RESERVED;
    for (int i = 0; i < APP_CFG_DO_COUNT; i++) out->name[out->count++] = (char *)c->dout[i].name;
    for (int a = 0; a < 3; a++)               out->name[out->count++] = (char *)c->aux_out[a].name;
}

static void io_fragment(const io_space_t *s, int i, char *buf, size_t len)
{
    if (s->name[i][0]) strlcpy(buf, s->name[i], len);
    else snprintf(buf, len, "%d", i + 1);
}

bool app_config_utf8_ok(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        int n; unsigned min;
        if (c < 0x80) continue;
        else if ((c & 0xE0) == 0xC0) { n = 1; min = 0x80;    c &= 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; min = 0x800;   c &= 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; min = 0x10000; c &= 0x07; }
        else return false;
        while (n--) {
            if ((*p & 0xC0) != 0x80) return false;
            c = (c << 6) | (*p++ & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return false;
    }
    return true;
}

/* The problem with channel i's own name, if any. */
static const char *io_name_problem(const io_space_t *s, int i)
{
    const char *n = s->name[i];
    if (!n[0]) return NULL;
    if (strpbrk(n, "/+#")) return "names must not contain '/', '+' or '#'";
    /* The name is an MQTT topic level and a Matter label, both UTF-8 by
       specification: a broker closes the connection over one that is not.
       A name cut at 20 bytes inside a character was stored that way. */
    if (!app_config_utf8_ok(n)) return "names must be valid UTF-8";
    for (int r = 0; s->reserved[r]; r++)
        if (!strcmp(n, s->reserved[r]))
            return s->reserved == OUTPUT_RESERVED ? "'set' and 'read' are reserved output names"
                                                  : "'read' is a reserved input name";
    return NULL;
}

/* Index of a channel before i with the same topic, or -1. */
static int io_clash(const io_space_t *s, int i)
{
    char a[APP_CFG_IO_NAME_MAX + 4], b[APP_CFG_IO_NAME_MAX + 4];
    io_fragment(s, i, a, sizeof(a));
    for (int j = 0; j < s->count; j++) {
        if (j == i) continue;
        io_fragment(s, j, b, sizeof(b));
        if (!strcmp(a, b)) return j;
    }
    return -1;
}

static bool io_space_ok(const io_space_t *s, const char **err_msg)
{
    for (int i = 0; i < s->count; i++) {
        const char *why = io_name_problem(s, i);
        if (why) { *err_msg = why; return false; }
        if (io_clash(s, i) >= 0) {
            *err_msg = s->reserved == OUTPUT_RESERVED
                ? "output names must be unique, including the numbers of unnamed outputs (1-11)"
                : "input names must be unique, including the numbers of unnamed inputs (1-11)";
            return false;
        }
    }
    return true;
}

bool app_config_io_names_ok(const app_config_t *cfg, const char **err_msg)
{
    io_space_t in, out;
    io_spaces(cfg, &in, &out);
    return io_space_ok(&in, err_msg) && io_space_ok(&out, err_msg);
}

/* A stored configuration from before the check can hold such a name. It is
   dropped -- the channel answers to its number again -- rather than left to
   switch every relay. Names in a clash are dropped all together, and again
   until none is left: a dropped name's number can clash in turn. */
static void io_space_normalize(io_space_t *s)
{
    for (int i = 0; i < s->count; i++)
        if (io_name_problem(s, i)) {
            ESP_LOGW(TAG, "I/O name '%s' dropped: %s", s->name[i], io_name_problem(s, i));
            s->name[i][0] = '\0';
        }
    for (int round = 0; round < IO_SPACE_MAX; round++) {
        bool drop[IO_SPACE_MAX] = { false }, any = false;
        for (int i = 0; i < s->count; i++)
            if (s->name[i][0] && io_clash(s, i) >= 0) { drop[i] = true; any = true; }
        if (!any) break;
        for (int i = 0; i < s->count; i++)
            if (drop[i]) {
                ESP_LOGW(TAG, "I/O name '%s' dropped: its topic is another channel's", s->name[i]);
                s->name[i][0] = '\0';
            }
    }
}

static void io_names_normalize(app_config_t *c)
{
    io_space_t in, out;
    io_spaces(c, &in, &out);
    io_space_normalize(&in);
    io_space_normalize(&out);
}

/* A base ID past the 11-bit block, stored before the web check compared
   as a double: 4294967295 wrapped through "+ 5" and was saved as 0xFFFF,
   and the CAN server then sent on IDs folded to 16 bits. Back to the
   default, as a request with a bad value would have left it. */
static void can_normalize(app_config_t *c)
{
    if (c->can.base_id > APP_CFG_CAN_BASE_ID_MAX)
        c->can.base_id = APP_CFG_CAN_BASE_ID_DEFAULT;
}

/* A blob written before a field existed reads it as zero, and a blob written
   by a hand-edited request can hold a value outside its range. Settled here,
   once, after loading: every reader then takes the structure as it is. */
static void modbus_normalize(app_config_t *c)
{
    if (c->modbus.tcp_uid < MB_UID_MIN || c->modbus.tcp_uid > MB_UID_MAX)
        c->modbus.tcp_uid = MB_TCP_UID_DEFAULT;
    if (c->modbus.rs485_tout_ms < MB_RS485_TOUT_MIN_MS ||
        c->modbus.rs485_tout_ms > MB_RS485_TOUT_MAX_MS)
        c->modbus.rs485_tout_ms = MB_RS485_TOUT_DEFAULT_MS;
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
        mbm_poll_t *e = &c->mbm[i];
        if (!e->port)                                      e->port        = MBM_PORT_DEFAULT;
        if (e->unit_id < MB_UID_MIN || e->unit_id > MB_UID_MAX) e->unit_id = MBM_UNIT_ID_DEFAULT;
        if (e->fc != 3 && e->fc != 4)                      e->fc          = MBM_FC_DEFAULT;
        if (!e->interval_ms)                               e->interval_ms = MBM_INTERVAL_DEFAULT_MS;
        if (e->transport > MBM_TRANSPORT_RTU)              e->transport   = MBM_TRANSPORT_TCP;
        /* The one value the web API refuses on the way in: an untouched row
           handed out with scale 0 came straight back as a 400 the moment a
           client enabled it without changing anything else. */
        if (!isfinite(e->scale) || e->scale == 0.0f)       e->scale       = 1.0f;
    }
}

esp_err_t app_config_init(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;

    NVS_GET_STR(h, K_DEVICE_NAME, s_cfg.device_name);
    NVS_GET_STR(h, K_MQTT_URL,    s_cfg.mqtt_url);
    NVS_GET_STR(h, K_MQTT_USER,   s_cfg.mqtt_user);
    NVS_GET_STR(h, K_MQTT_PASS,   s_cfg.mqtt_password);
    NVS_GET_STR(h, K_MQTT_TOPIC,  s_cfg.mqtt_topic_prefix);
    NVS_GET_STR(h, K_TZ,          s_cfg.tz);

    /* DI config blob — silently keep defaults if not found or size changed
       (the latter happens when di_config_t grows beyond its reserved bytes). */
    size_t sz = sizeof(s_cfg.di);
    nvs_get_blob(h, K_DI_CFG, s_cfg.di, &sz);
    sz = sizeof(s_cfg.dout);
    nvs_get_blob(h, K_DOUT_CFG, s_cfg.dout, &sz);
    sz = sizeof(s_cfg.modbus);
    if (nvs_get_blob(h, K_MODBUS_CFG, &s_cfg.modbus, &sz) == ESP_OK && s_cfg.modbus.cfg_ver == 0) {
        /* Stored before the gateway was a setting of its own: a master then
           always forwarded Modbus TCP requests. Such a board keeps doing so;
           the next save stores the new layout. */
        s_cfg.modbus.gateway = (s_cfg.modbus.rs485_role == MB_ROLE_MASTER);
        s_cfg.modbus.cfg_ver = 1;
    }
    uint8_t led_mode_val = 0;
    nvs_get_u8(h, K_LED_MODE, &led_mode_val);
    s_cfg.led_mode = led_mode_val;
    sz = sizeof(s_cfg.aux_mode);
    if (nvs_get_blob(h, K_AUX_MODE, s_cfg.aux_mode, &sz) != ESP_OK) {
        /* No pad modes stored. A firmware built with APP_AUX_DI_ENABLE before
           the pads became switchable had all three as inputs, always, and
           wrote an 11-entry input table; such a board keeps its inputs. */
        size_t di_len = 0;
        if (nvs_get_blob(h, K_DI_CFG, NULL, &di_len) == ESP_OK &&
            di_len == 11 * sizeof(di_config_t))
            for (int a = 0; a < 3; a++) s_cfg.aux_mode[a] = AUX_MODE_INPUT;
    }
    sz = sizeof(s_cfg.aux_pwm);
    nvs_get_blob(h, K_AUX_PWM, s_cfg.aux_pwm, &sz);
    sz = sizeof(s_cfg.aux_out);
    nvs_get_blob(h, K_AUX_OUT, s_cfg.aux_out, &sz);
    sz = sizeof(s_cfg.can);
    nvs_get_blob(h, K_CAN_CFG, &s_cfg.can, &sz);
    sz = sizeof(s_cfg.sntp);
    nvs_get_blob(h, K_SNTP_CFG, &s_cfg.sntp, &sz);
    sz = sizeof(s_cfg.mbm);
    nvs_get_blob(h, K_MBM_CFG, s_cfg.mbm, &sz);

    nvs_close(h);
    modbus_normalize(&s_cfg);
    can_normalize(&s_cfg);
    aux_normalize(&s_cfg);
    io_names_normalize(&s_cfg);
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return &s_cfg;
}

esp_err_t app_config_update(const app_config_t *cfg)
{
    /* Flash first, live state second: a write that fails leaves the device
       running the configuration it still has in flash, and the caller is
       told so, instead of running the new one until the next reboot while
       having answered ok. Every step is chained, because a single
       nvs_set_*() that fails -- a full partition, say -- was swallowed.

       Known limit: the keys below are written one after another and NVS
       has no transaction (nvs_commit() is a no-op), so a failure or a
       power cut part-way leaves the flash with some new and some old
       values for the next boot. Nothing locks the device that way -- the
       network settings and the web UI stay reachable, and the next save
       writes everything again -- which is why the per-key layout, and
       with it the graceful growth of each struct, is kept. The one store
       where a torn write would lock the user out, the password, is a
       single blob for that reason (auth.c). */
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

#define PUT(call) do { if (ret == ESP_OK) ret = (call); } while (0)
    PUT(nvs_set_str(h, K_DEVICE_NAME, cfg->device_name));
    PUT(nvs_set_str(h, K_MQTT_URL,    cfg->mqtt_url));
    PUT(nvs_set_str(h, K_MQTT_USER,   cfg->mqtt_user));
    PUT(nvs_set_str(h, K_MQTT_PASS,   cfg->mqtt_password));
    PUT(nvs_set_str(h, K_MQTT_TOPIC,  cfg->mqtt_topic_prefix));
    PUT(nvs_set_str(h, K_TZ,          cfg->tz));
    PUT(nvs_set_blob(h, K_DI_CFG,     cfg->di,   sizeof(cfg->di)));
    PUT(nvs_set_blob(h, K_DOUT_CFG,   cfg->dout, sizeof(cfg->dout)));
    PUT(nvs_set_blob(h, K_MODBUS_CFG, &cfg->modbus, sizeof(cfg->modbus)));
    PUT(nvs_set_u8(h, K_LED_MODE,     cfg->led_mode));
    PUT(nvs_set_blob(h, K_AUX_MODE,   cfg->aux_mode, sizeof(cfg->aux_mode)));
    PUT(nvs_set_blob(h, K_AUX_PWM,    cfg->aux_pwm,  sizeof(cfg->aux_pwm)));
    PUT(nvs_set_blob(h, K_AUX_OUT,    cfg->aux_out,  sizeof(cfg->aux_out)));
    PUT(nvs_set_blob(h, K_CAN_CFG,    &cfg->can, sizeof(cfg->can)));
    PUT(nvs_set_blob(h, K_SNTP_CFG,   &cfg->sntp, sizeof(cfg->sntp)));
    PUT(nvs_set_blob(h, K_MBM_CFG,    cfg->mbm, sizeof(cfg->mbm)));
    PUT(nvs_commit(h));
#undef PUT
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "storing the configuration failed: %s", esp_err_to_name(ret));
        return ret;
    }

    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    return ESP_OK;
}
