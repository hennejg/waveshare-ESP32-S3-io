/**
   Copyright 2025 Achim Pieters | StudioPieters®

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   for more information visit https://www.studiopieters.nl
 **/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <lwip/sockets.h>
#include <lwip/ip_addr.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>

#include <http_parser.h>
#include "wifi_config.h"
#include "form_urlencoded.h"

enum {
        STATION_MODE = 1,
        SOFTAP_MODE = 2,
        STATIONAP_MODE = 3,
};

#define STATION_GOT_IP 5
#define SOFTAP_IF WIFI_IF_AP
#define STATION_IF WIFI_IF_STA

static nvs_handle_t wifi_cfg_handle;
static volatile bool sta_got_ip = false;

static wifi_mode_t opmode_to_wifi_mode(int mode) {
        switch (mode) {
        case STATION_MODE: return WIFI_MODE_STA;
        case SOFTAP_MODE: return WIFI_MODE_AP;
        case STATIONAP_MODE: return WIFI_MODE_APSTA;
        default: return WIFI_MODE_NULL;
        }
}

static int wifi_mode_to_opmode(wifi_mode_t mode) {
        switch (mode) {
        case WIFI_MODE_STA: return STATION_MODE;
        case WIFI_MODE_AP: return SOFTAP_MODE;
        case WIFI_MODE_APSTA: return STATIONAP_MODE;
        default: return 0;
        }
}

static int sdk_wifi_get_opmode(void) {
        wifi_mode_t m;
        if (esp_wifi_get_mode(&m) != ESP_OK) return 0;
        return wifi_mode_to_opmode(m);
}

static void sdk_wifi_set_opmode(int mode) {
        ESP_LOGI("wifi_config", "Setting WiFi mode: %d", mode);
        esp_wifi_set_mode(opmode_to_wifi_mode(mode));
}

static void sdk_wifi_get_macaddr(int iface, uint8_t *mac) {
        esp_wifi_get_mac(iface == SOFTAP_IF ? WIFI_IF_AP : WIFI_IF_STA, mac);
}

static void sdk_wifi_softap_get_config(wifi_config_t *cfg) {
        esp_wifi_get_config(WIFI_IF_AP, cfg);
}

static void sdk_wifi_softap_set_config(wifi_config_t *cfg) {
        esp_wifi_set_config(WIFI_IF_AP, cfg);
}

static void sdk_wifi_station_set_config(wifi_config_t *cfg) {
        esp_wifi_set_config(WIFI_IF_STA, cfg);
}

static void sdk_wifi_station_connect(void) {
        esp_wifi_connect();
}

static void sdk_wifi_station_set_auto_connect(bool en) {
        safe_set_auto_connect(en);
}

static void sysparam_init(void) {
        static bool initialized = false;
        if (!initialized) {
                nvs_flash_init();
                nvs_open("wifi_cfg", NVS_READWRITE, &wifi_cfg_handle);
                initialized = true;
        }
}

static void sysparam_set_string(const char *key, const char *value) {
        sysparam_init();
        if (!value) value = "";
        nvs_set_str(wifi_cfg_handle, key, value);
        nvs_commit(wifi_cfg_handle);
}

static void sysparam_get_string(const char *key, char **value) {
        sysparam_init();
        size_t required = 0;
        if (nvs_get_str(wifi_cfg_handle, key, NULL, &required) == ESP_OK && required > 0) {
                *value = malloc(required);
                nvs_get_str(wifi_cfg_handle, key, *value, &required);
        } else {
                *value = NULL;
        }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
        if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
                sta_got_ip = true;
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
                sta_got_ip = false;
        }
}

static int sdk_wifi_station_get_connect_status(void) {
        return sta_got_ip ? STATION_GOT_IP : 0;
}

static bool s_ap_disabled = false;
void wifi_config_disable_ap(void) { s_ap_disabled = true; }

static void wifi_config_init_wifi(void) {
        static bool wifi_inited = false;
        if (wifi_inited) return;

        esp_netif_init();
        esp_event_loop_create_default();
        /* Create netifs if not already present (e.g. when Matter's InitWiFiStack
         * ran first).  Skip AP entirely when wifi_config_disable_ap() was called
         * (Matter CHIPoBLE handles commissioning; AP portal is not needed and
         * its beacon buffers would exhaust the DMA that BLE already claimed). */
        if (!s_ap_disabled && !esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"))
            esp_netif_create_default_wifi_ap();
        if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))
            esp_netif_create_default_wifi_sta();

        /* Skip esp_wifi_init/start if WiFi is already running (e.g. Matter's
         * InitWiFiStack ran first).  Calling esp_wifi_start() on an already-
         * started driver triggers internal event processing that can fragment
         * the residual internal DMA heap, starving later DMA consumers (eth). */
        wifi_mode_t _mode;
        bool already_started = (esp_wifi_get_mode(&_mode) == ESP_OK);
        if (!already_started) {
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            esp_wifi_init(&cfg);
        }
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
        if (s_ap_disabled)
            esp_wifi_set_mode(WIFI_MODE_STA);
        if (!already_started) {
            ESP_LOGI("wifi_config", "Starting WiFi...");
            esp_wifi_start();
        }

        wifi_inited = true;
}

#define WIFI_CONFIG_SERVER_PORT 80

#ifndef WIFI_CONFIG_CONNECT_TIMEOUT
#define WIFI_CONFIG_CONNECT_TIMEOUT 15000
#endif
#ifndef WIFI_CONFIG_CONNECTED_MONITOR_INTERVAL
#define WIFI_CONFIG_CONNECTED_MONITOR_INTERVAL 30000
#endif
#ifndef WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL
#define WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL 10000
#endif

#define INFO(message, ...) printf(">>> wifi_config: " message "\n", ## __VA_ARGS__);
#define ERROR(message, ...) printf("!!! wifi_config: " message "\n", ## __VA_ARGS__);

#ifdef WIFI_CONFIG_DEBUG
#define DEBUG(message, ...) printf("*** wifi_config: " message "\n", ## __VA_ARGS__);
#else
#define DEBUG(message, ...)
#endif


typedef enum {
        ENDPOINT_UNKNOWN = 0,
        ENDPOINT_INDEX,
        ENDPOINT_SETTINGS,
        ENDPOINT_SETTINGS_UPDATE,
        ENDPOINT_ETH_ONLY,
} endpoint_t;

static void (*s_eth_only_cb)(void) = NULL;
void wifi_config_set_eth_only_callback(void (*cb)(void)) { s_eth_only_cb = cb; }

static bool (*s_eth_available_fn)(void) = NULL;
void wifi_config_set_eth_available_fn(bool (*fn)(void)) { s_eth_available_fn = fn; }


typedef struct {
        char *ssid_prefix;
        char *password;
        char *custom_html;
        void (*on_wifi_ready)(); // deprecated
        void (*on_event)(wifi_config_event_t);

        int first_time;
        TimerHandle_t network_monitor_timer;
        TaskHandle_t http_task_handle;
        TaskHandle_t dns_task_handle;
} wifi_config_context_t;


static wifi_config_context_t *context = NULL;

typedef struct _client {
        int fd;
        bool disconnected;

        http_parser parser;
        endpoint_t endpoint;
        uint8_t *body;
        size_t body_length;

        struct _client *next;
} client_t;


static int wifi_config_has_configuration();
static int wifi_config_station_connect();
static void wifi_config_softap_start();
static void wifi_config_softap_stop();

static client_t *client_new() {
        client_t *client = malloc(sizeof(client_t));
        memset(client, 0, sizeof(client_t));

        http_parser_init(&client->parser, HTTP_REQUEST);
        client->parser.data = client;

        return client;
}


static void client_free(client_t *client) {
        if (client->body)
                free(client->body);

        free(client);
}


static void client_send(client_t *client, const char *payload, size_t payload_size) {
        lwip_write(client->fd, payload, payload_size);
}
static void client_send_index(client_t *client) {
        ESP_LOGI("wifi_config", "Serving captive portal response");
        extern const uint8_t index_html_start[] asm ("_binary_index_html_start");
        extern const uint8_t index_html_end[] asm ("_binary_index_html_end");

        const char *header =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n";

        client_send(client, header, strlen(header));
        client_send(client, (const char *)index_html_start, index_html_end - index_html_start);
}






static void client_send_chunk(client_t *client, const char *payload) {
        int len = strlen(payload);
        char buffer[10];
        int buffer_len = snprintf(buffer, sizeof(buffer), "%x\r\n", len);
        client_send(client, buffer, buffer_len);
        client_send(client, payload, len);
        client_send(client, "\r\n", 2);
}


static void client_send_redirect(client_t *client, int code, const char *redirect_url) {
        DEBUG("Redirecting to %s", redirect_url);
        const char *reason = (code == 301) ? "Moved Permanently" : "Found";
        char buffer[128];
        size_t len = snprintf(buffer, sizeof(buffer),
                "HTTP/1.1 %d %s\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                code, reason, redirect_url);
        client_send(client, buffer, len);
}


typedef struct _wifi_network_info {
        char ssid[33];
        bool secure;

        struct _wifi_network_info *next;
} wifi_network_info_t;


wifi_network_info_t *wifi_networks = NULL;
SemaphoreHandle_t wifi_networks_mutex;


static void wifi_scan_task(void *arg)
{
        INFO("Starting WiFi scan");
        while (true) {
                if (sdk_wifi_get_opmode() != STATIONAP_MODE)
                        break;

                esp_wifi_scan_start(NULL, true);

                uint16_t ap_num = 0;
                esp_wifi_scan_get_ap_num(&ap_num);
                wifi_ap_record_t *records = calloc(ap_num, sizeof(wifi_ap_record_t));
                if (records && esp_wifi_scan_get_ap_records(&ap_num, records) == ESP_OK) {
                        xSemaphoreTake(wifi_networks_mutex, portMAX_DELAY);

                        wifi_network_info_t *wifi_network = wifi_networks;
                        while (wifi_network) {
                                wifi_network_info_t *next = wifi_network->next;
                                free(wifi_network);
                                wifi_network = next;
                        }
                        wifi_networks = NULL;

                        for (int i = 0; i < ap_num; i++) {
                                wifi_network_info_t *net = wifi_networks;
                                while (net) {
                                        if (!strncmp(net->ssid, (char *)records[i].ssid, sizeof(net->ssid)))
                                                break;
                                        net = net->next;
                                }
                                if (!net) {
                                        wifi_network_info_t *net = malloc(sizeof(wifi_network_info_t));
                                        memset(net, 0, sizeof(*net));
                                        strncpy(net->ssid, (char *)records[i].ssid, sizeof(net->ssid));
                                        net->secure = records[i].authmode != WIFI_AUTH_OPEN;
                                        net->next = wifi_networks;
                                        wifi_networks = net;
                                }
                        }

                        xSemaphoreGive(wifi_networks_mutex);
                }

                free(records);
                vTaskDelay(10000 / portTICK_PERIOD_MS);
        }

        xSemaphoreTake(wifi_networks_mutex, portMAX_DELAY);

        wifi_network_info_t *wifi_network = wifi_networks;
        while (wifi_network) {
                wifi_network_info_t *next = wifi_network->next;
                free(wifi_network);
                wifi_network = next;
        }
        wifi_networks = NULL;

        xSemaphoreGive(wifi_networks_mutex);

        vTaskDelete(NULL);
}

#include "index.html.h"

static void wifi_config_server_on_settings(client_t *client) {
        static const char http_prologue[] =
                "HTTP/1.1 200 \r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Cache-Control: no-store\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: close\r\n"
                "\r\n";

        client_send(client, http_prologue, sizeof(http_prologue)-1);
        client_send_chunk(client, html_settings_header);

        if (context->custom_html != NULL && context->custom_html[0] > 0) {
                uint8_t buffer_size = strlen(html_settings_custom_html) + strlen(context->custom_html);
                char* buffer = (char*) calloc(buffer_size, sizeof(char)); //fill up the buffer with zeros
                snprintf(buffer, buffer_size, html_settings_custom_html, context->custom_html); //fill in template with the custom_html content
                client_send_chunk(client, buffer);
                free(buffer);
        }

        client_send_chunk(client, html_settings_body);

        if (xSemaphoreTake(wifi_networks_mutex, 5000 / portTICK_PERIOD_MS)) {
                char buffer[64];
                wifi_network_info_t *net = wifi_networks;
                while (net) {
                        snprintf(
                                buffer, sizeof(buffer),
                                html_network_item,
                                net->secure ? "secure" : "unsecure", net->ssid
                                );
                        client_send_chunk(client, buffer);

                        net = net->next;
                }

                xSemaphoreGive(wifi_networks_mutex);
        }

        client_send_chunk(client, html_settings_footer);

        if (s_eth_available_fn && s_eth_available_fn()) {
                static const char eth_section[] =
                        "<div style='margin-top:1.5rem;border-top:1px solid #ccc;padding-top:1rem;"
                        "font-family:sans-serif;font-size:.9rem;color:#555;text-align:center'>"
                        "<p>Ethernet is connected. You can skip WiFi entirely.</p>"
                        "<a href='/eth-only' style='display:inline-block;padding:.4rem 1rem;"
                        "background:#1a73e8;color:#fff;border-radius:4px;text-decoration:none'>"
                        "ETH only &mdash; disable WiFi &rarr;</a>"
                        "<p style='margin-top:.8rem;color:#999;font-size:.8rem'>"
                        "To return to WiFi setup: hold the BOOT button for &ge;&nbsp;5&nbsp;s.</p>"
                        "</div>";
                client_send_chunk(client, eth_section);
        }

        client_send_chunk(client, "");
}


static void wifi_config_server_on_settings_update(client_t *client) {
        DEBUG("Update settings, body = %s", client->body);

        form_param_t *form = form_params_parse((char *)client->body);
        if (!form) {
                DEBUG("Couldn't parse form data, redirecting to /settings");
                client_send_redirect(client, 302, "/settings");
                return;
        }

        form_param_t *ssid_param     = form_params_find(form, "ssid");
        form_param_t *password_param = form_params_find(form, "password");
        if (!ssid_param) {
                DEBUG("Invalid form data, redirecting to /settings");
                form_params_free(form);
                client_send_redirect(client, 302, "/settings");
                return;
        }

        DEBUG("Setting wifi_ssid param = %s", ssid_param->value);
        DEBUG("Setting wifi_password param = %s", password_param ? password_param->value : "(none)");

        sysparam_set_string("wifi_ssid", ssid_param->value);
        sysparam_set_string("wifi_password", password_param ? password_param->value : "");

        /* Send a confirmation page before rebooting so the user sees feedback.
         * Reboot is cleaner than a live APSTA→STA transition: avoids mode-change
         * errors and ensures WiFi starts fresh in STA-only mode. */
        char html[768];
        snprintf(html, sizeof(html),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                 "<!DOCTYPE html><html><head>"
                 "<meta charset='utf-8'>"
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<title>Connecting</title></head>"
                 "<body style='font-family:sans-serif;text-align:center;padding:2rem'>"
                 "<h2>Credentials saved</h2>"
                 "<p>Rebooting &mdash; will connect to <strong>%.32s</strong>.</p>"
                 "<p style='color:#888;font-size:.85rem'>"
                 "To change WiFi: hold the BOOT button for &ge;&nbsp;5&nbsp;s.</p>"
                 "</body></html>",
                 ssid_param->value);
        form_params_free(form);

        write(client->fd, html, strlen(html));
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
}


static int wifi_config_server_on_url(http_parser *parser, const char *data, size_t length) {
        client_t *client = (client_t*) parser->data;

        client->endpoint = ENDPOINT_UNKNOWN;
        if (parser->method == HTTP_GET) {
                if (!strncmp(data, "/settings", length)) {
                        client->endpoint = ENDPOINT_SETTINGS;
                } else if (!strncmp(data, "/", length)) {
                        client->endpoint = ENDPOINT_INDEX;
                } else if (!strncmp(data, "/eth-only", length)) {
                        client->endpoint = ENDPOINT_ETH_ONLY;
                }
        } else if (parser->method == HTTP_POST) {
                if (!strncmp(data, "/settings", length)) {
                        client->endpoint = ENDPOINT_SETTINGS_UPDATE;
                }
        }

        if (client->endpoint == ENDPOINT_UNKNOWN) {
                char *url = strndup(data, length);
                DEBUG("Got HTTP request: %s %s", http_method_str(parser->method), url);
                free(url);
        }

        return 0;
}


static int wifi_config_server_on_body(http_parser *parser, const char *data, size_t length) {
        client_t *client = parser->data;
        client->body = realloc(client->body, client->body_length + length + 1);
        memcpy(client->body + client->body_length, data, length);
        client->body_length += length;
        client->body[client->body_length] = 0;

        return 0;
}


static int wifi_config_server_on_message_complete(http_parser *parser) {
        client_t *client = parser->data;

        switch(client->endpoint) {
        case ENDPOINT_INDEX: {
                INFO("GET / -> 302 http://192.168.4.1/settings");
                client_send_redirect(client, 302, "http://192.168.4.1/settings");
                break;
        }
        case ENDPOINT_SETTINGS: {
                DEBUG("GET /settings");
                wifi_config_server_on_settings(client);
                break;
        }
        case ENDPOINT_SETTINGS_UPDATE: {
                DEBUG("POST /settings");
                wifi_config_server_on_settings_update(client);
                break;
        }
        case ENDPOINT_ETH_ONLY: {
                static const char html[] =
                        "HTTP/1.1 200 \r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                        "<title>Ethernet-only mode</title></head>"
                        "<body style='font-family:sans-serif;text-align:center;padding:2rem'>"
                        "<h2>Switching to Ethernet-only mode</h2>"
                        "<p>Rebooting now &mdash; connect via Ethernet cable.</p>"
                        "<p style='color:#888;font-size:.85rem'>"
                        "To return to WiFi setup: hold the BOOT button for &ge;&nbsp;5&nbsp;s.</p>"
                        "</body></html>";
                write(client->fd, html, sizeof(html) - 1);
                if (s_eth_only_cb) s_eth_only_cb();
                vTaskDelay(pdMS_TO_TICKS(400));
                esp_restart();
                break;
        }
        case ENDPOINT_UNKNOWN: {
                INFO("Captive portal probe -> 302 http://192.168.4.1/settings");
                client_send_redirect(client, 302, "http://192.168.4.1/settings");
                break;
        }
        }

        if (client->body) {
                free(client->body);
                client->body = NULL;
                client->body_length = 0;
        }

        client->disconnected = true;   /* signal read loop to exit after response is sent */

        return 0;
}


/* Direct GET handler — bypasses http_parser entirely for GET requests.
 * Returns true if the request was handled (connection should close).
 * http_parser state machine proved unreliable for Android's Connection:close
 * GET probes, so we route GET requests with a simple inline parser. */
static bool handle_get_request(client_t *client, const char *data, int len) {
        /* Require complete headers (\r\n\r\n) and a GET request line */
        bool eoh = false;
        for (int i = 0; i <= len - 4; i++) {
                if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n') {
                        eoh = true;
                        break;
                }
        }
        if (!eoh || len < 5 || memcmp(data, "GET ", 4) != 0)
                return false;

        const char *url_start = data + 4;
        const char *url_end = (const char *)memchr(url_start, ' ', len - 4);
        if (!url_end)
                return false;

        int ulen = url_end - url_start;
        char url[64];
        if (ulen >= (int)sizeof(url))
                ulen = sizeof(url) - 1;
        memcpy(url, url_start, ulen);
        url[ulen] = '\0';

        if (strcmp(url, "/settings") == 0) {
                INFO("GET /settings");
                wifi_config_server_on_settings(client);
        } else if (strcmp(url, "/eth-only") == 0) {
                static const char html[] =
                        "HTTP/1.1 200 \r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                        "<title>Ethernet-only mode</title></head>"
                        "<body style='font-family:sans-serif;text-align:center;padding:2rem'>"
                        "<h2>Switching to Ethernet-only mode</h2>"
                        "<p>Rebooting now &mdash; connect via Ethernet cable.</p>"
                        "<p style='color:#888;font-size:.85rem'>"
                        "To return to WiFi setup: hold the BOOT button for &ge;&nbsp;5&nbsp;s.</p>"
                        "</body></html>";
                write(client->fd, html, sizeof(html) - 1);
                if (s_eth_only_cb) s_eth_only_cb();
                vTaskDelay(pdMS_TO_TICKS(400));
                esp_restart();
        } else if (strcmp(url, "/captive-portal") == 0) {
                /* RFC 8910 Captive Portal API endpoint — advertised via DHCP option 114.
                 * RFC 8910 clients probe this with Accept: application/captive+json.
                 * NOTE: tested Samsung Android 16 (SM-G990B2 / One UI 8) does NOT probe
                 * this endpoint even when option 114 is present in the DHCP offer. */
                INFO("GET /captive-portal -> RFC 8910 response");
                static const char capport_resp[] =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/captive+json\r\n"
                        "Content-Length: 64\r\n"
                        "Cache-Control: no-store\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"captive\":true,\"user-portal-url\":\"http://192.168.4.1/settings\"}";
                write(client->fd, capport_resp, sizeof(capport_resp) - 1);
        } else {
                /* Redirect everything else to the settings page.  This covers:
                 *   - AOSP Android: GET /generate_204  Host: connectivitycheck.gstatic.com
                 *   - iOS/macOS:    GET /hotspot-detect.html  Host: captive.apple.com
                 *   - Windows:      GET /ncsi.txt  Host: www.msftncsi.com
                 * All receive a 302 redirect; a non-204 response signals captive portal
                 * to these OS network monitors.
                 *
                 * KNOWN LIMITATION — Samsung Android 16 (One UI 8):
                 *   NetworkMonitor sends only an HTTPS probe to connectivitycheck.gstatic.com.
                 *   When that probe fails (TLS alert — untrusted self-signed cert), the OS
                 *   marks the network NO_INTERNET rather than CAPTIVE_PORTAL and never shows
                 *   the "Sign in to network" notification.  No HTTP fallback is issued.
                 *   Users must open a browser manually and navigate to http://192.168.4.1. */
                if (strcmp(url, "/generate_204") == 0 ||
                    strcmp(url, "/hotspot-detect.html") == 0 ||
                    strcmp(url, "/ncsi.txt") == 0 ||
                    strcmp(url, "/canonical.html") == 0) {
                        INFO("GET %s -> captive portal probe, 302 to settings", url);
                } else {
                        INFO("GET %s -> 302 http://192.168.4.1/settings", url);
                }
                client_send_redirect(client, 302, "http://192.168.4.1/settings");
        }

        client->disconnected = true;
        return true;
}

/* Return 1 for GET requests so http_parser sets F_SKIPBODY — kept as a
 * belt-and-suspenders fallback in case handle_get_request ever misses. */
static int wifi_config_server_on_headers_complete(http_parser *parser) {
        return (parser->method == HTTP_GET) ? 1 : 0;
}

static http_parser_settings wifi_config_http_parser_settings = {
        .on_url = wifi_config_server_on_url,
        .on_headers_complete = wifi_config_server_on_headers_complete,
        .on_body = wifi_config_server_on_body,
        .on_message_complete = wifi_config_server_on_message_complete,
};


static void http_task(void *arg) {
        INFO("Starting HTTP server");

        int listenfd = socket(AF_INET, SOCK_STREAM, 0);
        if (listenfd < 0) {
                ERROR("HTTP socket() failed");
                vTaskDelete(NULL);
                return;
        }

        const int reuse = 1;
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        /* 1-second accept() timeout — allows periodic stop-notification checks.
         * Deliberately NOT using O_NONBLOCK: accepted sockets inherit the flag in
         * lwIP and then SO_RCVTIMEO is ignored (non-blocking wins), which causes
         * lwip_read to return EAGAIN before Android's HTTP GET arrives. */
        const struct timeval accept_to = { 1, 0 };
        setsockopt(listenfd, SOL_SOCKET, SO_RCVTIMEO, &accept_to, sizeof(accept_to));

        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        /* Bind only to the AP interface IP so that web_server.c (on the ETH
         * interface) can hold INADDR_ANY:80 simultaneously. */
        serv_addr.sin_addr.s_addr = inet_addr("192.168.4.1");
        serv_addr.sin_port = htons(WIFI_CONFIG_SERVER_PORT);

        if (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
                ERROR("HTTP bind() failed — port %d in use?", WIFI_CONFIG_SERVER_PORT);
                lwip_close(listenfd);
                vTaskDelete(NULL);
                return;
        }
        if (listen(listenfd, 4) < 0) {
                ERROR("HTTP listen() failed");
                lwip_close(listenfd);
                vTaskDelete(NULL);
                return;
        }

        char data[256];

        while (true) {
                uint32_t task_value = 0;
                if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE && task_value)
                        break;

                int fd = accept(listenfd, NULL, NULL);
                if (fd < 0)
                        continue;   /* timeout (EAGAIN) or error — recheck stop notification */

                INFO("HTTP: client connected, fd=%d", fd);

                const struct timeval client_to = { 5, 0 };
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &client_to, sizeof(client_to));

                client_t *client = client_new();
                client->fd = fd;

                while (true) {
                        int len = lwip_read(fd, data, sizeof(data) - 1);
                        if (len > 0) {
                                data[len] = '\0';
                                INFO("HTTP rx fd=%d len=%d [%.*s]", fd, len, len, data);
                                if (!client->disconnected) {
                                        if (!handle_get_request(client, data, len)) {
                                                /* Not a complete GET — run http_parser (handles POST body) */
                                                size_t nparsed = http_parser_execute(&client->parser, &wifi_config_http_parser_settings, data, len);
                                                if ((int)nparsed != len || HTTP_PARSER_ERRNO(&client->parser) != HPE_OK)
                                                        INFO("HTTP parser: consumed %d/%d errno=%s", (int)nparsed, len,
                                                             http_errno_name(HTTP_PARSER_ERRNO(&client->parser)));
                                        }
                                }
                                if (client->disconnected)
                                        break;
                        } else {
                                /* len==0: client closed (ACKed our response); len<0: timeout/error */
                                INFO("HTTP rx fd=%d close len=%d errno=%d", fd, len, errno);
                                break;
                        }
                }

                lwip_close(fd);
                client_free(client);
        }

        INFO("Stopping HTTP server");
        lwip_close(listenfd);
        vTaskDelete(NULL);
}


static void http_start() {
        xTaskCreate(http_task, "wifi_config HTTP", 8192, NULL, 2, &context->http_task_handle);
}


static void http_stop() {
        if (!context->http_task_handle)
                return;

        xTaskNotify(context->http_task_handle, 1, eSetValueWithOverwrite);
        context->http_task_handle = NULL;
}


static void dns_task(void *arg)
{
        INFO("Starting DNS server");

        ip4_addr_t server_addr;
        IP4_ADDR(&server_addr, 192, 168, 4, 1);

        struct sockaddr_in serv_addr;
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
                ERROR("DNS socket() failed");
                vTaskDelete(NULL);
                return;
        }

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        serv_addr.sin_port = htons(53);
        if (bind(fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
                ERROR("DNS bind() failed — port 53 in use?");
                lwip_close(fd);
                vTaskDelete(NULL);
                return;
        }

        const struct timeval timeout = { 2, 0 }; /* 2 second timeout */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        for (;;) {
                /* 512 bytes: enough for EDNS queries (OPT record appended by Android 8+) */
                char buffer[512];
                struct sockaddr src_addr;
                socklen_t src_addr_len = sizeof(src_addr);
                size_t count = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&src_addr, &src_addr_len);

                /* Drop messages that are too small or too large to build a valid response */
                if (count > 12 && count <= sizeof(buffer) - 16 && src_addr.sa_family == AF_INET) {
                        size_t qname_len = strlen(buffer + 12) + 1;

                        /* Read QTYPE before overwriting the buffer */
                        uint8_t qtype_hi = (uint8_t)buffer[12 + qname_len];
                        uint8_t qtype_lo = (uint8_t)buffer[12 + qname_len + 1];
                        /* Answer with an A record for A (0x0001) and ANY (0x00FF) queries */
                        int send_a = (qtype_hi == 0x00 && (qtype_lo == 0x01 || qtype_lo == 0xFF));

                        char *head = buffer + 2;
                        /* QR=1, copy RD from query, AA/TC=0 */
                        *head++ = (char)(0x80 | (buffer[2] & 0x01));
                        /* RA=1, RCODE=0 */
                        *head++ = (char)0x80;
                        *head++ = 0x00; /* QDCOUNT = 1 */
                        *head++ = 0x01;
                        *head++ = 0x00; /* ANCOUNT */
                        *head++ = send_a ? 0x01 : 0x00;
                        *head++ = 0x00; /* NSCOUNT = 0 */
                        *head++ = 0x00;
                        *head++ = 0x00; /* ARCOUNT = 0 */
                        *head++ = 0x00;
                        head += qname_len; /* skip QNAME (kept verbatim) */
                        /* Echo original QTYPE and QCLASS in the question section */
                        *head++ = qtype_hi;
                        *head++ = qtype_lo;
                        *head++ = 0x00; /* QCLASS IN */
                        *head++ = 0x01;

                        uint32_t reply_len = 2 + 10 + qname_len + 4;

                        if (send_a) {
                                *head++ = 0xC0; /* name = pointer to offset 12 */
                                *head++ = 0x0C;
                                *head++ = 0x00; /* TYPE A */
                                *head++ = 0x01;
                                *head++ = 0x00; /* CLASS IN */
                                *head++ = 0x01;
                                *head++ = 0x00; /* TTL = 120 s */
                                *head++ = 0x00;
                                *head++ = 0x00;
                                *head++ = 0x78;
                                *head++ = 0x00; /* RDLENGTH = 4 */
                                *head++ = 0x04;
                                *head++ = ip4_addr1(&server_addr);
                                *head++ = ip4_addr2(&server_addr);
                                *head++ = ip4_addr3(&server_addr);
                                *head++ = ip4_addr4(&server_addr);
                                reply_len += 16;
                        }

                        /* Decode DNS label-encoded QNAME for diagnostics */
                        char domain[128] = "";
                        const char *p = buffer + 12;
                        int dlen = 0;
                        while (*p && dlen < (int)sizeof(domain) - 2) {
                                int llen = (unsigned char)*p++;
                                if (dlen) domain[dlen++] = '.';
                                while (llen-- && dlen < (int)sizeof(domain) - 1)
                                        domain[dlen++] = *p++;
                        }
                        domain[dlen] = '\0';
                        INFO("DNS query: %s type=0x%02X -> %s", domain, qtype_lo, send_a ? "A 192.168.4.1" : "NOERROR/empty");
                        sendto(fd, buffer, reply_len, 0, &src_addr, src_addr_len);
                }

                uint32_t task_value = 0;
                if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE) {
                        if (task_value)
                                break;
                }
        }

        INFO("Stopping DNS server");

        lwip_close(fd);

        vTaskDelete(NULL);
}


static void dns_start() {
        xTaskCreate(dns_task, "wifi_config DNS", 4096, NULL, 2, &context->dns_task_handle);
}


static void dns_stop() {
        if (!context->dns_task_handle)
                return;

        xTaskNotify(context->dns_task_handle, 1, eSetValueWithOverwrite);
        context->dns_task_handle = NULL;
}


static void wifi_config_softap_start() {
        INFO("Starting AP mode");

        sdk_wifi_set_opmode(STATIONAP_MODE);

        uint8_t macaddr[6];
        sdk_wifi_get_macaddr(SOFTAP_IF, macaddr);

        wifi_config_t ap_cfg;
        sdk_wifi_softap_get_config(&ap_cfg);
        ap_cfg.ap.ssid_len = snprintf(
                (char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid),
                "%s-%02X%02X%02X", context->ssid_prefix, macaddr[3], macaddr[4], macaddr[5]
                );
        ap_cfg.ap.ssid_hidden = 0;
        if (context->password) {
                ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
                strncpy((char *)ap_cfg.ap.password,
                        context->password, sizeof(ap_cfg.ap.password));
        } else {
                ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }
        ap_cfg.ap.max_connection = 2;
        ap_cfg.ap.beacon_interval = 100;

        DEBUG("Starting AP SSID=%s", ap_cfg.ap.ssid);

        sdk_wifi_softap_set_config(&ap_cfg);

        /* DHCP option 114 (RFC 8910): advertise the captive portal API URI.
         * Clients that implement RFC 8910 will probe this URL with
         * Accept: application/captive+json instead of using HTTPS connectivity checks.
         * NOTE: Samsung Android 16 (One UI 8) ignores this option entirely. */
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        INFO("AP netif handle: %p", (void *)ap_netif);
        if (ap_netif) {
                static const char portal_uri[] = "http://192.168.4.1/captive-portal";
                esp_err_t err;
                err = esp_netif_dhcps_stop(ap_netif);
                INFO("DHCP stop: %s", esp_err_to_name(err));
                err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                        ESP_NETIF_CAPTIVEPORTAL_URI, (void *)portal_uri, sizeof(portal_uri) - 1);
                INFO("DHCP captive portal option set: %s (uri=%s)", esp_err_to_name(err), portal_uri);
                err = esp_netif_dhcps_start(ap_netif);
                INFO("DHCP start: %s", esp_err_to_name(err));
        }

        wifi_networks_mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(wifi_networks_mutex);

        xTaskCreate(wifi_scan_task, "wifi_config scan", 4096, NULL, 2, NULL);

        INFO("Starting AP interface");

        dns_start();
        http_start();
}


static void wifi_config_softap_stop() {
        dns_stop();
        http_stop();
        sdk_wifi_set_opmode(STATION_MODE);
}


static void wifi_config_monitor_callback(TimerHandle_t xTimer) {
        if (sdk_wifi_station_get_connect_status() == STATION_GOT_IP) {
                if (sdk_wifi_get_opmode() == STATION_MODE && !context->first_time)
                        return;

                // Connected to station, all is dandy
                INFO("Connected to WiFi network");

                wifi_config_softap_stop();
                sdk_wifi_station_set_auto_connect(false);

                if (context->on_event)
                        context->on_event(WIFI_CONFIG_CONNECTED);

                context->first_time = false;

                // change monitoring poll interval
                xTimerChangePeriod(
                        context->network_monitor_timer,
                        pdMS_TO_TICKS(WIFI_CONFIG_CONNECTED_MONITOR_INTERVAL), 0);

                return;
        } else {
                if (wifi_config_has_configuration())
                        wifi_config_station_connect();

                if (sdk_wifi_get_opmode() != STATION_MODE)
                        return;

                INFO("Disconnected from WiFi network");

                if (!context->first_time && context->on_event)
                        context->on_event(WIFI_CONFIG_DISCONNECTED);

                // change monitoring poll interval
                xTimerChangePeriod(
                        context->network_monitor_timer,
                        pdMS_TO_TICKS(WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL), 0);

                if (!s_ap_disabled)
                        wifi_config_softap_start();
        }
}


static int wifi_config_has_configuration() {
        char *wifi_ssid = NULL;
        sysparam_get_string("wifi_ssid", &wifi_ssid);

        /* wifi_config_reset() stores "" rather than deleting the key, so treat
         * an empty string the same as absent. */
        if (!wifi_ssid || !wifi_ssid[0]) {
                free(wifi_ssid);
                return 0;
        }

        free(wifi_ssid);

        return 1;
}


static int wifi_config_station_connect() {
        char *wifi_ssid = NULL;
        char *wifi_password = NULL;
        sysparam_get_string("wifi_ssid", &wifi_ssid);
        sysparam_get_string("wifi_password", &wifi_password);

        if (!wifi_ssid) {
                ERROR("No configuration found");
                if (wifi_password)
                        free(wifi_password);
                return -1;
        }

        INFO("Connecting to %s", wifi_ssid);

        wifi_config_t sta_config;
        memset(&sta_config, 0, sizeof(sta_config));
        strncpy((char *)sta_config.sta.ssid, wifi_ssid, sizeof(sta_config.sta.ssid));
        sta_config.sta.ssid[sizeof(sta_config.sta.ssid)-1] = 0;
        if (wifi_password)
                strncpy((char *)sta_config.sta.password, wifi_password, sizeof(sta_config.sta.password));

        sdk_wifi_station_set_config(&sta_config);

        sdk_wifi_station_connect();
        sdk_wifi_station_set_auto_connect(true);

        free(wifi_ssid);
        if (wifi_password)
                free(wifi_password);

        return 0;
}


void wifi_config_start() {
        wifi_config_init_wifi();
        sdk_wifi_set_opmode(STATION_MODE);

        context->first_time = true;

        if (wifi_config_station_connect()) {
                if (!s_ap_disabled)
                        wifi_config_softap_start();
                else
                        return;  /* AP disabled + STA has no config: CHIPoBLE handles it */
        }

        if (!context->network_monitor_timer) {
                context->network_monitor_timer = xTimerCreate(
                        "wifi_cfg_mon",
                        pdMS_TO_TICKS(WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL),
                        pdTRUE,
                        NULL,
                        wifi_config_monitor_callback);
                xTimerStart(context->network_monitor_timer, 0);
        } else {
                xTimerChangePeriod(
                        context->network_monitor_timer,
                        pdMS_TO_TICKS(WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL), 0);
        }
}


void wifi_config_legacy_support_on_event(wifi_config_event_t event) {
        if (event == WIFI_CONFIG_CONNECTED) {
                if (context->on_wifi_ready) {
                        context->on_wifi_ready();
                }
        }
#ifndef WIFI_CONFIG_NO_RESTART
        else if (event == WIFI_CONFIG_DISCONNECTED) {
                esp_restart();
        }
#endif
}


void wifi_config_init(const char *ssid_prefix, const char *password, void (*on_wifi_ready)()) {
        INFO("Initializing WiFi config");
        if (password && strlen(password) < 8) {
                ERROR("Password should be at least 8 characters");
                return;
        }

        context = malloc(sizeof(wifi_config_context_t));
        memset(context, 0, sizeof(*context));

        context->ssid_prefix = strndup(ssid_prefix, 33-7);
        if (password)
                context->password = strdup(password);

        context->on_wifi_ready = on_wifi_ready;
        context->on_event = wifi_config_legacy_support_on_event;

        wifi_config_start();
}


void wifi_config_init2(const char *ssid_prefix, const char *password,
                       void (*on_event)(wifi_config_event_t))
{
        INFO("Initializing WiFi config");
        if (password && strlen(password) < 8) {
                ERROR("Password should be at least 8 characters");
                return;
        }

        context = malloc(sizeof(wifi_config_context_t));
        memset(context, 0, sizeof(*context));

        context->ssid_prefix = strndup(ssid_prefix, 33-7);
        if (password)
                context->password = strdup(password);

        context->on_event = on_event;

        wifi_config_start();
}


void wifi_config_reset() {
        sysparam_set_string("wifi_ssid", "");
        sysparam_set_string("wifi_password", "");
}


void wifi_config_get(char **ssid, char **password) {
        if (ssid)
                sysparam_get_string("wifi_ssid", ssid);

        if (password)
                sysparam_get_string("wifi_password", password);
}


void wifi_config_set(const char *ssid, const char *password) {
        sysparam_set_string("wifi_ssid", ssid);
        sysparam_set_string("wifi_password", password);
}

void wifi_config_set_custom_html(char *html) {
        if (context == NULL) {
                ERROR("Cannot set custom html content, WiFi configuration not initialised yet");
                return;
        }

        context->custom_html = html;
}


__attribute__((used)) static void *linker_keep_client_send_index = (void *)&client_send_index;
