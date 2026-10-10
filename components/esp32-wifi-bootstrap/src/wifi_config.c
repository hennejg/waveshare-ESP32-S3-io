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

/* Retried on the next use when the namespace could not be opened, rather
   than going on with a handle that every read and write then fails on. */
static esp_err_t sysparam_init(void) {
        static bool initialized = false;
        if (!initialized) {
                nvs_flash_init();
                esp_err_t r = nvs_open("wifi_cfg", NVS_READWRITE, &wifi_cfg_handle);
                if (r != ESP_OK) return r;
                initialized = true;
        }
        return ESP_OK;
}

static esp_err_t sysparam_set_string(const char *key, const char *value) {
        esp_err_t r = sysparam_init();
        if (r != ESP_OK) return r;
        if (!value) value = "";
        r = nvs_set_str(wifi_cfg_handle, key, value);
        if (r == ESP_OK) r = nvs_commit(wifi_cfg_handle);
        return r;
}

static void sysparam_get_string(const char *key, char **value) {
        size_t required = 0;
        if (sysparam_init() == ESP_OK &&
            nvs_get_str(wifi_cfg_handle, key, NULL, &required) == ESP_OK && required > 0) {
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

/* Set from another task (Matter, once commissioned); the monitor reads it. */
static volatile bool s_ap_disabled = false;
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

/* Writes the first max_in bytes of in to out as HTML text. SSIDs come from
   whatever access point is in range and are put into the setup page: as
   they are, a 21-character SSID like <svg/onload=...> is markup, and runs
   script on the page the WiFi password is typed into. out is always
   terminated; what does not fit whole is left off. */
static void html_escape(char *out, size_t cap, const char *in, size_t max_in) {
        size_t o = 0;
        for (size_t i = 0; i < max_in && in[i]; i++) {
                const char *e;
                char one[2] = { in[i], 0 };
                switch (in[i]) {
                case '&':  e = "&amp;";  break;
                case '<':  e = "&lt;";   break;
                case '>':  e = "&gt;";   break;
                case '"':  e = "&quot;"; break;
                case '\'': e = "&#39;";  break;
                default:   e = one;      break;
                }
                size_t n = strlen(e);
                if (o + n >= cap) break;
                memcpy(out + o, e, n);
                o += n;
        }
        out[o] = 0;
}

/* An SSID is at most 32 bytes, each at most 6 once escaped. */
#define SSID_HTML_MAX (32 * 6 + 1)

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

        client_send_chunk(client, html_settings_body);

        if (xSemaphoreTake(wifi_networks_mutex, 5000 / portTICK_PERIOD_MS)) {
                char ssid[SSID_HTML_MAX];
                char buffer[SSID_HTML_MAX + 64];
                wifi_network_info_t *net = wifi_networks;
                while (net) {
                        html_escape(ssid, sizeof(ssid), net->ssid, sizeof(net->ssid));
                        snprintf(
                                buffer, sizeof(buffer),
                                html_network_item,
                                net->secure ? "secure" : "unsecure", ssid
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
                        "<form method='post' action='/eth-only' style='margin:0'>"
                        "<button type='submit' style='padding:.4rem 1rem;border:0;"
                        "background:#1a73e8;color:#fff;border-radius:4px;font-size:1rem'>"
                        "ETH only &mdash; disable WiFi &rarr;</button></form>"
                        "<p style='margin-top:.8rem;color:#999;font-size:.8rem'>"
                        "To return to WiFi setup: hold the BOOT button for &ge;&nbsp;5&nbsp;s.</p>"
                        "</div>";
                client_send_chunk(client, eth_section);
        }

        client_send_chunk(client, "");
}


/* Two NVS keys cannot be written as one. The SSID is cleared first and set
   last, so a write that fails part way leaves no SSID -- which brings the
   setup AP back -- instead of the new password next to the old network. */
static esp_err_t store_credentials(const char *ssid, const char *password) {
        esp_err_t r = sysparam_set_string("wifi_ssid", "");
        if (r == ESP_OK) r = sysparam_set_string("wifi_password", password);
        if (r == ESP_OK) r = sysparam_set_string("wifi_ssid", ssid);
        if (r != ESP_OK) ERROR("Storing the WiFi credentials failed: %s", esp_err_to_name(r));
        return r;
}

static void wifi_config_server_on_settings_update(client_t *client) {
        /* A POST without a body (Content-Length: 0) never allocates one. */
        if (!client->body) {
                client_send_redirect(client, 302, "/settings");
                return;
        }

        form_param_t *form = form_params_parse((char *)client->body);
        if (!form) {
                DEBUG("Couldn't parse form data, redirecting to /settings");
                client_send_redirect(client, 302, "/settings");
                return;
        }

        form_param_t *ssid_param     = form_params_find(form, "ssid");
        form_param_t *password_param = form_params_find(form, "password");
        /* What the station can take: an SSID of 1..32 bytes, a passphrase
           of up to 64. Anything else was stored and confirmed before, and
           then never connected -- with the AP off for good, as credentials
           were stored. */
        if (!ssid_param || !ssid_param->value || strlen(ssid_param->value) > 32 ||
            (password_param && password_param->value && strlen(password_param->value) > 64)) {
                DEBUG("Invalid form data, redirecting to /settings");
                form_params_free(form);
                client_send_redirect(client, 302, "/settings");
                return;
        }

        char ssid[SSID_HTML_MAX];
        html_escape(ssid, sizeof(ssid), ssid_param->value, 32);
        esp_err_t stored = store_credentials(ssid_param->value, password_param ? password_param->value : "");
        form_params_free(form);

        if (stored != ESP_OK) {
                /* No reboot: nothing new to boot into, and the page stays
                   where the user can try again. */
                static const char failed[] =
                        "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                        "<title>Not saved</title></head>"
                        "<body style='font-family:sans-serif;text-align:center;padding:2rem'>"
                        "<h2>Credentials could not be saved</h2>"
                        "<p><a href='/settings'>Try again</a></p>"
                        "</body></html>";
                write(client->fd, failed, sizeof(failed) - 1);
                return;
        }

        /* Send a confirmation page before rebooting so the user sees feedback.
         * Reboot is cleaner than a live APSTA→STA transition: avoids mode-change
         * errors and ensures WiFi starts fresh in STA-only mode. */
        char html[768 + SSID_HTML_MAX];
        snprintf(html, sizeof(html),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                 "<!DOCTYPE html><html><head>"
                 "<meta charset='utf-8'>"
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<title>Connecting</title></head>"
                 "<body style='font-family:sans-serif;text-align:center;padding:2rem'>"
                 "<h2>Credentials saved</h2>"
                 "<p>Rebooting &mdash; will connect to <strong>%s</strong>.</p>"
                 "<p style='color:#888;font-size:.85rem'>"
                 "To change WiFi: hold the BOOT button for &ge;&nbsp;5&nbsp;s.</p>"
                 "</body></html>",
                 ssid);

        write(client->fd, html, strlen(html));
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
}


/* The whole URL, not a prefix of it: strncmp() bounded by the request's
   length matched any start of the path -- "POST /" was a settings update and
   "POST /e" switched WiFi off. */
static bool url_is(const char *data, size_t length, const char *path) {
        return length == strlen(path) && !memcmp(data, path, length);
}

static int wifi_config_server_on_url(http_parser *parser, const char *data, size_t length) {
        client_t *client = (client_t*) parser->data;

        client->endpoint = ENDPOINT_UNKNOWN;
        if (parser->method == HTTP_GET) {
                if (url_is(data, length, "/settings")) {
                        client->endpoint = ENDPOINT_SETTINGS;
                } else if (url_is(data, length, "/")) {
                        client->endpoint = ENDPOINT_INDEX;
                }
        } else if (parser->method == HTTP_POST) {
                /* Switching WiFi off is a POST, so a plain link or an <img>
                 * on some other page cannot do it; GET only ever shows pages. */
                if (url_is(data, length, "/settings")) {
                        client->endpoint = ENDPOINT_SETTINGS_UPDATE;
                } else if (url_is(data, length, "/eth-only")) {
                        client->endpoint = ENDPOINT_ETH_ONLY;
                }
        }

        if (client->endpoint == ENDPOINT_UNKNOWN) {
                char *url = strndup(data, length);
                DEBUG("Got HTTP request: %s %s", http_method_str(parser->method), url);
                free(url);
        }

        return 0;
}


/* The largest body the settings form can produce: an SSID of 32 and a
   passphrase of 63 characters, every one percent-encoded, plus the field
   names. Anything beyond it is not a settings form, and a body with no
   limit let any client on the open setup network grow the allocation until
   the heap was gone. */
#define WIFI_CONFIG_MAX_BODY 512

static int wifi_config_server_on_body(http_parser *parser, const char *data, size_t length) {
        client_t *client = parser->data;
        if (length > WIFI_CONFIG_MAX_BODY || client->body_length > WIFI_CONFIG_MAX_BODY - length) {
                client->disconnected = true;
                return 1;                       /* stops the parser; the read loop ends */
        }
        uint8_t *grown = realloc(client->body, client->body_length + length + 1);
        if (!grown) {
                client->disconnected = true;    /* the old buffer is still ours and is freed with the client */
                return 1;
        }
        client->body = grown;
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


/* One connection, start to close. The server takes one client at a time,
   so a client must not be able to keep it: SO_RCVTIMEO only bounds each
   read, and a client sending a byte every few seconds -- or anything the
   parser cannot read, such as a TLS hello on port 80, which left the loop
   reading on -- kept the setup page from everyone else for as long as it
   liked. Now the whole connection has a deadline, and a parse error ends it. */
#define WIFI_CONFIG_CLIENT_DEADLINE_MS 10000

static void serve_client(int fd) {
        const struct timeval client_to = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &client_to, sizeof(client_to));

        client_t *client = client_new();
        client->fd = fd;
        char data[256];
        const TickType_t start = xTaskGetTickCount();

        while (xTaskGetTickCount() - start < pdMS_TO_TICKS(WIFI_CONFIG_CLIENT_DEADLINE_MS)) {
                int len = lwip_read(fd, data, sizeof(data) - 1);
                if (len <= 0) {
                        /* len==0: client closed (ACKed our response); len<0: timeout/error */
                        INFO("HTTP rx fd=%d close len=%d errno=%d", fd, len, errno);
                        break;
                }
                data[len] = '\0';
                /* Only the length: the data is the setup form, WiFi password included. */
                INFO("HTTP rx fd=%d len=%d", fd, len);
                if (!handle_get_request(client, data, len)) {
                        /* Not a complete GET — run http_parser (handles POST body) */
                        size_t nparsed = http_parser_execute(&client->parser, &wifi_config_http_parser_settings, data, len);
                        if ((int)nparsed != len || HTTP_PARSER_ERRNO(&client->parser) != HPE_OK) {
                                INFO("HTTP parser: consumed %d/%d errno=%s", (int)nparsed, len,
                                     http_errno_name(HTTP_PARSER_ERRNO(&client->parser)));
                                break;
                        }
                }
                if (client->disconnected)
                        break;
        }

        lwip_close(fd);
        client_free(client);
}

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

        while (true) {
                uint32_t task_value = 0;
                if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE && task_value)
                        break;

                int fd = accept(listenfd, NULL, NULL);
                if (fd < 0)
                        continue;   /* timeout (EAGAIN) or error — recheck stop notification */

                INFO("HTTP: client connected, fd=%d", fd);
                serve_client(fd);
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


/* Turns the query in buf (len bytes received, cap the buffer size) into the
   reply in place: every A or ANY question is answered with ip, anything else
   with an empty NOERROR. Returns the reply's length, or 0 for a datagram
   that is dropped.

   Everything is read within len. The QNAME used to be measured with strlen()
   on a buffer that is not terminated and still holds an earlier, longer
   datagram: anyone on the open setup AP could send 512 non-zero bytes and
   then a short query, and the reply was written and sent from past the end
   of the stack buffer. */
static size_t dns_build_reply(uint8_t *buf, size_t len, size_t cap, const uint8_t ip[4]) {
        if (len < 12 || (buf[2] & 0x80))               /* header; a response, not a query */
                return 0;
        if (buf[4] != 0 || buf[5] != 1)                /* exactly one question */
                return 0;

        size_t i = 12;
        while (i < len && buf[i]) {
                if (buf[i] > 63)                       /* compression or extended label */
                        return 0;
                i += 1 + buf[i];
        }
        if (i >= len || i - 12 + 1 > 255)              /* no terminator, or name too long */
                return 0;
        size_t qname_len = i - 12 + 1;
        if (12 + qname_len + 4 > len)                  /* QTYPE and QCLASS */
                return 0;

        uint8_t qtype_hi = buf[12 + qname_len];
        uint8_t qtype_lo = buf[12 + qname_len + 1];
        /* Answer with an A record for A (0x0001) and ANY (0x00FF) queries */
        int send_a = (qtype_hi == 0x00 && (qtype_lo == 0x01 || qtype_lo == 0xFF));
        size_t reply_len = 12 + qname_len + 4 + (send_a ? 16 : 0);
        if (reply_len > cap)
                return 0;

        /* QR=1, copy RD from query, AA/TC=0; RA=1, RCODE=0 */
        buf[2] = (uint8_t)(0x80 | (buf[2] & 0x01));
        buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = send_a ? 0x01 : 0x00;  /* ANCOUNT */
        buf[8] = buf[9] = buf[10] = buf[11] = 0x00;    /* NSCOUNT, ARCOUNT */
        /* The question stays as it came, QCLASS set to IN. */
        uint8_t *head = buf + 12 + qname_len + 2;
        *head++ = 0x00;
        *head++ = 0x01;

        if (send_a) {
                static const uint8_t answer[12] = {
                        0xC0, 0x0C,                    /* name = pointer to offset 12 */
                        0x00, 0x01,                    /* TYPE A */
                        0x00, 0x01,                    /* CLASS IN */
                        0x00, 0x00, 0x00, 0x78,        /* TTL = 120 s */
                        0x00, 0x04,                    /* RDLENGTH = 4 */
                };
                memcpy(head, answer, sizeof(answer));
                memcpy(head + sizeof(answer), ip, 4);
        }
        DEBUG("DNS query type=0x%02X -> %s", qtype_lo, send_a ? "A 192.168.4.1" : "NOERROR/empty");
        return reply_len;
}


static void dns_task(void *arg)
{
        INFO("Starting DNS server");

        static const uint8_t ip[4] = { 192, 168, 4, 1 };

        struct sockaddr_in serv_addr;
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
                ERROR("DNS socket() failed");
                vTaskDelete(NULL);
                return;
        }

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        /* The AP address only, as the HTTP server does: bound to every
         * interface, it answered DNS queries arriving over Ethernet or the
         * station link too, with 192.168.4.1 for every name -- to any host
         * on the network that asked it. */
        memcpy(&serv_addr.sin_addr.s_addr, ip, 4);
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
                uint8_t buffer[512];
                struct sockaddr src_addr;
                socklen_t src_addr_len = sizeof(src_addr);
                int recvd = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&src_addr, &src_addr_len);

                if (recvd > 0 && src_addr.sa_family == AF_INET) {
                        size_t reply_len = dns_build_reply(buffer, (size_t)recvd, sizeof(buffer), ip);
                        if (reply_len)
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


/* Set when the monitor has seen the station lose its link. With credentials
 * stored the setup AP stays off during an outage, so the mode is still plain
 * STATION when the link comes back -- this, not the mode, is what says the
 * application must be told again (it stopped its services on the loss). */
static bool s_link_lost;

static void wifi_config_monitor_callback(TimerHandle_t xTimer) {
        /* Disabled while it was up: taken down here, on the task that
           starts and stops it. */
        if (s_ap_disabled && sdk_wifi_get_opmode() != STATION_MODE)
                wifi_config_softap_stop();

        if (sdk_wifi_station_get_connect_status() == STATION_GOT_IP) {
                if (sdk_wifi_get_opmode() == STATION_MODE && !context->first_time && !s_link_lost)
                        return;
                s_link_lost = false;

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
                if (!context->first_time)
                        s_link_lost = true;

                if (!context->first_time && context->on_event)
                        context->on_event(WIFI_CONFIG_DISCONNECTED);

                // change monitoring poll interval
                xTimerChangePeriod(
                        context->network_monitor_timer,
                        pdMS_TO_TICKS(WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL), 0);

                /* The open setup AP only while there is nothing to connect
                 * to. With credentials stored, a lost link is an outage, not
                 * a setup: keep retrying the station and leave the AP off, so
                 * jamming the real network cannot bring up an open AP that
                 * takes new credentials from anyone in range. A long press of
                 * BOOT clears the credentials and brings the AP back. */
                if (!s_ap_disabled && !wifi_config_has_configuration())
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
        /* Both fields are fixed-length, not strings: a 32-byte SSID and a
           64-digit hex key fill them with no terminator. */
        strncpy((char *)sta_config.sta.ssid, wifi_ssid, sizeof(sta_config.sta.ssid));
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


bool wifi_config_reset_requested(void) {
        /* wifi_config_reset() leaves the key, empty; a device that never had
           credentials through this component has no key at all. */
        size_t required = 0;
        return sysparam_init() == ESP_OK &&
               nvs_get_str(wifi_cfg_handle, "wifi_ssid", NULL, &required) == ESP_OK &&
               required <= 1;
}


esp_err_t wifi_config_reset() {
        esp_err_t r = sysparam_set_string("wifi_ssid", "");     /* the one that counts */
        esp_err_t p = sysparam_set_string("wifi_password", "");
        return r != ESP_OK ? r : p;
}


void wifi_config_get(char **ssid, char **password) {
        if (ssid)
                sysparam_get_string("wifi_ssid", ssid);

        if (password)
                sysparam_get_string("wifi_password", password);
}


esp_err_t wifi_config_set(const char *ssid, const char *password) {
        return store_credentials(ssid, password);
}

