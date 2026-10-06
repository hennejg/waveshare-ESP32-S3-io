#include "mb_ident.h"
#include "mb_pdu.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/inet.h"

/* The identification block. Everything a client may want to know before it
   trusts the registers behind it: what the device is, which one of several
   it is, which firmware it runs, and how it is reachable. The fixed strings
   are built once; the live numbers (uptime, addresses) are read at the
   time of the request, so a read is always current and costs nothing while
   nobody asks.

   Serial number: the ESP32-S3 has no serial number of its own, so the base
   MAC from the eFuse stands in for it. It is unique per chip and survives
   every flash. The numeric form takes its last four bytes, which are the
   ones that differ between chips of the same batch. */

#define VENDOR_NAME  "Waveshare"
#define PRODUCT_NAME "Waveshare-ESP32-S3-POE-ETH-8DI-8DO"
#define MODEL_NAME   "ESP32-S3-POE-ETH-8DI-8DO"
#define VENDOR_URL   "https://github.com/hennejg/waveshare-ESP32-S3-io"

static uint8_t  s_mac[6];
static char     s_serial[16];             /* 12 hex digits */
static char     s_mac_str[18];
static char     s_version[33];
static uint16_t s_fw[3];

static void mac_str(char *dst, size_t n, const uint8_t *m)
{
    snprintf(dst, n, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void mb_ident_init(void)
{
    if (esp_read_mac(s_mac, ESP_MAC_BASE) != ESP_OK) memset(s_mac, 0, 6);
    mac_str(s_mac_str, sizeof(s_mac_str), s_mac);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(s_version, sizeof(s_version), "%s", d->version);

    /* "v1.2.3-4-gabcdef" or "1.2.3": the three numbers in front are the
       release; anything behind them is git's description of the distance
       from it and goes into the string form only. */
    unsigned a = 0, b = 0, c = 0;
    const char *v = d->version;
    if (*v == 'v' || *v == 'V') v++;
    if (sscanf(v, "%u.%u.%u", &a, &b, &c) < 2) a = b = c = 0;
    s_fw[0] = (uint16_t)a; s_fw[1] = (uint16_t)b; s_fw[2] = (uint16_t)c;
}

/* ------------------------------------------------------------ registers */

static void put16(uint8_t *regs, uint16_t idx, uint16_t v)
{
    regs[idx * 2] = (uint8_t)(v >> 8); regs[idx * 2 + 1] = (uint8_t)v;
}
static void put32(uint8_t *regs, uint16_t idx, uint32_t v)
{
    put16(regs, idx, (uint16_t)(v >> 16)); put16(regs, (uint16_t)(idx + 1), (uint16_t)v);
}
static void put_bytes(uint8_t *regs, uint16_t idx, const uint8_t *b, size_t n)
{
    memcpy(&regs[idx * 2], b, n);
}

/* Builds the whole block, 160 bytes, on the stack of whichever task asks;
   a read then copies the slice it wants. Simpler than filling only the
   requested slice, and a client reads this perhaps once a day. */
static void build(uint8_t *regs)
{
    memset(regs, 0, MB_IDENT_REG_COUNT * 2);
    const app_config_t *cfg = app_config_get();

    put16(regs, MB_IDENT_REG_DEVICE_TYPE, MB_IDENT_TYPE_8DI_8DO);
    put16(regs, MB_IDENT_REG_FW_MAJOR, s_fw[0]);
    put16(regs, MB_IDENT_REG_FW_MINOR, s_fw[1]);
    put16(regs, MB_IDENT_REG_FW_PATCH, s_fw[2]);
    put_bytes(regs, MB_IDENT_REG_MAC, s_mac, 6);
    put32(regs, MB_IDENT_REG_SERIAL,
          ((uint32_t)s_mac[2] << 24) | ((uint32_t)s_mac[3] << 16) |
          ((uint32_t)s_mac[4] << 8)  |  (uint32_t)s_mac[5]);
    put32(regs, MB_IDENT_REG_UPTIME, (uint32_t)(esp_timer_get_time() / 1000000LL));

    esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t ip = {0};
    if (eth && esp_netif_get_ip_info(eth, &ip) == ESP_OK) {
        put32(regs, MB_IDENT_REG_IP,      ntohl(ip.ip.addr));
        put32(regs, MB_IDENT_REG_NETMASK, ntohl(ip.netmask.addr));
        put32(regs, MB_IDENT_REG_GATEWAY, ntohl(ip.gw.addr));
    }
    esp_netif_dhcp_status_t dhcp = ESP_NETIF_DHCP_INIT;
    if (eth && esp_netif_dhcpc_get_status(eth, &dhcp) == ESP_OK)
        put16(regs, MB_IDENT_REG_DHCP, dhcp == ESP_NETIF_DHCP_STARTED ? 1 : 0);

    mb_ascii_to_regs(PRODUCT_NAME,     20, &regs[MB_IDENT_REG_MODEL * 2]);
    mb_ascii_to_regs(cfg->device_name, 16, &regs[MB_IDENT_REG_DEVICE_NAME * 2]);
    mb_ascii_to_regs(s_serial,          8, &regs[MB_IDENT_REG_SERIAL_STR * 2]);
    mb_ascii_to_regs(s_version,        16, &regs[MB_IDENT_REG_FW_VERSION * 2]);
}

uint8_t mb_ident_read_input(uint16_t addr, uint16_t count, uint8_t *out)
{
    if ((uint32_t)addr + count > MB_IDENT_REG_COUNT) return MB_EXC_ILLEGAL_ADDR;
    uint8_t regs[MB_IDENT_REG_COUNT * 2];
    build(regs);
    memcpy(out, &regs[addr * 2], (size_t)count * 2u);
    return MB_EXC_NONE;
}

/* ------------------------------------------------------- FC 43 / MEI 14 */

uint8_t mb_ident_device_id(const mb_request_t *req, uint8_t *resp, uint16_t *resp_len)
{
    const app_config_t *cfg = app_config_get();
    const mb_devid_obj_t objs[] = {
        { MB_DEVID_OBJ_VENDOR_NAME,   VENDOR_NAME      },
        { MB_DEVID_OBJ_PRODUCT_CODE,  PRODUCT_NAME     },
        { MB_DEVID_OBJ_REVISION,      s_version        },
        { MB_DEVID_OBJ_VENDOR_URL,    VENDOR_URL       },
        { MB_DEVID_OBJ_PRODUCT_NAME,  PRODUCT_NAME     },
        { MB_DEVID_OBJ_MODEL_NAME,    MODEL_NAME       },
        { MB_DEVID_OBJ_USER_APP_NAME, cfg->device_name },
        { 0x80, s_serial  },           /* serial number, 12 hex digits      */
        { 0x81, s_mac_str },           /* MAC, AA:BB:CC:DD:EE:FF            */
    };
    return mb_devid_encode(objs, sizeof(objs) / sizeof(objs[0]),
                           (uint8_t)req->count, (uint8_t)req->addr, resp, resp_len);
}
