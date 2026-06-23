#include "scripting.h"
#include "scripting_priv.h"
#include "quickjs.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <string.h>

// dsl.js is embedded verbatim at build time (EMBED_TXTFILES in CMakeLists.txt), so the
// firmware always runs the canonical engine source — no hand-synced copy to drift.
extern const char dsl_js_start[] asm("_binary_dsl_js_start");

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

typedef enum { EVT_MQTT, EVT_INPUT_CHANGE, EVT_RELOAD, EVT_TIMER } evt_type_t;

typedef struct {
    evt_type_t type;
    union {
        struct { char topic[MAX_TOPIC_LEN]; char payload[MAX_PAYLOAD_LEN]; } mqtt;
        struct { uint8_t channel; bool state; } input;
        struct { char *script; } reload;  // heap-allocated; task frees after eval
        struct { uint32_t id; } timer;     // a rule timer fired (see _set_timer)
    };
} scripting_evt_t;

static QueueHandle_t       s_queue;
static const char         *s_user_script;
const  scripting_io_t     *g_scripting_io;  // used by bindings.c

/* ── Rule timers (.after / .heldFor) ─────────────────────────────────────────
 * dsl.js drives time-based rules through _set_timer(ms, fn) → id and
 * _clear_timer(id). esp_timer callbacks run on the esp_timer task, but QuickJS is
 * single-threaded and may only be touched from the scripting task — so the callback
 * does nothing but post an EVT_TIMER carrying the id; the scripting task looks the id
 * up and invokes the stored JS function. The table is thus only ever accessed from
 * the scripting task (set / clear / EVT_TIMER), so it needs no lock. */
#define MAX_TIMERS 24

typedef struct {
    bool               used;
    uint32_t           id;
    esp_timer_handle_t handle;
    JSValue            fn;     // owned reference (JS_DupValue on set, freed on fire/clear)
} timer_slot_t;

static timer_slot_t s_timers[MAX_TIMERS];
static uint32_t     s_next_timer_id = 1;

static timer_slot_t *timer_find(uint32_t id)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (s_timers[i].used && s_timers[i].id == id) return &s_timers[i];
    return NULL;
}

// esp_timer task context — must NOT touch QuickJS; just hand the id to our task.
static void timer_fired_cb(void *arg)
{
    scripting_evt_t ev = { .type = EVT_TIMER };
    ev.timer.id = (uint32_t)(uintptr_t)arg;
    if (s_queue) xQueueSend(s_queue, &ev, 0);
}

static JSValue js_set_timer(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t ms;
    if (JS_ToInt32(ctx, &ms, argv[0]) < 0) return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "_set_timer: second argument must be a function");
    if (ms < 0) ms = 0;

    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) if (!s_timers[i].used) { slot = i; break; }
    if (slot < 0) {
        ESP_LOGW(TAG, "rule timer table full (%d) — timer dropped", MAX_TIMERS);
        return JS_NewInt32(ctx, -1);
    }

    uint32_t id = s_next_timer_id++;
    if (s_next_timer_id == 0) s_next_timer_id = 1;   // never hand out id 0

    esp_timer_create_args_t args = {
        .callback        = timer_fired_cb,
        .arg             = (void *)(uintptr_t)id,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "rule",
    };
    esp_timer_handle_t h;
    if (esp_timer_create(&args, &h) != ESP_OK)
        return JS_NewInt32(ctx, -1);
    if (esp_timer_start_once(h, (uint64_t)ms * 1000) != ESP_OK) {
        esp_timer_delete(h);
        return JS_NewInt32(ctx, -1);
    }

    s_timers[slot].used   = true;
    s_timers[slot].id     = id;
    s_timers[slot].handle = h;
    s_timers[slot].fn     = JS_DupValue(ctx, argv[1]);
    return JS_NewInt32(ctx, (int32_t)id);
}

static JSValue js_clear_timer(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;
    timer_slot_t *t = timer_find((uint32_t)id);
    if (t) {
        esp_timer_stop(t->handle);
        esp_timer_delete(t->handle);
        JS_FreeValue(ctx, t->fn);
        t->used = false;
    }
    return JS_UNDEFINED;
}

static void register_timer_bindings(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "_set_timer",   JS_NewCFunction(ctx, js_set_timer,   "_set_timer",   2));
    JS_SetPropertyStr(ctx, g, "_clear_timer", JS_NewCFunction(ctx, js_clear_timer, "_clear_timer", 1));
    JS_FreeValue(ctx, g);
}

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
    register_timer_bindings(ctx);
    eval_or_log(ctx, dsl_js_start, "<dsl>");
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
                // _reset_rules() also cancels any pending .after()/.heldFor() timers
                eval_or_log(ctx, "_reset_rules()", "<reload>");
                eval_or_log(ctx, ev.reload.script, "<user>");
                free(ev.reload.script);
                ESP_LOGI(TAG, "Rules reloaded");
                break;
            case EVT_TIMER: {
                timer_slot_t *t = timer_find(ev.timer.id);
                if (t) {                              // ignore if already cleared/reloaded
                    JSValue fn = t->fn;               // take ownership
                    esp_timer_delete(t->handle);      // one-shot has already fired
                    t->used = false;                  // free slot before calling (fn may re-arm)
                    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
                    if (JS_IsException(r)) {
                        JSValue exc = JS_GetException(ctx);
                        const char *msg = JS_ToCString(ctx, exc);
                        ESP_LOGE(TAG, "rule timer error: %s", msg ? msg : "(unknown)");
                        JS_FreeCString(ctx, msg);
                        JS_FreeValue(ctx, exc);
                    }
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, fn);
                }
                break;
            }
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
