#include "app_mqtt.h"
#include "app_config.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "cJSON.h"

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

    /* Built by the serializer: the device name is free text, and pasted
       into the JSON as it was, a name like  Pump "West"  made the message
       unparseable for every subscriber. */
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddBoolToObject(o, "online", true);
    cJSON_AddStringToObject(o, "device", cfg->device_name);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    char *payload = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!payload) return;

    esp_mqtt_client_publish(s_client, cfg->mqtt_topic_prefix,
                            payload, 0, /*qos*/1, /*retain*/1);
    cJSON_free(payload);
}

/* The timer only rings; the publish runs on a task of its own. Publishing
   takes the client's API lock and may wait on the network, and esp_timer
   runs every callback in the system on one task in turn: a stalled publish
   held up rule timers, the auth timeout and every other timer with it. */
static TaskHandle_t s_health_task;

/* Held by health_task around its publish and by app_mqtt_stop() around the
   destroy, so the task can never run esp_mqtt_client_publish() on a client
   that stop() has freed between the NULL check and the call. */
static SemaphoreHandle_t s_client_lock;

static void health_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_client_lock, portMAX_DELAY);
        publish_health();
        xSemaphoreGive(s_client_lock);
    }
}

static void health_timer_cb(void *arg)
{
    if (s_health_task) xTaskNotifyGive(s_health_task);
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

    /* The lock and the task outlive the client: stop() leaves them, start()
       reuses them. The timer is created once too and deleted by stop(). */
    if (!s_client_lock) s_client_lock = xSemaphoreCreateMutex();
    if (!s_client_lock) {
        ESP_LOGE(TAG, "Failed to create client lock");
        return ESP_ERR_NO_MEM;
    }
    /* The publish runs the transport write on this stack; with an mqtts://
       or wss:// URL that is an mbedtls record write. */
    if (!s_health_task &&
        xTaskCreate(health_task, "mqtt_health", 4096, NULL, 3,
                    &s_health_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create health task");
        return ESP_ERR_NO_MEM;
    }
    if (!s_health_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = health_timer_cb,
            .name     = "mqtt_health",
        };
        esp_err_t ret = esp_timer_create(&timer_args, &s_health_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create health timer: %s", esp_err_to_name(ret));
            return ret;
        }
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
    if (s_health_timer) {
        esp_timer_stop(s_health_timer);
        esp_timer_delete(s_health_timer);
        s_health_timer = NULL;
    }
    /* Stop first, outside the lock: the mqtt task may be inside the inline
       publish_health() of MQTT_EVENT_CONNECTED, and stop() waits for it to
       exit. Once it has, only health_task can still reach s_client. */
    esp_mqtt_client_stop(s_client);
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    esp_mqtt_client_destroy(s_client);
    s_client    = NULL;
    s_connected = false;
    xSemaphoreGive(s_client_lock);
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
