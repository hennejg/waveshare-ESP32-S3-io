#include "app_mqtt.h"
#include "app_config.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "mqtt_client.h"

#define TAG          "app_mqtt"
#define TOPIC_MAXLEN  256   /* prefix(63) + '/'(1) + topic(191) + NUL */

static esp_mqtt_client_handle_t s_client    = NULL;
static volatile bool            s_connected = false;
static app_mqtt_connected_cb_t  s_on_connected    = NULL;
static app_mqtt_msg_cb_t        s_on_msg          = NULL;
static void                   (*s_on_disconnected)(void) = NULL;
static void                   (*s_on_publish_cb)(void)   = NULL;

/* ------------------------------------------------------------------ helpers */

/* Fill buf with "<prefix>/<topic>" or just "<topic>" if prefix is empty
   or the topic is absolute (starts with '/'). */
static void make_topic(const char *topic, char *buf, size_t buf_len)
{
    const char *prefix = app_config_get()->mqtt_topic_prefix;
    if (prefix[0] && topic[0] != '/') {
        snprintf(buf, buf_len, "%s/%s", prefix, topic);
    } else {
        strlcpy(buf, topic, buf_len);
    }
}

/* ------------------------------------------------------------------ events */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        s_connected = true;
        if (s_on_connected) s_on_connected();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        s_connected = false;
        if (s_on_disconnected) s_on_disconnected();
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGD(TAG, "Subscribed, msg_id=%" PRId32, event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGD(TAG, "Unsubscribed, msg_id=%" PRId32, event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "Published, msg_id=%" PRId32, event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        if (s_on_msg) {
            s_on_msg(event->topic, (size_t)event->topic_len,
                     event->data,  (size_t)event->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Transport error: errno %d",
                     event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGE(TAG, "MQTT error type: %d",
                     event->error_handle->error_type);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ public */

esp_err_t app_mqtt_start(void)
{
    if (s_client) return ESP_OK;

    const app_config_t *cfg = app_config_get();
    if (!cfg->mqtt_url[0]) {
        ESP_LOGW(TAG, "No broker URL configured — MQTT disabled");
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg->mqtt_url,
    };

    if (cfg->mqtt_user[0]) {
        mqtt_cfg.credentials.username = cfg->mqtt_user;
    }
    if (cfg->mqtt_password[0]) {
        mqtt_cfg.credentials.authentication.password = cfg->mqtt_password;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_err_t ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Connecting to %s", cfg->mqtt_url);
    return ESP_OK;
}

void app_mqtt_stop(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client    = NULL;
    s_connected = false;
}

bool app_mqtt_is_connected(void)
{
    return s_connected;
}

int app_mqtt_publish(const char *topic, const char *payload, int len,
                     int qos, bool retain)
{
    if (!s_client) return -1;
    char full[TOPIC_MAXLEN];
    make_topic(topic, full, sizeof(full));
    int id = esp_mqtt_client_publish(s_client, full, payload, len, qos, retain);
    if (id >= 0 && s_on_publish_cb) s_on_publish_cb();
    return id;
}

int app_mqtt_subscribe(const char *topic, int qos)
{
    if (!s_client) return -1;
    char full[TOPIC_MAXLEN];
    make_topic(topic, full, sizeof(full));
    return esp_mqtt_client_subscribe(s_client, full, qos);
}

int app_mqtt_unsubscribe(const char *topic)
{
    if (!s_client) return -1;
    char full[TOPIC_MAXLEN];
    make_topic(topic, full, sizeof(full));
    return esp_mqtt_client_unsubscribe(s_client, full);
}

void app_mqtt_set_connected_callback(app_mqtt_connected_cb_t cb)
{
    s_on_connected = cb;
}

void app_mqtt_set_disconnected_callback(void (*cb)(void)) { s_on_disconnected = cb; }
void app_mqtt_set_publish_callback(void (*cb)(void))      { s_on_publish_cb   = cb; }

void app_mqtt_set_msg_callback(app_mqtt_msg_cb_t cb)
{
    s_on_msg = cb;
}
