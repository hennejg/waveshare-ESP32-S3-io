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

void mb_ident_init(void)
{
    if (esp_read_mac(s_mac, ESP_MAC_BASE) != ESP_OK) memset(s_mac, 0, 6);
    snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", MAC2STR(s_mac));
    snprintf(s_serial,  sizeof(s_serial),  "%02X%02X%02X%02X%02X%02X",      MAC2STR(s_mac));

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

static bool has_ip(esp_netif_t *n, void *ctx)
{
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(n, &ip) == ESP_OK &&
           ip.ip.addr == *(const uint32_t *)ctx;
}

/* The interface the client reached us through. Over TCP that is known
   exactly: the connection's local address. Only ETH_DEF used to be asked, so
   a board on WiFi told its clients it was reachable at 0.0.0.0, and with both
   up a client on one was given the other's address and gateway.

   Over RS-485 there is no such address, and the first interface that is up
   with an address is the best answer there is -- Ethernet, then WiFi, then
   the provisioning access point. */
static esp_netif_t *reachable_netif(uint32_t local_ip)
{
    if (local_ip) {
        esp_netif_t *n = esp_netif_find_if(has_ip, &local_ip);
        if (n) return n;
    }

    static const char *const keys[] = { "ETH_DEF", "WIFI_STA_DEF", "WIFI_AP_DEF" };
    esp_netif_t *fallback = NULL;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        esp_netif_t *n = esp_netif_get_handle_from_ifkey(keys[i]);
        if (!n) continue;
        esp_netif_ip_info_t ip;
        if (esp_netif_is_netif_up(n) && esp_netif_get_ip_info(n, &ip) == ESP_OK &&
            ip.ip.addr != 0)
            return n;
        if (!fallback) fallback = n;
    }
    return fallback;
}

/* Builds the whole block, 160 bytes, on the stack of whichever task asks;
   a read then copies the slice it wants. Simpler than filling only the
   requested slice, and a client reads this perhaps once a day. */
static void build(uint8_t *regs, uint32_t local_ip)
{
    memset(regs, 0, MB_IDENT_REG_COUNT * 2);
    const app_config_t *cfg = app_config_get();

    mb_put16(&regs[2 * MB_IDENT_REG_DEVICE_TYPE], MB_IDENT_TYPE_8DI_8DO);
    mb_put16(&regs[2 * MB_IDENT_REG_FW_MAJOR], s_fw[0]);
    mb_put16(&regs[2 * MB_IDENT_REG_FW_MINOR], s_fw[1]);
    mb_put16(&regs[2 * MB_IDENT_REG_FW_PATCH], s_fw[2]);
    memcpy(&regs[2 * MB_IDENT_REG_MAC], s_mac, 6);
    mb_put32(&regs[2 * MB_IDENT_REG_SERIAL],
          ((uint32_t)s_mac[2] << 24) | ((uint32_t)s_mac[3] << 16) |
          ((uint32_t)s_mac[4] << 8)  |  (uint32_t)s_mac[5]);
    mb_put32(&regs[2 * MB_IDENT_REG_UPTIME], (uint32_t)(esp_timer_get_time() / 1000000LL));

    esp_netif_t *netif = reachable_netif(local_ip);
    esp_netif_ip_info_t ip = {0};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        mb_put32(&regs[2 * MB_IDENT_REG_IP],      ntohl(ip.ip.addr));
        mb_put32(&regs[2 * MB_IDENT_REG_NETMASK], ntohl(ip.netmask.addr));
        mb_put32(&regs[2 * MB_IDENT_REG_GATEWAY], ntohl(ip.gw.addr));
    }
    esp_netif_dhcp_status_t dhcp = ESP_NETIF_DHCP_INIT;
    if (netif && esp_netif_dhcpc_get_status(netif, &dhcp) == ESP_OK)
        mb_put16(&regs[2 * MB_IDENT_REG_DHCP], dhcp == ESP_NETIF_DHCP_STARTED ? 1 : 0);

    mb_ascii_to_regs(PRODUCT_NAME,     20, &regs[MB_IDENT_REG_MODEL * 2]);
    mb_ascii_to_regs(cfg->device_name, 16, &regs[MB_IDENT_REG_DEVICE_NAME * 2]);
    mb_ascii_to_regs(s_serial,          8, &regs[MB_IDENT_REG_SERIAL_STR * 2]);
    mb_ascii_to_regs(s_version,        16, &regs[MB_IDENT_REG_FW_VERSION * 2]);
}

uint8_t mb_ident_read_input(uint16_t addr, uint16_t count, uint32_t local_ip,
                            uint8_t *out)
{
    if ((uint32_t)addr + count > MB_IDENT_REG_COUNT) return MB_EXC_ILLEGAL_ADDR;
    uint8_t regs[MB_IDENT_REG_COUNT * 2];
    build(regs, local_ip);
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
