#include "app_mqtt.h"
#include "app_config.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#define TAG                  "app_mqtt"
#define TOPIC_MAXLEN          256   /* prefix(63) + '/'(1) + topic(191) + NUL */
#define HEALTH_INTERVAL_US   (60LL * 1000 * 1000)

static esp_mqtt_client_handle_t s_client       = NULL;
static volatile bool            s_connected    = false;
static esp_timer_handle_t       s_health_timer = NULL;
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

/* ------------------------------------------------------------------ health */

static void publish_health(void)
{
    if (!s_client || !s_connected) return;
    const app_config_t *cfg = app_config_get();
    if (!cfg->mqtt_topic_prefix[0]) return;

    char payload[128];
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    snprintf(payload, sizeof(payload),
             "{\"online\":true,\"device\":\"%s\",\"uptime_s\":%" PRId64 "}",
             cfg->device_name, uptime_s);

    esp_mqtt_client_publish(s_client, cfg->mqtt_topic_prefix,
                            payload, 0, /*qos*/1, /*retain*/1);
}

static void health_timer_cb(void *arg) { publish_health(); }

/* ------------------------------------------------------------------ events */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        s_connected = true;
        publish_health();
        if (s_health_timer) {
            esp_timer_stop(s_health_timer);   /* no-op if not running */
            esp_timer_start_periodic(s_health_timer, HEALTH_INTERVAL_US);
        }
        if (s_on_connected) s_on_connected();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        s_connected = false;
        if (s_health_timer) esp_timer_stop(s_health_timer);
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
        /* A payload larger than CONFIG_MQTT_BUFFER_SIZE arrives as several
           events. Only the first carries the topic; the continuations have
           msg_topic == NULL and topic_len == 0, and forwarding those made the
           rule engine run a pass with an empty topic over mid-payload bytes,
           re-evaluating every wildcard rule. Without
           CONFIG_MQTT_TOPIC_PRESENT_ALL_DATA_EVENTS there is no topic to
           reassemble them under, so drop the continuations and say so once. */
        if (event->topic_len == 0 || event->topic == NULL) {
            if (event->current_data_offset == 0) break;   /* not a continuation */
            ESP_LOGW(TAG, "dropping continuation of a %d-byte payload at offset %d"
                          " — raise CONFIG_MQTT_BUFFER_SIZE to receive it whole",
                     event->total_data_len, event->current_data_offset);
            break;
        }
        if (s_on_msg) {
            /* Hand the handlers the topic the way they subscribed to it:
               relative, without the configured prefix. Each handler used to
               match its own suffix against the full topic instead, and the
               suffix match could not tell "<prefix>/output/set" from
               "<prefix>/rules/output/set" -- which arrives through the rule
               engine's "rules/#" subscription and switched every output.
               Stripping once here lets every handler compare the whole
               topic, and lets a rule's mqtt('rules/x') match with a prefix
               set, which it never did. A topic that does not start with the
               prefix (an absolute one, or a stray retained message) is
               passed through unchanged. */
            const char *topic = event->topic;
            size_t      tlen  = (size_t)event->topic_len;
            const char *prefix = app_config_get()->mqtt_topic_prefix;
            size_t      plen   = strlen(prefix);
            if (plen && tlen > plen + 1 &&
                memcmp(topic, prefix, plen) == 0 && topic[plen] == '/') {
                topic += plen + 1;
                tlen  -= plen + 1;
            }
            s_on_msg(topic, tlen, event->data, (size_t)event->data_len);
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
    if (cfg->mqtt_topic_prefix[0]) {
        mqtt_cfg.session.last_will.topic   = cfg->mqtt_topic_prefix;
        mqtt_cfg.session.last_will.msg     = "{\"online\":false}";
        mqtt_cfg.session.last_will.msg_len = 0;   /* null-terminated */
        mqtt_cfg.session.last_will.qos     = 1;
        mqtt_cfg.session.last_will.retain  = 1;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    esp_timer_create_args_t timer_args = {
        .callback = health_timer_cb,
        .name     = "mqtt_health",
    };
    esp_timer_create(&timer_args, &s_health_timer);

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
    if (s_health_timer) {
        esp_timer_stop(s_health_timer);
        esp_timer_delete(s_health_timer);
        s_health_timer = NULL;
    }
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
