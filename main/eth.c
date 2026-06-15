#include "eth.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"

#define TAG  "eth"

/* W5500 hardware pin mapping */
#define ETH_SPI_HOST    SPI2_HOST
#define ETH_SCK_GPIO    GPIO_NUM_15
#define ETH_MOSI_GPIO   GPIO_NUM_13
#define ETH_MISO_GPIO   GPIO_NUM_14
#define ETH_CS_GPIO     GPIO_NUM_16
#define ETH_IRQ_GPIO    GPIO_NUM_12
#define ETH_RST_GPIO    39          /* reset — plain int, not gpio_num_t */
#define ETH_PHY_ADDR    1
#define ETH_SPI_HZ      (20 * 1000 * 1000)

static eth_connected_cb_t s_on_connected;

/* ---------------------------------------------------------------- events */

static void eth_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "Started");   break;
    case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "Stopped");   break;
    case ETHERNET_EVENT_CONNECTED:    ESP_LOGI(TAG, "Link up");   break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGI(TAG, "Link down"); break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *ev = event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    if (s_on_connected) s_on_connected();
}

/* ---------------------------------------------------------------- public */

esp_err_t eth_init(eth_connected_cb_t on_connected)
{
    s_on_connected = on_connected;

    /* 1 — SPI bus (shared bus, other devices may be added later) */
    spi_bus_config_t bus_cfg = {
        .miso_io_num   = ETH_MISO_GPIO,
        .mosi_io_num   = ETH_MOSI_GPIO,
        .sclk_io_num   = ETH_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(ETH_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
        TAG, "SPI bus init");

    /* 2 — W5500 SPI device config (driver registers device internally) */
    spi_device_interface_config_t spi_dev = {
        .mode           = 0,
        .clock_speed_hz = ETH_SPI_HZ,
        .queue_size     = 16,
        .spics_io_num   = ETH_CS_GPIO,
    };

    /* 3 — W5500 MAC */
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_dev);
    w5500_cfg.int_gpio_num = ETH_IRQ_GPIO;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    if (!mac) return ESP_FAIL;

    /* 4 — W5500 PHY */
    eth_phy_config_t phy_cfg   = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr            = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num      = ETH_RST_GPIO;

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) return ESP_FAIL;

    /* 5 — Install Ethernet driver */
    esp_eth_config_t eth_cfg  = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_hdl  = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &eth_hdl), TAG, "driver install");

    /* 6 — Network interface */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_ERROR(
        esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_hdl)),
        TAG, "netif attach");

    const char *hostname = app_config_get()->device_name;
    if (hostname[0]) esp_netif_set_hostname(eth_netif, hostname);

    /* 7 — Event handlers */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL),
        TAG, "ETH event register");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL),
        TAG, "IP event register");

    /* 8 — Start */
    ESP_RETURN_ON_ERROR(esp_eth_start(eth_hdl), TAG, "eth start");

    ESP_LOGI(TAG, "W5500 initialized (SPI2, CS=GPIO%d, IRQ=GPIO%d)",
             ETH_CS_GPIO, ETH_IRQ_GPIO);
    return ESP_OK;
}
