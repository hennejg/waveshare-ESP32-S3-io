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
#include <sys/time.h>
#include <stdbool.h>

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

/* Upper bound on how long one event may spend inside JavaScript. Without it an
   endless loop in a user script stops the engine for the rest of the boot --
   and because the script is persisted before it runs, for every boot after
   that too. See components/scripting/Kconfig. */
#define TIME_BUDGET_US   ((int64_t)CONFIG_SCRIPTING_TIME_BUDGET_MS * 1000)
#define JS_STACK_LIMIT   (16 * 1024)

// Max topic / payload through the event queue; larger messages are truncated.
#define MAX_TOPIC_LEN    128
#define MAX_PAYLOAD_LEN  256

// Subscribes to every topic under this prefix when MQTT connects.
#define RULES_TOPIC      "rules/#"

/* ── Event queue ─────────────────────────────────────────────────────────── */

typedef enum { EVT_MQTT, EVT_INPUT_CHANGE, EVT_RELOAD, EVT_TIMER, EVT_TIME_SYNC,
               EVT_ACTIVITY } evt_type_t;

// Fieldbus command-activity sources (DSL _on_activity key); see scripting_on_*_activity.
#define ACT_MODBUS 0
#define ACT_CAN    1

typedef struct {
    evt_type_t type;
    union {
        struct { char topic[MAX_TOPIC_LEN]; char payload[MAX_PAYLOAD_LEN]; } mqtt;
        struct { uint8_t channel; bool state; } input;
        struct { char *script; } reload;  // heap-allocated; task frees after eval
        struct { uint32_t id; } timer;     // a rule timer fired (see _set_timer)
        struct { uint8_t source; } activity;  // ACT_MODBUS / ACT_CAN
    };
} scripting_evt_t;

static QueueHandle_t       s_queue;
static const char         *s_user_script;
const  scripting_io_t     *g_scripting_io;  // used by bindings.c

/* Wall-clock validity: false until the system clock holds real time (RTC seed at boot
 * or an SNTP sync), at which point cron may safely arm. Set before the scripting task
 * starts (RTC path) or from the EVT_TIME_SYNC handler (SNTP path) — both on or before
 * the scripting task, so the bool is only ever touched there; no lock needed. */
static bool s_time_valid = false;

/* ── Rule timers (.after / .heldFor) ─────────────────────────────────────────
 * dsl.js drives time-based rules through _set_timer(ms, fn) → id and
 * _clear_timer(id). esp_timer callbacks run on the esp_timer task, but QuickJS is
 * single-threaded and may only be touched from the scripting task — so the callback
 * does nothing but post an EVT_TIMER carrying the id; the scripting task looks the id
 * up and invokes the stored JS function. The table is thus only ever accessed from
 * the scripting task (set / clear / EVT_TIMER), so it needs no lock. */
/* Two generations of rules share this table during a reload: the running set
   still holds its slots while the new script is being evaluated, so it has to
   be big enough for both or the new script quietly fails to arm. */
#define MAX_TIMERS 48

typedef struct {
    bool               used;
    uint32_t           id;
    esp_timer_handle_t handle;
    JSValue            fn;     // owned reference (JS_DupValue on set, freed on fire/clear)
} timer_slot_t;

static timer_slot_t s_timers[MAX_TIMERS];
static uint32_t     s_next_timer_id = 1;
/* Counts refusals, so a reload can tell whether the new script got every
   timer it asked for. */
static uint32_t     s_timer_full_count;

/* Result of the last finished reload, so the HTTP handler can report what
   actually happened instead of acknowledging a request it cannot see through.
   Guarded because the engine writes it while the web server reads it. */
static scripting_reload_status_t s_reload_status;
static SemaphoreHandle_t         s_reload_lock;

static void publish_reload_result(bool ok, const char *message)
{
    if (s_reload_lock) xSemaphoreTake(s_reload_lock, portMAX_DELAY);
    s_reload_status.ok = ok;
    strlcpy(s_reload_status.message, message ? message : "", sizeof(s_reload_status.message));
    s_reload_status.generation++;
    if (s_reload_lock) xSemaphoreGive(s_reload_lock);
}

void scripting_reload_status(scripting_reload_status_t *out)
{
    if (!out) return;
    if (s_reload_lock) xSemaphoreTake(s_reload_lock, portMAX_DELAY);
    *out = s_reload_status;
    if (s_reload_lock) xSemaphoreGive(s_reload_lock);
}

static timer_slot_t *timer_find(uint32_t id)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (s_timers[i].used && s_timers[i].id == id) return &s_timers[i];
    return NULL;
}

/* Timer ids that did not fit in the event queue. Losing one is not a missed
   tick but a permanent defect: the slot stays used, its esp_timer is never
   deleted and its JS function never released, so an .after() off-pulse never
   switches off, a watchdog() never expires, an every() never re-arms -- and
   after MAX_TIMERS such losses no rule can arm a timer at all.

   Single producer (the esp_timer task) and single consumer (the scripting
   task), so head and tail need no lock. */
#define TIMER_OVERFLOW_SLOTS (MAX_TIMERS + 1)   /* one slot is the empty marker */
static volatile uint32_t s_overflow[TIMER_OVERFLOW_SLOTS];
static volatile uint8_t  s_of_head, s_of_tail;

// esp_timer task context — must NOT touch QuickJS; just hand the id to our task.
static void timer_fired_cb(void *arg)
{
    scripting_evt_t ev = { .type = EVT_TIMER };
    ev.timer.id = (uint32_t)(uintptr_t)arg;

    if (s_queue && xQueueSend(s_queue, &ev, 0) == pdTRUE) return;

    uint8_t next = (uint8_t)((s_of_head + 1u) % TIMER_OVERFLOW_SLOTS);
    if (next == s_of_tail) {                 /* even the overflow ring is full */
        ESP_LOGE(TAG, "rule timer %u lost: event queue and overflow both full",
                 (unsigned)ev.timer.id);
        return;
    }
    s_overflow[s_of_head] = ev.timer.id;
    s_of_head = next;
    ESP_LOGW(TAG, "event queue full - rule timer %u deferred",
             (unsigned)ev.timer.id);
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
        s_timer_full_count++;
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

// Wall-clock epoch in ms (UTC), straight from gettimeofday — the explicit time source
// for cron, instead of relying on QuickJS Date.now()'s own gettimeofday wiring.
static JSValue js_now(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double ms = (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
    return JS_NewFloat64(ctx, ms);
}

// True once the clock holds real wall-clock time; dsl.js gates cron arming on this.
static JSValue js_time_valid(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    return JS_NewBool(ctx, s_time_valid);
}

static void register_timer_bindings(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "_set_timer",   JS_NewCFunction(ctx, js_set_timer,   "_set_timer",   2));
    JS_SetPropertyStr(ctx, g, "_clear_timer", JS_NewCFunction(ctx, js_clear_timer, "_clear_timer", 1));
    JS_SetPropertyStr(ctx, g, "_now",         JS_NewCFunction(ctx, js_now,         "_now",         0));
    JS_SetPropertyStr(ctx, g, "_time_valid",  JS_NewCFunction(ctx, js_time_valid,  "_time_valid",  0));
    JS_FreeValue(ctx, g);
}

/* ── PSRAM allocator for the QuickJS heap ────────────────────────────────── */

/* Custom allocator -- prefixed s3_ to avoid clashing with quickjs.h's js_malloc.
   JS_SetMemoryLimit() only stores the limit; QuickJS expects the allocator to
   enforce it and to maintain malloc_size/malloc_count, exactly as js_def_malloc
   does. Without that the limit has no effect and the GC threshold, which is
   derived from malloc_size, never triggers either -- a script could take the
   shared PSRAM rather than its own budget. */
static void *s3_js_malloc(JSMallocState *s, size_t n)
{
    if (s->malloc_size + n > s->malloc_limit) return NULL;
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    s->malloc_count++;
    s->malloc_size += heap_caps_get_allocated_size(p);
    return p;
}

static void s3_js_free(JSMallocState *s, void *p)
{
    if (!p) return;
    s->malloc_count--;
    s->malloc_size -= heap_caps_get_allocated_size(p);
    free(p);
}

static void *s3_js_realloc(JSMallocState *s, void *p, size_t n)
{
    size_t old_size = p ? heap_caps_get_allocated_size(p) : 0;

    if (!n) {                      /* realloc(p, 0) frees */
        if (p) { s->malloc_count--; s->malloc_size -= old_size; free(p); }
        return NULL;
    }
    if (!p) return s3_js_malloc(s, n);
    if (s->malloc_size + n - old_size > s->malloc_limit) return NULL;

    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!q) return NULL;           /* p stays valid and accounted for */
    s->malloc_size += heap_caps_get_allocated_size(q) - old_size;
    return q;
}

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

/* Same as eval_or_log(), but says whether it worked and hands back the error
   so the caller can report it rather than only logging it. */
static bool eval_check(JSContext *ctx, const char *src, const char *name,
                       char *err, size_t err_len)
{
    JSValue result = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(result);
    if (!ok) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        ESP_LOGE(TAG, "JS error in %s: %s", name, msg ? msg : "(unknown)");
        if (err && err_len) strlcpy(err, msg ? msg : "(unknown)", err_len);
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
    return ok;
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

/* ── Execution time budget ──────────────────────────────────────────────── */

static int64_t s_deadline_us;   /* 0 = no budget in force */

/* QuickJS polls this while interpreting; a non-zero answer throws
   InternalError at that point, unwinding whatever the script was doing. */
static int js_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    if (!s_deadline_us) return 0;
    return esp_timer_get_time() > s_deadline_us;
}

/* Called before every entry into JS. The deadline has to be renewed per event,
   not per boot: once it has passed, every further call would throw at once. */
static inline void budget_start(void)
{
    s_deadline_us = TIME_BUDGET_US ? esp_timer_get_time() + TIME_BUDGET_US : 0;
}

/* Runs one elapsed timer. Reached from the event queue and, when that was
   full, from the overflow ring. */
static void run_timer(JSContext *ctx, uint32_t id)
{
    timer_slot_t *t = timer_find(id);
    if (!t) return;                       // already cleared or belongs to a replaced context

    JSValue fn = t->fn;                   // take ownership
    esp_timer_delete(t->handle);          // one-shot has already fired
    t->used = false;                      // free slot before calling (fn may re-arm)

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

static void drain_overflow(JSContext *ctx)
{
    while (s_of_tail != s_of_head) {
        uint32_t id = s_overflow[s_of_tail];
        s_of_tail = (uint8_t)((s_of_tail + 1u) % TIMER_OVERFLOW_SLOTS);
        budget_start();
        run_timer(ctx, id);
    }
}

/* Release the slots marked in `mine`, freeing their JS references against the
   context they were created in. */
static void clear_timers(JSContext *ctx, const bool *mine)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!mine[i] || !s_timers[i].used) continue;
        esp_timer_stop(s_timers[i].handle);
        esp_timer_delete(s_timers[i].handle);
        JS_FreeValue(ctx, s_timers[i].fn);
        s_timers[i].used = false;
    }
}

static void bind_handlers(JSContext *ctx, JSValue *mqtt, JSValue *input,
                          JSValue *time_sync, JSValue *activity)
{
    JSValue global = JS_GetGlobalObject(ctx);
    *mqtt      = JS_GetPropertyStr(ctx, global, "_on_mqtt");
    *input     = JS_GetPropertyStr(ctx, global, "_on_input");
    *time_sync = JS_GetPropertyStr(ctx, global, "_on_time_sync");
    *activity  = JS_GetPropertyStr(ctx, global, "_on_activity");
    JS_FreeValue(ctx, global);
}

/* ── Scripting task ──────────────────────────────────────────────────────── */

static void scripting_task(void *arg)
{
    JSRuntime *rt = JS_NewRuntime2(&s_psram_alloc, NULL);
    JS_SetMemoryLimit(rt, JS_HEAP_LIMIT);
    JS_SetMaxStackSize(rt, JS_STACK_LIMIT);
    JS_SetGCThreshold(rt, 256 * 1024);
    JS_SetInterruptHandler(rt, js_interrupt, NULL);

    JSContext *ctx = JS_NewContext(rt);

    scripting_register_bindings(ctx);
    register_timer_bindings(ctx);
    budget_start();
    eval_or_log(ctx, dsl_js_start, "<dsl>");
    budget_start();
    eval_or_log(ctx, s_user_script, "<user>");
    drain_jobs(rt);

    JSValue on_mqtt, on_input, on_time_sync, on_activity;
    bind_handlers(ctx, &on_mqtt, &on_input, &on_time_sync, &on_activity);

    ESP_LOGI(TAG, "Rule engine ready. Free heap: %lu B  SPIRAM: %lu B",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    scripting_evt_t ev;
    for (;;) {
        drain_overflow(ctx);   /* timers the event queue could not take */

        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
            budget_start();
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
            case EVT_RELOAD: {
                /* A reload gets its own JSContext instead of being evaluated on
                   top of the old one. Re-evaluating in the same context kept
                   the previous script's global lexical bindings, so a second
                   save of a script containing a top-level `const` failed with
                   "redeclaration of ..." -- a compile error, so nothing of the
                   new script ran, and the old rules had already been cleared.
                   It also meant a user script could overwrite _reset_rules or a
                   DSL prototype and keep that for the rest of the boot.

                   The new context is built and the script evaluated into it
                   first. Only if that works is it swapped in; otherwise the old
                   one carries on serving the rules it already has. */
                char err[96] = "";
                JSContext *nctx = JS_NewContext(rt);
                if (!nctx) {
                    ESP_LOGE(TAG, "reload: out of memory, rules unchanged");
                    free(ev.reload.script);
                    publish_reload_result(false, "out of memory");
                    break;
                }

                /* Timers already armed belong to the old context; remember them
                   so the right set is released whichever way this goes. */
                bool was_used[MAX_TIMERS];
                for (int i = 0; i < MAX_TIMERS; i++) was_used[i] = s_timers[i].used;

                scripting_register_bindings(nctx);
                register_timer_bindings(nctx);

                uint32_t full_before = s_timer_full_count;

                budget_start();
                bool ok = eval_check(nctx, dsl_js_start, "<dsl>", err, sizeof(err));
                if (ok) {
                    budget_start();   /* the user script gets a budget of its own */
                    ok = eval_check(nctx, ev.reload.script, "<user>", err, sizeof(err));
                }
                free(ev.reload.script);

                /* The running rules still hold their slots while this was
                   evaluated. If the table ran out, the new script is missing
                   timers it asked for -- an every() that never ticks, a
                   watchdog that never expires. Refuse the reload rather than
                   install a ruleset that is quietly incomplete. */
                if (ok && s_timer_full_count != full_before) {
                    ESP_LOGE(TAG, "reload needs more timers than are free (%d total)",
                             MAX_TIMERS);
                    snprintf(err, sizeof(err),
                             "more timers needed than are free (%d total)", MAX_TIMERS);
                    ok = false;
                }

                if (!ok) {
                    /* Release what the failed script managed to arm, keep the
                       running rules. */
                    bool theirs[MAX_TIMERS];
                    for (int i = 0; i < MAX_TIMERS; i++)
                        theirs[i] = !was_used[i] && s_timers[i].used;
                    clear_timers(nctx, theirs);
                    JS_FreeContext(nctx);
                    ESP_LOGE(TAG, "rules not applied, previous rules still running");
                    publish_reload_result(false, err);
                    break;
                }

                drain_jobs(rt);
                clear_timers(ctx, was_used);          /* old context's timers */
                JS_FreeValue(ctx, on_mqtt);
                JS_FreeValue(ctx, on_input);
                JS_FreeValue(ctx, on_time_sync);
                JS_FreeValue(ctx, on_activity);
                JS_FreeContext(ctx);

                ctx = nctx;
                bind_handlers(ctx, &on_mqtt, &on_input, &on_time_sync, &on_activity);
                ESP_LOGI(TAG, "Rules reloaded");
                publish_reload_result(true, "");
                break;
            }
            case EVT_TIMER:
                run_timer(ctx, ev.timer.id);
                break;
            case EVT_TIME_SYNC:
                // Clock just became real (first SNTP sync, or corrected by a later one).
                // Mark valid and let dsl.js re-arm cron from the corrected wall-clock.
                s_time_valid = true;
                if (JS_IsFunction(ctx, on_time_sync)) {
                    JSValue r = JS_Call(ctx, on_time_sync, JS_UNDEFINED, 0, NULL);
                    if (JS_IsException(r)) {
                        JSValue exc = JS_GetException(ctx);
                        const char *msg = JS_ToCString(ctx, exc);
                        ESP_LOGE(TAG, "_on_time_sync error: %s", msg ? msg : "(unknown)");
                        JS_FreeCString(ctx, msg);
                        JS_FreeValue(ctx, exc);
                    }
                    JS_FreeValue(ctx, r);
                }
                break;
            case EVT_ACTIVITY: {
                // A fieldbus command arrived → feed the matching DSL command-health source.
                const char *src = (ev.activity.source == ACT_CAN) ? "can" : "modbus";
                call2(ctx, on_activity, JS_NewString(ctx, src), JS_UNDEFINED);
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

    s_reload_lock = xSemaphoreCreateMutex();
    if (!s_reload_lock) {
        ESP_LOGE(TAG, "Failed to create reload status lock");
        return ESP_ERR_NO_MEM;
    }

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

void scripting_set_time_valid(void)
{
    // Boot path: the RTC seeded real time before the scripting task started, so cron
    // may arm at load. Just flip the flag the _time_valid() binding reports.
    s_time_valid = true;
}

void scripting_on_time_sync(void)
{
    // Runtime path: an SNTP sync stepped the clock. Tell the scripting task to re-arm
    // cron from the corrected wall-clock (it also sets s_time_valid). If the engine
    // isn't up yet, the boot-time _time_valid() read covers arming.
    if (!s_queue) { s_time_valid = true; return; }
    scripting_evt_t ev = { .type = EVT_TIME_SYNC };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE)
        ESP_LOGW(TAG, "scripting_on_time_sync: queue full");
}

static void post_activity(uint8_t source)
{
    if (!s_queue) return;
    scripting_evt_t ev = { .type = EVT_ACTIVITY, .activity = { .source = source } };
    // Drop silently if the queue is full — a missed health feed only risks a spurious
    // "stale" edge, and flooding the log from a busy RX task would be worse.
    xQueueSend(s_queue, &ev, 0);
}

void scripting_on_modbus_activity(void) { post_activity(ACT_MODBUS); }
void scripting_on_can_activity(void)    { post_activity(ACT_CAN); }

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
