#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Called on every MQTT_EVENT_CONNECTED (including reconnects).
   Use this to re-subscribe — subscriptions are lost on reconnect. */
typedef void (*app_mqtt_connected_cb_t)(void);

/* Called for each incoming message. topic and data are NOT null-terminated.
   Runs inside the MQTT task — keep it short, no blocking calls. */
typedef void (*app_mqtt_msg_cb_t)(const char *topic, size_t topic_len,
                                   const char *data,  size_t data_len);

esp_err_t app_mqtt_start(void);
void      app_mqtt_stop(void);
bool      app_mqtt_is_connected(void);

/* Publish / subscribe — the configured topic prefix is prepended automatically.
   Pass a topic starting with '/' to use an absolute topic (no prefix added).
   Return value: message ID (>= 0) on success, -1 on error or not connected. */
int app_mqtt_publish(const char *topic, const char *payload, int len,
                     int qos, bool retain);
int app_mqtt_subscribe(const char *topic, int qos);
int app_mqtt_unsubscribe(const char *topic);

void app_mqtt_set_connected_callback(app_mqtt_connected_cb_t cb);
void app_mqtt_set_msg_callback(app_mqtt_msg_cb_t cb);
