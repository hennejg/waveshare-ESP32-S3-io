#include "scripting.h"
#include "scripting_priv.h"
#include "dsl.js.h"
#include "quickjs.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <string.h>

#define TAG              "scripting"

// Stack > SPIRAM_MALLOC_ALWAYSINTERNAL (4 KiB) → auto-allocated in PSRAM.
#define TASK_STACK_BYTES (48 * 1024)
#define TASK_PRIORITY    3
#define QUEUE_DEPTH      16

// QuickJS heap allocated from PSRAM via custom allocator.
#define JS_HEAP_LIMIT    (1 * 1024 * 1024)
#define JS_STACK_LIMIT   (16 * 1024)

// Max topic / payload through the event queue; larger messages are truncated.
#define MAX_TOPIC_LEN    128
#define MAX_PAYLOAD_LEN  256

// Subscribes to every topic under this prefix when MQTT connects.
#define RULES_TOPIC      "rules/#"

/* ── Event queue ─────────────────────────────────────────────────────────── */

typedef enum { EVT_MQTT, EVT_INPUT_CHANGE, EVT_RELOAD } evt_type_t;

typedef struct {
    evt_type_t type;
    union {
        struct { char topic[MAX_TOPIC_LEN]; char payload[MAX_PAYLOAD_LEN]; } mqtt;
        struct { uint8_t channel; bool state; } input;
        struct { char *script; } reload;  // heap-allocated; task frees after eval
    };
} scripting_evt_t;

static QueueHandle_t       s_queue;
static const char         *s_user_script;
const  scripting_io_t     *g_scripting_io;  // used by bindings.c

/* ── PSRAM allocator for the QuickJS heap ────────────────────────────────── */

// Custom allocator — prefixed s3_ to avoid clashing with quickjs.h's js_malloc etc.
static void *s3_js_malloc(JSMallocState *s, size_t n)
    { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static void s3_js_free(JSMallocState *s, void *p)
    { free(p); }

static void *s3_js_realloc(JSMallocState *s, void *p, size_t n)
    { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static size_t s3_js_usable_size(const void *p)
    { return heap_caps_get_allocated_size((void *)p); }

static const JSMallocFunctions s_psram_alloc = {
    s3_js_malloc, s3_js_free, s3_js_realloc, s3_js_usable_size,
};

/* ── JS helpers ──────────────────────────────────────────────────────────── */

static void drain_jobs(JSRuntime *rt)
{
    JSContext *ctx2;
    while (JS_ExecutePendingJob(rt, &ctx2) > 0) {}
}

static void eval_or_log(JSContext *ctx, const char *src, const char *name)
{
    JSValue result = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        ESP_LOGE(TAG, "JS error in %s: %s", name, msg ? msg : "(unknown)");
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
}

// Call a two-argument JS function; takes ownership of a0 and a1.
static void call2(JSContext *ctx, JSValue fn, JSValue a0, JSValue a1)
{
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, a0);
        JS_FreeValue(ctx, a1);
        return;
    }
    JSValue argv[2] = { a0, a1 };
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 2, argv);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        ESP_LOGE(TAG, "JS callback error: %s", msg ? msg : "(unknown)");
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, a0);
    JS_FreeValue(ctx, a1);
}

/* ── Scripting task ──────────────────────────────────────────────────────── */

static void scripting_task(void *arg)
{
    JSRuntime *rt = JS_NewRuntime2(&s_psram_alloc, NULL);
    JS_SetMemoryLimit(rt, JS_HEAP_LIMIT);
    JS_SetMaxStackSize(rt, JS_STACK_LIMIT);
    JS_SetGCThreshold(rt, 256 * 1024);

    JSContext *ctx = JS_NewContext(rt);

    scripting_register_bindings(ctx);
    eval_or_log(ctx, DSL_JS_SOURCE, "<dsl>");
    eval_or_log(ctx, s_user_script, "<user>");
    drain_jobs(rt);

    JSValue global   = JS_GetGlobalObject(ctx);
    JSValue on_mqtt  = JS_GetPropertyStr(ctx, global, "_on_mqtt");
    JSValue on_input = JS_GetPropertyStr(ctx, global, "_on_input");
    JS_FreeValue(ctx, global);

    ESP_LOGI(TAG, "Rule engine ready. Free heap: %lu B  SPIRAM: %lu B",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    scripting_evt_t ev;
    for (;;) {
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
            switch (ev.type) {
            case EVT_MQTT:
                call2(ctx, on_mqtt,
                      JS_NewString(ctx, ev.mqtt.topic),
                      JS_NewString(ctx, ev.mqtt.payload));
                break;
            case EVT_INPUT_CHANGE:
                call2(ctx, on_input,
                      JS_NewInt32(ctx, ev.input.channel),
                      JS_NewBool(ctx,  ev.input.state));
                break;
            case EVT_RELOAD:
                eval_or_log(ctx, "_rules=[]", "<reload>");
                eval_or_log(ctx, ev.reload.script, "<user>");
                free(ev.reload.script);
                ESP_LOGI(TAG, "Rules reloaded");
                break;
            }
            drain_jobs(rt);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t scripting_init(const char *user_script, const scripting_io_t *io)
{
    s_user_script  = user_script;
    g_scripting_io = io;

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(scripting_evt_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(scripting_task, "scripting",
                                TASK_STACK_BYTES / sizeof(StackType_t),
                                NULL, TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        ESP_LOGE(TAG, "Failed to create scripting task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void scripting_on_mqtt_connected(void)
{
    if (g_scripting_io && g_scripting_io->mqtt_subscribe)
        g_scripting_io->mqtt_subscribe(RULES_TOPIC, 0);
}

void scripting_on_mqtt_message(const char *topic, size_t tlen,
                                const char *data,  size_t dlen)
{
    if (!s_queue) return;

    scripting_evt_t ev = { .type = EVT_MQTT };
    size_t tl = tlen < sizeof(ev.mqtt.topic)   - 1 ? tlen : sizeof(ev.mqtt.topic)   - 1;
    size_t dl = dlen < sizeof(ev.mqtt.payload) - 1 ? dlen : sizeof(ev.mqtt.payload) - 1;

    if (tlen > tl || dlen > dl)
        ESP_LOGW(TAG, "MQTT msg truncated (%zu/%zu → %zu/%zu)", tlen, dlen, tl, dl);

    memcpy(ev.mqtt.topic,   topic, tl); ev.mqtt.topic[tl]   = '\0';
    memcpy(ev.mqtt.payload, data,  dl); ev.mqtt.payload[dl] = '\0';

    if (xQueueSend(s_queue, &ev, 0) != pdTRUE)
        ESP_LOGW(TAG, "Event queue full — MQTT message dropped");
}

void scripting_reload(const char *new_script)
{
    if (!s_queue || !new_script) return;
    char *copy = strdup(new_script);
    if (!copy) { ESP_LOGE(TAG, "scripting_reload: out of memory"); return; }
    scripting_evt_t ev = { .type = EVT_RELOAD };
    ev.reload.script = copy;
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "scripting_reload: queue full");
        free(copy);
    }
}

void scripting_on_input_change(uint8_t channel, bool state)
{
    if (!s_queue) return;
    scripting_evt_t ev = {
        .type  = EVT_INPUT_CHANGE,
        .input = { .channel = channel, .state = state },
    };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE)
        ESP_LOGW(TAG, "Event queue full — input change dropped");
}
