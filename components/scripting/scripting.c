#include "scripting.h"
#include "scripting_priv.h"
#include "quickjs.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
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

/* In PSRAM (xTaskCreateWithCaps): internal RAM is what the board runs short
   of. QuickJS recurses in C per JS call, about 7 KB per step of an output
   cascade, so this holds CASCADE_CAP (16) steps. NVS writes cannot run on a
   PSRAM stack: storing the rules goes through store_task. */
#define TASK_STACK_BYTES (160 * 1024)
#define TASK_PRIORITY    3
#define QUEUE_DEPTH      16

// QuickJS heap allocated from PSRAM via custom allocator.
#define JS_HEAP_LIMIT    (1 * 1024 * 1024)
/* Room to work kept free above what a reload leaves in use. */
#define HANDOVER_SLACK   (64 * 1024)

/* Upper bound on how long one event may spend inside JavaScript. Without it an
   endless loop in a user script stops the engine for the rest of the boot --
   and because the script is persisted before it runs, for every boot after
   that too. See components/scripting/Kconfig. */
#define TIME_BUDGET_US   ((int64_t)CONFIG_SCRIPTING_TIME_BUDGET_MS * 1000)
/* What QuickJS may use of the task's stack; the rest is for the C below
   the deepest JS frame -- a binding publishing over MQTT/TLS, a log line. */
#define JS_STACK_LIMIT   (TASK_STACK_BYTES - 16 * 1024)

// Max topic / payload through the event queue, terminator included; larger
// messages are dropped (see scripting_on_mqtt_message()).
#define MAX_TOPIC_LEN    128
#define MAX_PAYLOAD_LEN  256

// Subscribes to every topic under this prefix when MQTT connects.
#define RULES_TOPIC      "rules/#"

/* ── Event queue ─────────────────────────────────────────────────────────── */

typedef enum { EVT_MQTT, EVT_INPUT_CHANGE, EVT_RELOAD, EVT_TIMER, EVT_TIME_SYNC,
               EVT_ACTIVITY, EVT_TZ_CHANGE, EVT_ANALOG } evt_type_t;

/* Input changes the event queue could not take, one bit per channel. Set by
   whoever reports the change, cleared by the engine when it resyncs. */
static volatile uint32_t s_input_lost;

// Fieldbus command-activity sources (DSL _on_activity key); see scripting_on_*_activity.
#define ACT_MODBUS 0
#define ACT_CAN    1

typedef struct {
    evt_type_t type;
    uint64_t   seq;     /* order of posting, see evt_send() */
    union {
        struct { char topic[MAX_TOPIC_LEN]; char payload[MAX_PAYLOAD_LEN]; uint16_t plen; } mqtt;
        struct { uint8_t channel; bool state; } input;
        struct { char *script; uint32_t ticket;            // script heap-allocated; task frees after eval
                 char *store; scripting_store_fn_t store_fn; } reload;  // store: heap, NULL = not stored
        struct { uint32_t id; } timer;     // a rule timer fired (see _set_timer)
        struct { uint8_t source; } activity;  // ACT_MODBUS / ACT_CAN
        struct { uint8_t pad; int32_t mv; } analog;
    };
} scripting_evt_t;

/* Every event is numbered as it is posted. What cannot go through the queue
   -- a timer when it is full -- takes the number too, and waits until the
   events posted before it have been handled: run ahead of them, a heldFor()
   timer checked an input whose LOW was still in the queue, saw the HIGH
   before it, and switched the output on.

   Numbering and queueing happen under one lock. Taken one after the other, a
   producer could be preempted between them and another queue a later number
   first: queue 6, 4 with timer 5 deferred -- head 6 is past 5, the timer ran,
   and LOW 4 behind it came too late. Under the lock the queue holds the
   numbers in order. Every producer is a task (the timers dispatch on the
   esp_timer task, the inputs on di_task), none an ISR.

   64 bits wide, so no comparison ever wraps: at 32 bits a channel whose mark
   was never set took every event past 2^31 for stale. */
static SemaphoreHandle_t s_send_lock;
static uint64_t          s_evt_seq;       /* last number handed out, under s_send_lock */

static inline bool evt_seq_after(uint64_t a, uint64_t b) { return a > b; }

static QueueHandle_t       s_queue;

static BaseType_t evt_send(scripting_evt_t *ev)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    ev->seq = ++s_evt_seq;
    BaseType_t r = xQueueSend(s_queue, ev, 0);
    xSemaphoreGive(s_send_lock);
    return r;
}

static uint64_t evt_seq_now(void)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    uint64_t v = s_evt_seq;
    xSemaphoreGive(s_send_lock);
    return v;
}

/* Input events and resync. A resync reads the live level of a channel whose
   change was dropped -- but events older than that read may still be waiting
   in the queue, and delivered afterwards one of them would overwrite the level
   just resynced: a HIGH from before a dropped LOW put the rule engine back on
   HIGH with the pin LOW, and a heldFor() on it fired. So a resync remembers,
   per channel, the last number handed out BEFORE it reads the level, and an
   input event at or below that mark is stale. */
static uint64_t s_input_mark[32];          /* scripting task only */

static inline void input_mark_resync(uint8_t ch)
{
    s_input_mark[ch & 31] = evt_seq_now();
}

static inline bool input_event_stale(uint8_t ch, uint64_t seq)
{
    return seq <= s_input_mark[ch & 31];
}

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

static uint32_t s_reload_seq;           /* last ticket handed out, under s_reload_lock */

static void publish_reload_result(uint32_t ticket, bool ok, const char *message,
                                  bool store_tried, esp_err_t store_err)
{
    if (s_reload_lock) xSemaphoreTake(s_reload_lock, portMAX_DELAY);
    s_reload_status.ok = ok;
    s_reload_status.store_tried = store_tried;
    s_reload_status.store_err   = store_err;
    strlcpy(s_reload_status.message, message ? message : "", sizeof(s_reload_status.message));
    s_reload_status.ticket = ticket;
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
static uint64_t          s_overflow_seq[TIMER_OVERFLOW_SLOTS];   /* its event number */
static volatile uint8_t  s_of_head, s_of_tail;

// esp_timer task context — must NOT touch QuickJS; just hand the id to our task.
static void timer_fired_cb(void *arg)
{
    scripting_evt_t ev = { .type = EVT_TIMER };
    ev.timer.id = (uint32_t)(uintptr_t)arg;

    if (s_queue && evt_send(&ev) == pdTRUE) return;

    uint8_t next = (uint8_t)((s_of_head + 1u) % TIMER_OVERFLOW_SLOTS);
    if (next == s_of_tail) {                 /* even the overflow ring is full */
        ESP_LOGE(TAG, "rule timer %u lost: event queue and overflow both full",
                 (unsigned)ev.timer.id);
        return;
    }
    s_overflow[s_of_head]     = ev.timer.id;
    s_overflow_seq[s_of_head] = ev.seq;    /* numbered by the failed evt_send() */
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the entry before the head that publishes it */
    s_of_head = next;
    ESP_LOGW(TAG, "event queue full - rule timer %u deferred",
             (unsigned)ev.timer.id);
}

/* Longer than the five years _cronNext() looks ahead, so no cron delay is ever
   cut short -- a timer that fires early runs its action unconditionally. */
#define TIMER_MAX_MS (6.0 * 366 * 24 * 3600 * 1000)

static JSValue js_set_timer(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    /* As a double: a delay to the first of next month is 2.6e9 ms, past
       what an int32 holds, and the truncation turned it negative, then
       zero -- the cron action ran at once and rescheduled itself at once. */
    double ms;
    if (JS_ToFloat64(ctx, &ms, argv[0]) < 0) return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "_set_timer: second argument must be a function");
    if (!(ms >= 0)) ms = 0;                       /* negative, or NaN */
    if (ms > TIMER_MAX_MS) ms = TIMER_MAX_MS;

    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) if (!s_timers[i].used) { slot = i; break; }
    /* Logged once per spell of a full table: dsl.js tries refused timers
       again on every event and every freed slot, and a line per refusal on
       the console held the engine up for seconds under load. */
    static bool full_logged;
    if (slot < 0) {
        s_timer_full_count++;
        if (!full_logged) {
            full_logged = true;
            ESP_LOGW(TAG, "rule timer table full (%d) — timers wait for a free slot", MAX_TIMERS);
        }
        return JS_NewInt32(ctx, -1);
    }
    full_logged = false;

    /* The id reaches JS as an int32, and dsl.js reads a negative one as
       "refused": from 2^31 on, every timer was armed in C but recorded as
       never scheduled, so nothing could cancel it -- a watchdog fed on time
       expired from its first feed, a heldFor() fired after the input had
       let go. Ids stay in 1..INT32_MAX and skip one still in use. */
    uint32_t id;
    do {
        id = s_next_timer_id;
        s_next_timer_id = s_next_timer_id >= INT32_MAX ? 1 : s_next_timer_id + 1;
    } while (timer_find(id));

    esp_timer_create_args_t args = {
        .callback        = timer_fired_cb,
        .arg             = (void *)(uintptr_t)id,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "rule",
    };
    esp_timer_handle_t h;
    if (esp_timer_create(&args, &h) != ESP_OK)
        return JS_NewInt32(ctx, -1);
    if (esp_timer_start_once(h, (uint64_t)(ms * 1000.0)) != ESP_OK) {
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
static bool    s_interrupted;   /* the budget cut a call short since the last recovery */

static int js_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    if (!s_deadline_us) return 0;
    if (esp_timer_get_time() <= s_deadline_us) return 0;
    s_interrupted = true;
    return 1;
}

/* Called before every entry into JS. The deadline has to be renewed per event,
   not per boot: once it has passed, every further call would throw at once. */
static inline void budget_start(void)
{
    s_deadline_us = TIME_BUDGET_US ? esp_timer_get_time() + TIME_BUDGET_US : 0;
}

/* Runs the promise jobs the last entry into JS queued, under a budget of
   their own. QuickJS turns the budget's interrupt inside a job into an
   ordinary rejection (promise_reaction_job, js_async_function_resume), so
   the job "succeeds" and a .catch() can queue the next one: draining until
   the queue was empty never ended, and with the watchdog only reporting, the
   engine hung until a reboot. Stops once the budget is spent and leaves what
   is queued for the next pass, which brings a budget of its own; the
   interrupt stays flagged for recover_interrupt() at the top of the loop.
   Says whether it ran over. */
static bool drain_jobs(JSRuntime *rt)
{
    if (!JS_IsJobPending(rt)) return false;      /* the usual case: nothing queued */
    bool before = s_interrupted;
    s_interrupted = false;
    budget_start();
    JSContext *jctx;
    for (;;) {
        /* The interrupt is polled per so many operations; a chain of short
           jobs may never reach a poll inside one, so check between them. */
        if (s_interrupted || js_interrupt(rt, NULL)) break;
        int r = JS_ExecutePendingJob(rt, &jctx);
        if (r == 0) break;
        if (r < 0) {
            /* A job that failed outright leaves its exception pending on the
               runtime, where the next call would trip over it. */
            JSValue exc = JS_GetException(jctx);
            if (!s_interrupted) {
                const char *msg = JS_ToCString(jctx, exc);
                ESP_LOGE(TAG, "promise job: %s", msg ? msg : "(unknown)");
                JS_FreeCString(jctx, msg);
            }
            JS_FreeValue(jctx, exc);
        }
    }
    bool over = s_interrupted;
    if (over) ESP_LOGW(TAG, "promise jobs ran over the time budget: the rest runs on the next pass");
    s_interrupted = before || over;
    return over;
}

/* After the budget interrupted a call: the interrupt is uncatchable and skips
   every finally, so dsl.js is left with the flags its passes set around their
   work -- a noLoop rule that never fires again, an mqtt().once() that keeps
   matching -- and _after_interrupt() clears them. Called after every entry
   into JS, before the next one -- also between the calls of a function that
   makes several, like drain_overflow(). */
/* _after_interrupt() also takes up the rule passes the interrupt cut short,
   past the rule that ran over, so the later rules still get the event. A
   later rule that runs over as well is cut short in its turn, and the next
   call goes on past it -- up to RESUME_TRIES times per interrupted call, so a
   script full of slow rules holds the task for a bounded time (at most
   RESUME_TRIES + 2 budgets, the interrupted call included). The last call
   passes false: it drops what is left and only clears flags, which evaluates
   no rule. */
#define RESUME_TRIES 3

static void recover_interrupt(JSContext *ctx)
{
    if (!s_interrupted) return;
    ESP_LOGW(TAG, "time budget exceeded: rule state reset after the interrupted call");
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "_after_interrupt");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, fn)) s_interrupted = false;
    for (int i = 0; s_interrupted && i <= RESUME_TRIES; i++) {
        s_interrupted = false;
        /* Each try can take a whole budget; a tick between them lets this
           core's idle task run, which the task watchdog waits for. */
        if (i) vTaskDelay(1);
        budget_start();
        JSValue resume = JS_NewBool(ctx, i < RESUME_TRIES);
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &resume);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            /* Cut short by the budget: the next rule that ran over, which
               dsl.js names when it goes on past it -- not an error of its own. */
            if (!s_interrupted) {
                const char *msg = JS_ToCString(ctx, exc);
                ESP_LOGE(TAG, "_after_interrupt error: %s", msg ? msg : "(unknown)");
                JS_FreeCString(ctx, msg);
            }
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
    }
    /* Still interrupted after the last call: tried again on the next pass
       of the loop. */
    JS_FreeValue(ctx, fn);
}

/* A time sync (the clock just became real, or a later sync corrected it) or
   a time-zone change: dsl.js re-arms cron from the wall clock. Only a sync
   declares the clock valid; after a time-zone change the timers are re-armed
   only if it already is, since they were computed with the old offset. */
static void handle_time_sync(JSContext *ctx, JSValue on_time_sync, bool sync)
{
    if (sync) s_time_valid = true;
    if (!s_time_valid || !JS_IsFunction(ctx, on_time_sync)) return;
    budget_start();
    JSValue r = JS_Call(ctx, on_time_sync, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        ESP_LOGE(TAG, "_on_time_sync%s error: %s", sync ? "" : " (tz)", msg ? msg : "(unknown)");
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);
}

/* Time syncs and time-zone changes the queue could not take. They are state,
   not moments: dropped, the sync left cron off until the next one an hour
   later on a device that booted without a valid RTC. Delivering one late is
   the same as delivering it on time, and two pending are one. */
static volatile uint8_t s_pending;      /* PENDING_* bits */
#define PENDING_TIME_SYNC  0x01u
#define PENDING_TZ_CHANGE  0x02u
#define PENDING_MQTT_STATE 0x04u

/* The broker connection state is state as well, and goes only this way, not
   through the queue: dropped when the queue was full, mqttDown() never
   matched and the local fallback stayed off until the next transition; and
   a latch next to a queued older state could be overtaken by it. What is
   delivered is the latest state. */
static volatile bool s_mqtt_up;
static volatile bool s_mqtt_known;      /* a state was ever reported */

static void deliver_pending(JSContext *ctx, JSValue on_time_sync, JSValue on_mqtt)
{
    uint8_t p = __atomic_exchange_n(&s_pending, 0u, __ATOMIC_ACQUIRE);
    if (p & PENDING_TIME_SYNC)      handle_time_sync(ctx, on_time_sync, true);
    else if (p & PENDING_TZ_CHANGE) handle_time_sync(ctx, on_time_sync, false);  /* a sync re-arms too */
    if (p & PENDING_MQTT_STATE) {
        budget_start();
        call2(ctx, on_mqtt, JS_NewString(ctx, "$sys/mqtt"), JS_NewString(ctx, s_mqtt_up ? "up" : "down"));
    }
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

/* Input changes the queue dropped are made good from the live level: the
   DSL compares it with what it last saw and raises the event only if they
   differ. Both levels of a dropped LOW-HIGH pair are gone for good, but no
   condition is left believing a level the input no longer has. */
static void resync_lost_inputs(JSContext *ctx)
{
    uint32_t lost = __atomic_exchange_n(&s_input_lost, 0u, __ATOMIC_RELAXED);
    if (!lost || !g_scripting_io || !g_scripting_io->di_get) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "_resync_input");
    JS_FreeValue(ctx, global);
    if (JS_IsFunction(ctx, fn)) {
        for (int ch = 0; lost; ch++, lost >>= 1) {
            if (!(lost & 1)) continue;
            /* The mark first, then the read: an event numbered after the
               mark may carry a level the read did not see yet, and is kept. */
            input_mark_resync((uint8_t)ch);
            JSValue args[2] = { JS_NewInt32(ctx, ch),
                                JS_NewBool(ctx, g_scripting_io->di_get((uint8_t)ch)) };
            /* A budget of its own, like every event: the one left over from
               the previous event may be spent, and the interrupt handler
               would abort this call before it delivered anything. */
            budget_start();
            JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) {
                /* Not given up: the channel is offered again on the next pass.
                   _resync_input() does nothing once the engine holds the live
                   level, so this cannot repeat a rule's action. */
                JSValue exc = JS_GetException(ctx);
                const char *msg = JS_ToCString(ctx, exc);
                ESP_LOGE(TAG, "input %d resync: %s", ch, msg ? msg : "(unknown)");
                JS_FreeCString(ctx, msg);
                JS_FreeValue(ctx, exc);
                __atomic_fetch_or(&s_input_lost, 1u << ch, __ATOMIC_RELAXED);
            }
            JS_FreeValue(ctx, r);
            recover_interrupt(ctx);   /* before the next channel's call, as in drain_overflow() */
        }
    }
    JS_FreeValue(ctx, fn);
}

/* Runs the deferred timers whose turn it is: those posted after every event
   still in the queue. One whose predecessors are still queued waits -- and
   so do the ones behind it in the ring, which are later still. */
static void drain_overflow(JSContext *ctx)
{
    while (s_of_tail != s_of_head) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        scripting_evt_t head;
        if (xQueuePeek(s_queue, &head, 0) == pdTRUE &&
            !evt_seq_after(head.seq, s_overflow_seq[s_of_tail]))
            break;
        uint32_t id = s_overflow[s_of_tail];
        s_of_tail = (uint8_t)((s_of_tail + 1u) % TIMER_OVERFLOW_SLOTS);
        budget_start();
        run_timer(ctx, id);
        /* Before the next one, not after the loop: a callback the budget cut
           short leaves _firing set, and the next timer's pass skipped the
           noLoop rule it was meant to run. */
        recover_interrupt(ctx);
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

/* ── Trial load at the boot limit ───────────────────────────────────────── */

/* A reload evaluates the new script while the old context is still alive,
   so the runtime's limit is doubled for the handover. A script that only fits
   under the doubled limit was accepted, stored -- and failed with "out of
   memory" at the next boot, which runs it under the normal one: no rules at
   all. Measuring the new context inside the shared runtime does not tell
   reliably what it will need alone (atoms and hash tables are shared and
   never shrink). So the script is first loaded the way a boot loads it: a
   runtime of its own, the normal limit, the same allocator and budget. Writes
   to the hardware, MQTT and timers go nowhere -- the script's top level runs
   once more, and must not switch anything twice.

   Going nowhere is not forgetting, though. The trial keeps what the script
   would keep at boot, or it measures less and accepts a script the boot
   cannot load, or refuses one the boot would load:
     - a timer holds its callback until it is cleared, like the real table
       does: dropped at once, twelve watchdogs' closures were collected, the
       trial fitted and the boot ran out of memory; and the table holds
       MAX_TIMERS, as at boot, when it starts empty;
     - an output or a PWM pad reads back what the trial wrote to it, starting
       from the hardware's state: reading the hardware, `output(0).on()`
       followed by a check that it is on failed in the trial and nowhere
       else. */
#define DRY_OUTPUTS 32
#define DRY_PADS    4                   /* pads 1..3, as the bindings number them */
static bool s_dry_out[DRY_OUTPUTS];
static struct { bool pwm; uint32_t freq; uint16_t duty; } s_dry_pad[DRY_PADS];
static struct { bool used; int32_t id; JSValue fn; } s_dry_timers[MAX_TIMERS];

static esp_err_t dry_dout_set(uint8_t ch, bool on)
{
    if (ch < DRY_OUTPUTS) s_dry_out[ch] = on;
    return ESP_OK;
}
static esp_err_t dry_dout_toggle(uint8_t ch)
{
    if (ch < DRY_OUTPUTS) s_dry_out[ch] = !s_dry_out[ch];
    return ESP_OK;
}
static bool dry_dout_get(uint8_t ch) { return ch < DRY_OUTPUTS && s_dry_out[ch]; }
static esp_err_t dry_pwm_set(uint8_t p, uint32_t f, uint16_t d)
{
    if (p < 1 || p >= DRY_PADS) return ESP_ERR_INVALID_ARG;
    if (!s_dry_pad[p].pwm)      return ESP_ERR_INVALID_STATE;
    if (f)       s_dry_pad[p].freq = f;
    if (d != 0xFFFF) s_dry_pad[p].duty = d;
    return ESP_OK;
}
static bool dry_pwm_get(uint8_t p, uint32_t *f, uint16_t *d)
{
    if (p < 1 || p >= DRY_PADS || !s_dry_pad[p].pwm) return false;
    *f = s_dry_pad[p].freq;
    *d = s_dry_pad[p].duty;
    return true;
}
static esp_err_t dry_led_set(uint8_t r, uint8_t g, uint8_t b)      { (void)r; (void)g; (void)b; return ESP_OK; }
static void      dry_buzzer_set(uint32_t f)                        { (void)f; }
static int       dry_mqtt_publish(const char *t, const char *p, int l, int q, bool r)
                                                                   { (void)t; (void)p; (void)l; (void)q; (void)r; return 0; }
static int       dry_mqtt_subscribe(const char *t, int q)          { (void)t; (void)q; return 0; }

/* The trial's outputs and pads, as the hardware has them now. */
static void dry_io_start(const scripting_io_t *real)
{
    int n = real->do_count ? real->do_count : 8;
    for (int ch = 0; ch < DRY_OUTPUTS; ch++)
        s_dry_out[ch] = ch < n && real->dout_get && real->dout_get((uint8_t)ch);
    for (int p = 0; p < DRY_PADS; p++) {
        s_dry_pad[p].pwm = p >= 1 && real->aux_pwm_get &&
                           real->aux_pwm_get((uint8_t)p, &s_dry_pad[p].freq, &s_dry_pad[p].duty);
    }
}

/* _set_timer's checks and table, without the esp_timer. */
static JSValue js_dry_set_timer(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    static int32_t next;
    double ms;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &ms, argv[0]) < 0) return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "_set_timer: second argument must be a function");
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (s_dry_timers[i].used) continue;
        next = next >= INT32_MAX || next < 1 ? 1 : next + 1;   /* no signed overflow */
        s_dry_timers[i].used = true;
        s_dry_timers[i].id   = next;
        s_dry_timers[i].fn   = JS_DupValue(ctx, argv[1]);
        return JS_NewInt32(ctx, next);
    }
    return JS_NewInt32(ctx, -1);
}
static JSValue js_dry_clear_timer(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t id;
    (void)this_val; (void)argc;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!s_dry_timers[i].used || s_dry_timers[i].id != id) continue;
        JS_FreeValue(ctx, s_dry_timers[i].fn);
        s_dry_timers[i].used = false;
    }
    return JS_UNDEFINED;
}
/* Before the trial's context goes: the references are held against it. */
static void dry_timers_free(JSContext *ctx)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!s_dry_timers[i].used) continue;
        JS_FreeValue(ctx, s_dry_timers[i].fn);
        s_dry_timers[i].used = false;
    }
}
static JSValue js_dry_nop(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static bool trial_load(const char *script, char *err, size_t errlen)
{
    JSRuntime *trt = JS_NewRuntime2(&s_psram_alloc, NULL);
    if (!trt) { snprintf(err, errlen, "out of memory"); return false; }
    JS_SetMemoryLimit(trt, JS_HEAP_LIMIT);
    JS_SetMaxStackSize(trt, JS_STACK_LIMIT);
    JS_SetGCThreshold(trt, 256 * 1024);
    JS_SetInterruptHandler(trt, js_interrupt, NULL);

    const scripting_io_t *real = g_scripting_io;
    scripting_io_t dry = *real;
    dry.dout_set       = dry_dout_set;
    dry.dout_toggle    = dry_dout_toggle;
    dry.dout_get       = dry_dout_get;
    if (dry.aux_pwm_set) dry.aux_pwm_set = dry_pwm_set;
    if (dry.aux_pwm_get) dry.aux_pwm_get = dry_pwm_get;
    if (dry.led_set)     dry.led_set     = dry_led_set;
    if (dry.buzzer_set)  dry.buzzer_set  = dry_buzzer_set;
    if (dry.mqtt_publish)   dry.mqtt_publish   = dry_mqtt_publish;
    if (dry.mqtt_subscribe) dry.mqtt_subscribe = dry_mqtt_subscribe;
    dry.mqtt_unsubscribe = NULL;

    dry_io_start(real);

    bool ok = false;
    JSContext *tctx = JS_NewContext(trt);
    if (!tctx) {
        snprintf(err, errlen, "out of memory");
    } else {
        JS_SetContextOpaque(tctx, &dry);   /* this context's bindings only */
        scripting_register_bindings(tctx);
        register_timer_bindings(tctx);
        JSValue g = JS_GetGlobalObject(tctx);
        JS_SetPropertyStr(tctx, g, "_set_timer",   JS_NewCFunction(tctx, js_dry_set_timer, "_set_timer",   2));
        JS_SetPropertyStr(tctx, g, "_clear_timer", JS_NewCFunction(tctx, js_dry_clear_timer, "_clear_timer", 1));
        JS_SetPropertyStr(tctx, g, "print",        JS_NewCFunction(tctx, js_dry_nop,       "print",        1));
        JS_FreeValue(tctx, g);
        budget_start();
        ok = eval_check(tctx, dsl_js_start, "<dsl>", err, errlen);
        if (ok) {
            budget_start();
            ok = eval_check(tctx, script, "<user>", err, errlen);
        }
        /* A script whose promise jobs alone run over the budget would hold
           the engine on every boot; refuse it like one whose top level does. */
        if (ok && drain_jobs(trt)) {
            snprintf(err, errlen, "promise jobs ran over the time budget");
            ok = false;
        }
        dry_timers_free(tctx);
        JS_FreeContext(tctx);
    }
    JS_FreeRuntime(trt);
    s_interrupted = false;              /* the trial's, not the running rules' */
    if (!ok) {
        char why[64];
        strlcpy(why, err, sizeof(why));
        /* Cut short if it must be: the reason comes first. */
        if (snprintf(err, errlen, "%s (loaded as at boot, %d KiB heap)", why, JS_HEAP_LIMIT / 1024) < 0)
            strlcpy(err, why, errlen);
    }
    return ok;
}

/* ── Flash writes off the PSRAM stack ───────────────────────────────────── */

/* NVS writes turn the flash cache off, and with it the PSRAM this task's
   stack lives in: the engine hands the store to a small task with its stack
   in internal RAM and waits for the result. */
static TaskHandle_t         s_engine_task, s_store_task;
static scripting_store_fn_t s_store_fn;
static const char          *s_store_arg;
static esp_err_t            s_store_ret;

static void store_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_store_ret = s_store_fn(s_store_arg);
        xTaskNotifyGive(s_engine_task);
    }
}

static esp_err_t store_off_psram(scripting_store_fn_t fn, const char *arg)
{
    if (!s_store_task) return ESP_ERR_INVALID_STATE;
    s_store_fn = fn; s_store_arg = arg;
    xTaskNotifyGive(s_store_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return s_store_ret;
}

/* ── Scripting task ──────────────────────────────────────────────────────── */

/* ── Broker subscriptions of the rules ──────────────────────────────────────
   Only "rules/#" used to be subscribed, so mqtt('sys/heartbeat') -- the
   documented watchdog example -- never received a message and its watchdog
   fired with the other side alive. The topics the loaded rules read are
   subscribed besides it, and the list follows every reload: what a new rule
   set no longer reads is unsubscribed (app_mqtt keeps a filter another
   module holds). Not subscribed: "$..." (the engine's own '$sys/mqtt', and
   broker-reserved), wildcards (a rule's topic is a name, matched as it is),
   what "rules/#" covers, and what the firmware delivers itself
   (mqtt_local).

   Wanted and in place are kept apart: an entry is `ok` only once the client
   took the SUBSCRIBE. A refused one (client out of memory, not connected)
   used to count as done -- the topic stayed silent for the session, and
   loading the same rules again did not try again. Refused entries are
   tried again every SUB_RETRY_MS while connected; a lost connection makes
   every entry not ok, and the connect handler subscribes them all. Entry 0
   is "rules/#". What the broker answers in its SUBACK is not seen here. */
#define MAX_RULE_SUBS 33          /* "rules/#" and 32 topics of the rules */
#define SUB_RETRY_MS  10000
typedef struct { char topic[MAX_TOPIC_LEN]; bool ok; } rule_sub_t;
static rule_sub_t        s_subs[MAX_RULE_SUBS];
static int               s_nsubs;
static SemaphoreHandle_t s_subs_mux;
static volatile bool     s_subs_retry;      /* an entry is not ok */
static bool              s_topics_retry;   /* loaded rules' list was not built */

static bool sub_wanted(const char *t, size_t len)
{
    if (len == 0 || len >= MAX_TOPIC_LEN || memchr(t, '\0', len)) return false;
    if (t[0] == '$' || memchr(t, '+', len) || memchr(t, '#', len)) return false;
    if (len >= 6 && !memcmp(t, "rules/", 6)) return false;
    if (g_scripting_io->mqtt_local && g_scripting_io->mqtt_local(t)) return false;
    return true;
}

static int sub_find(const rule_sub_t *list, int n, const char *t)
{
    for (int i = 0; i < n; i++) if (!strcmp(list[i].topic, t)) return i;
    return -1;
}

/* Records the result of subscribing t, if t is still wanted. */
static void sub_mark(const char *t, bool ok)
{
    xSemaphoreTake(s_subs_mux, portMAX_DELAY);
    int i = sub_find(s_subs, s_nsubs, t);
    if (i >= 0) s_subs[i].ok = ok;
    if (!ok) s_subs_retry = true;
    xSemaphoreGive(s_subs_mux);
}

static bool sub_try(const char *t)
{
    bool ok = g_scripting_io->mqtt_subscribe(t, 0) >= 0;
    sub_mark(t, ok);
    return ok;
}

/* Subscribes every entry that is not ok (all = every entry). */
static void subs_apply(bool all)
{
    if (!g_scripting_io || !g_scripting_io->mqtt_subscribe || !s_subs_mux) return;
    rule_sub_t *t = heap_caps_malloc(sizeof(s_subs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t) { s_subs_retry = true; ESP_LOGE(TAG, "rule topics not subscribed: out of memory"); return; }
    xSemaphoreTake(s_subs_mux, portMAX_DELAY);
    int n = s_nsubs;
    memcpy(t, s_subs, sizeof(s_subs));
    s_subs_retry = false;
    xSemaphoreGive(s_subs_mux);
    int failed = 0;
    for (int i = 0; i < n; i++)
        if ((all || !t[i].ok) && !sub_try(t[i].topic)) failed++;
    if (failed) ESP_LOGW(TAG, "%d MQTT subscription(s) of the rules refused; tried again in %d s",
                         failed, SUB_RETRY_MS / 1000);
    heap_caps_free(t);
}

/* The broker connection went: nothing is subscribed until the next one. */
static void subs_lost(void)
{
    if (!s_subs_mux) return;
    xSemaphoreTake(s_subs_mux, portMAX_DELAY);
    for (int i = 0; i < s_nsubs; i++) s_subs[i].ok = false;
    xSemaphoreGive(s_subs_mux);
}

/* After a load, on the engine task. */
static void update_rule_subscriptions(JSContext *ctx)
{
    if (!g_scripting_io || !g_scripting_io->mqtt_subscribe || !s_subs_mux) return;
    s_topics_retry = true;
    rule_sub_t *want = heap_caps_calloc(2, sizeof(s_subs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!want) { ESP_LOGE(TAG, "rule topics not updated: out of memory"); return; }
    rule_sub_t *old = want + MAX_RULE_SUBS;
    int n = 0, skipped = 0;
    strcpy(want[n++].topic, RULES_TOPIC);

    JSValue g  = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, g, "_mqtt_topics");
    budget_start();
    JSValue arr = JS_IsFunction(ctx, fn) ? JS_Call(ctx, fn, g, 0, NULL) : JS_UNDEFINED;
    if (JS_IsException(arr) || JS_IsArray(ctx, arr) <= 0) goto list_failed;
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lv)) { JS_FreeValue(ctx, lv); goto list_failed; }
    int len_result = JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    if (len_result < 0) goto list_failed;
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(v)) { JS_FreeValue(ctx, v); goto list_failed; }
        size_t tl = 0;
        const char *t = JS_ToCStringLen(ctx, &tl, v);
        if (!t) {
            JS_FreeValue(ctx, v);
            goto list_failed;
        }
        if (sub_wanted(t, tl)) {
            if (n < MAX_RULE_SUBS) strcpy(want[n++].topic, t);
            else skipped++;
        }
        JS_FreeCString(ctx, t);
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, arr); JS_FreeValue(ctx, fn); JS_FreeValue(ctx, g);
    s_topics_retry = false;
    if (skipped)
        ESP_LOGW(TAG, "rules read %d MQTT topics more than the %d subscribed: those receive nothing",
                 skipped, MAX_RULE_SUBS - 1);

    /* What stays keeps its state: in place stays in place, refused is
       tried again below -- also when the same rules are loaded again. */
    xSemaphoreTake(s_subs_mux, portMAX_DELAY);
    int nold = s_nsubs;
    memcpy(old, s_subs, sizeof(s_subs));
    for (int i = 0; i < n; i++) {
        int k = sub_find(old, nold, want[i].topic);
        want[i].ok = k >= 0 && old[k].ok;
    }
    memcpy(s_subs, want, sizeof(s_subs));
    s_nsubs = n;
    xSemaphoreGive(s_subs_mux);

    for (int i = 0; i < nold; i++)
        if (sub_find(want, n, old[i].topic) < 0 && g_scripting_io->mqtt_unsubscribe)
            g_scripting_io->mqtt_unsubscribe(old[i].topic);
    /* Not connected (or the connect handler ran before s_mqtt_up was set):
       left to the retry, which runs once the connection is up. */
    for (int i = 0; i < n; i++) {
        if (want[i].ok) continue;
        if (!s_mqtt_up) { s_subs_retry = true; continue; }
        if (sub_find(old, nold, want[i].topic) < 0)
            ESP_LOGI(TAG, "rules read MQTT topic '%s': subscribing", want[i].topic);
        sub_try(want[i].topic);
    }
    heap_caps_free(want);
    return;

list_failed:
    /* Do not replace a working list with a partial one after an allocation
       or JS conversion failed. The engine retries the current context. */
    JS_FreeValue(ctx, JS_GetException(ctx));
    ESP_LOGE(TAG, "rule topics could not be listed; subscriptions unchanged, retry pending");
    JS_FreeValue(ctx, arr); JS_FreeValue(ctx, fn); JS_FreeValue(ctx, g);
    heap_caps_free(want);
}

/* On the engine task: rebuild a failed list before retrying subscriptions,
   at most every SUB_RETRY_MS and only while connected. */
static void subs_retry_due(JSContext *ctx)
{
    static TickType_t last;
    if ((!s_subs_retry && !s_topics_retry) || !s_mqtt_up) return;
    TickType_t now = xTaskGetTickCount();
    if (last && now - last < pdMS_TO_TICKS(SUB_RETRY_MS)) return;
    last = now;
    if (s_topics_retry) {
        update_rule_subscriptions(ctx);
        if (s_topics_retry) return;
    }
    subs_apply(false);
}

/* Allocation can fail even after the task's stack was allocated. Keep the
   queue available and retry without dereferencing a NULL runtime/context. */
static JSContext *engine_context_new(JSRuntime **runtime)
{
    JSRuntime *rt = JS_NewRuntime2(&s_psram_alloc, NULL);
    *runtime = NULL;
    if (!rt) return NULL;
    JS_SetMemoryLimit(rt, JS_HEAP_LIMIT);
    JS_SetMaxStackSize(rt, JS_STACK_LIMIT);
    JS_SetGCThreshold(rt, 256 * 1024);
    JS_SetInterruptHandler(rt, js_interrupt, NULL);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return NULL;
    }
    *runtime = rt;
    return ctx;
}

static void scripting_task(void *arg)
{
    JSRuntime *rt;
    JSContext *ctx;
    bool logged_oom = false;
    while (!(ctx = engine_context_new(&rt))) {
        if (!logged_oom) {
            ESP_LOGE(TAG, "Rule engine: out of memory; retrying startup");
            logged_oom = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    size_t heap_limit = JS_HEAP_LIMIT;   /* in force; a reload may raise it */

    scripting_register_bindings(ctx);
    register_timer_bindings(ctx);
    budget_start();
    eval_or_log(ctx, dsl_js_start, "<dsl>");
    budget_start();
    eval_or_log(ctx, s_user_script, "<user>");
    drain_jobs(rt);

    JSValue on_mqtt, on_input, on_time_sync, on_activity;
    bind_handlers(ctx, &on_mqtt, &on_input, &on_time_sync, &on_activity);
    update_rule_subscriptions(ctx);

    ESP_LOGI(TAG, "Rule engine ready. Free heap: %lu B  SPIRAM: %lu B",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    scripting_evt_t ev;
    for (;;) {
        recover_interrupt(ctx);   /* the event of the previous pass */
        deliver_pending(ctx, on_time_sync, on_mqtt);
        recover_interrupt(ctx);
        /* Resync before the deferred timers: a dropped input change has no
           place in the order, and the live level is never older than a
           timer that is due -- a stale one is what fired heldFor() wrongly. */
        resync_lost_inputs(ctx);
        recover_interrupt(ctx);
        drain_overflow(ctx);   /* timers the event queue could not take, in order */
        recover_interrupt(ctx);
        subs_retry_due(ctx);
        /* Promise jobs left by a drain that ran over, or queued by any of the
           calls above, which do not drain: they used to wait for the next
           queued event, on a quiet device indefinitely. Nothing to do and
           no cost when none is pending. */
        drain_jobs(rt);
        recover_interrupt(ctx);

        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
            budget_start();
            switch (ev.type) {
            case EVT_MQTT:
                /* With its length: JS_NewString() ended the payload at a
                   NUL byte, and a rule saw "ON" of "ON\0OFF". */
                call2(ctx, on_mqtt,
                      JS_NewString(ctx, ev.mqtt.topic),
                      JS_NewStringLen(ctx, ev.mqtt.payload, ev.mqtt.plen));
                break;
            case EVT_INPUT_CHANGE:
                /* Older than a resync of this channel: the resync already
                   delivered a later level, and this one would undo it. */
                if (input_event_stale(ev.input.channel, ev.seq)) break;
                call2(ctx, on_input,
                      JS_NewInt32(ctx, ev.input.channel),
                      JS_NewBool(ctx,  ev.input.state));
                break;
            case EVT_ANALOG: {
                /* Looked up per event rather than bound once: analog events
                   are a few per second at most. */
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue fn = JS_GetPropertyStr(ctx, global, "_on_analog");
                JS_FreeValue(ctx, global);
                if (JS_IsFunction(ctx, fn))
                    call2(ctx, fn, JS_NewInt32(ctx, ev.analog.pad),
                          ev.analog.mv == INT32_MIN ? JS_NULL : JS_NewInt32(ctx, ev.analog.mv));
                JS_FreeValue(ctx, fn);
                break;
            }
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

                if (!trial_load(ev.reload.script, err, sizeof(err))) {
                    ESP_LOGE(TAG, "rules not applied, they would not load at boot: %s", err);
                    free(ev.reload.script);
                    free(ev.reload.store);
                    publish_reload_result(ev.reload.ticket, false, err, false, ESP_OK);
                    break;
                }

                /* Both generations are alive while the new script is being
                   evaluated, and the memory limit applies to the runtime as a
                   whole. With the limit enforced, a script that legitimately
                   uses most of its budget left no room to build the
                   replacement -- and since the swap never happened the old
                   context was never freed either, so every further reload
                   failed the same way until a reboot.

                   Lift the ceiling for the duration of the handover. The
                   steady-state budget is unchanged: it is restored as soon as
                   one of the two contexts is gone. */
                JS_SetMemoryLimit(rt, heap_limit + JS_HEAP_LIMIT);

                JSContext *nctx = JS_NewContext(rt);
                if (!nctx) {
                    JS_SetMemoryLimit(rt, heap_limit);
                    ESP_LOGE(TAG, "reload: out of memory, rules unchanged");
                    free(ev.reload.script);
                    free(ev.reload.store);
                    publish_reload_result(ev.reload.ticket, false, "out of memory", false, ESP_OK);
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
                    /* New one gone again: back to what was in force -- a
                       limit an earlier reload raised included, or the
                       running rules would find every allocation refused. */
                    JS_SetMemoryLimit(rt, heap_limit);
                    ESP_LOGE(TAG, "rules not applied, previous rules still running");
                    free(ev.reload.store);
                    publish_reload_result(ev.reload.ticket, false, err, false, ESP_OK);
                    break;
                }

                drain_jobs(rt);
                /* The new rules have not seen the broker state yet:
                   mqttDown() would not match until the next transition. */
                if (s_mqtt_known)
                    __atomic_fetch_or(&s_pending, PENDING_MQTT_STATE, __ATOMIC_RELEASE);
                clear_timers(ctx, was_used);          /* old context's timers */
                JS_FreeValue(ctx, on_mqtt);
                JS_FreeValue(ctx, on_input);
                JS_FreeValue(ctx, on_time_sync);
                JS_FreeValue(ctx, on_activity);
                JS_FreeContext(ctx);
                /* The old one is gone. The trial load showed the script
                   fits the normal limit alone, but this runtime keeps the
                   atom and shape tables the handover grew -- QuickJS never
                   shrinks them. Should that leave it above the limit, the
                   limit is raised to what is in use plus room to work until
                   the next boot, which starts at the normal one again;
                   enforced as it was, every allocation would fail and the
                   rules would stop. */
                JS_RunGC(rt);
                JSMemoryUsage mu;
                JS_ComputeMemoryUsage(rt, &mu);
                size_t in_use = (size_t)mu.malloc_size, limit = JS_HEAP_LIMIT;
                if (in_use + HANDOVER_SLACK > limit) {
                    limit = in_use + HANDOVER_SLACK;
                    ESP_LOGW(TAG, "rule heap after the reload %u KiB: limit %u KiB until the next boot",
                             (unsigned)(in_use / 1024), (unsigned)(limit / 1024));
                }
                JS_SetMemoryLimit(rt, limit);
                heap_limit = limit;

                ctx = nctx;
                bind_handlers(ctx, &on_mqtt, &on_input, &on_time_sync, &on_activity);
                ESP_LOGI(TAG, "Rules reloaded");
                update_rule_subscriptions(ctx);
                /* Stored here, before the verdict is published, and not by
                   the caller after it: the caller may have stopped waiting. */
                bool      tried = ev.reload.store && ev.reload.store_fn;
                esp_err_t serr  = ESP_OK;
                if (tried) {
                    serr = store_off_psram(ev.reload.store_fn, ev.reload.store);
                    if (serr != ESP_OK)
                        ESP_LOGE(TAG, "rules running but not stored: %s", esp_err_to_name(serr));
                }
                free(ev.reload.store);
                publish_reload_result(ev.reload.ticket, true, "", tried, serr);
                break;
            }
            case EVT_TIMER:
                run_timer(ctx, ev.timer.id);
                break;
            case EVT_TIME_SYNC:
                handle_time_sync(ctx, on_time_sync, true);
                break;
            case EVT_TZ_CHANGE:
                handle_time_sync(ctx, on_time_sync, false);
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
    if (!user_script || !io) return ESP_ERR_INVALID_ARG;
    if (s_queue || s_store_task || s_engine_task) return ESP_ERR_INVALID_STATE;
    bool made_subs_mux = !s_subs_mux;
    s_user_script  = user_script;
    g_scripting_io = io;
    /* Without it the connect handler subscribes nothing, not even "rules/#". */
    if (!s_subs_mux) s_subs_mux = xSemaphoreCreateMutex();
    if (!s_subs_mux) {
        ESP_LOGE(TAG, "subscription lock: out of memory");
        return ESP_ERR_NO_MEM;
    }

    s_reload_lock = xSemaphoreCreateMutex();
    if (!s_reload_lock) {
        ESP_LOGE(TAG, "Failed to create reload status lock");
        goto no_mem;
    }

    s_send_lock = xSemaphoreCreateMutex();
    if (!s_send_lock) {
        ESP_LOGE(TAG, "Failed to create event lock");
        goto no_mem;
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(scripting_evt_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        goto no_mem;
    }

    if (xTaskCreate(store_task, "rules_store", 4096, NULL, TASK_PRIORITY, &s_store_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create rules store task");
        goto no_mem;
    }
    BaseType_t ok = xTaskCreateWithCaps(scripting_task, "scripting",
                                        TASK_STACK_BYTES / sizeof(StackType_t),
                                        NULL, TASK_PRIORITY, &s_engine_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scripting task");
        goto no_mem;
    }
    return ESP_OK;

no_mem:
    /* No engine task is running on this path. Stop its store helper before
       freeing resources, and clear handles so producers cannot use them. */
    if (s_store_task) { vTaskDelete(s_store_task); s_store_task = NULL; }
    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
    if (s_send_lock) { vSemaphoreDelete(s_send_lock); s_send_lock = NULL; }
    if (s_reload_lock) { vSemaphoreDelete(s_reload_lock); s_reload_lock = NULL; }
    if (made_subs_mux && s_subs_mux) { vSemaphoreDelete(s_subs_mux); s_subs_mux = NULL; }
    return ESP_ERR_NO_MEM;
}

void scripting_on_mqtt_connected(void)
{
    /* Before anything is loaded the list holds only "rules/#". */
    if (s_subs_mux && s_nsubs == 0) {
        xSemaphoreTake(s_subs_mux, portMAX_DELAY);
        if (s_nsubs == 0) { strcpy(s_subs[0].topic, RULES_TOPIC); s_subs[0].ok = false; s_nsubs = 1; }
        xSemaphoreGive(s_subs_mux);
    }
    subs_apply(true);
}

void scripting_on_mqtt_state(bool up)
{
    if (!up) subs_lost();
    s_mqtt_up = up;
    s_mqtt_known = true;
    __atomic_fetch_or(&s_pending, PENDING_MQTT_STATE, __ATOMIC_RELEASE);
}

void scripting_on_mqtt_message(const char *topic, size_t tlen,
                                const char *data,  size_t dlen)
{
    if (!s_queue) return;

    scripting_evt_t ev = { .type = EVT_MQTT };
    /* Too long is dropped, not cut: a cut topic could equal a shorter one
       and fire that topic's rules, and a cut payload is a different value
       -- half a JSON object, a number missing its last digits. */
    if (tlen >= sizeof(ev.mqtt.topic) || dlen >= sizeof(ev.mqtt.payload)) {
        ESP_LOGW(TAG, "MQTT message dropped: topic %zu/%zu, payload %zu/%zu bytes over the limit",
                 tlen, sizeof(ev.mqtt.topic) - 1, dlen, sizeof(ev.mqtt.payload) - 1);
        return;
    }

    /* A topic is a name: with a NUL byte in it, it would match the shorter
       one in front of it. MQTT does not allow one; such a message is not
       for any rule. */
    if (memchr(topic, '\0', tlen)) {
        ESP_LOGW(TAG, "MQTT message dropped: NUL byte in the topic");
        return;
    }
    memcpy(ev.mqtt.topic,   topic, tlen); ev.mqtt.topic[tlen]   = '\0';
    memcpy(ev.mqtt.payload, data,  dlen); ev.mqtt.payload[dlen] = '\0';
    ev.mqtt.plen = (uint16_t)dlen;

    if (evt_send(&ev) != pdTRUE)
        ESP_LOGW(TAG, "Event queue full — MQTT message dropped");
}

uint32_t scripting_reload(const char *new_script)
{
    return scripting_reload_and_store(new_script, NULL, NULL);
}

uint32_t scripting_reload_and_store(const char *new_script, const char *to_store,
                                    scripting_store_fn_t store)
{
    if (!s_queue || !new_script) return 0;
    char *copy = strdup(new_script);
    char *keep = (to_store && store) ? strdup(to_store) : NULL;
    if (!copy || ((to_store && store) && !keep)) {
        ESP_LOGE(TAG, "scripting_reload: out of memory");
        free(copy); free(keep);
        return 0;
    }
    scripting_evt_t ev = { .type = EVT_RELOAD };
    ev.reload.script   = copy;
    ev.reload.store    = keep;
    ev.reload.store_fn = store;
    if (s_reload_lock) xSemaphoreTake(s_reload_lock, portMAX_DELAY);
    ev.reload.ticket = ++s_reload_seq;
    if (ev.reload.ticket == 0) ev.reload.ticket = ++s_reload_seq;   /* 0 means "not queued" */
    if (s_reload_lock) xSemaphoreGive(s_reload_lock);
    if (evt_send(&ev) != pdTRUE) {
        ESP_LOGW(TAG, "scripting_reload: queue full");
        free(copy);
        free(keep);
        return 0;
    }
    return ev.reload.ticket;
}

void scripting_set_time_valid(void)
{
    // Boot path: the RTC seeded real time before the scripting task started, so cron
    // may arm at load. Just flip the flag the _time_valid() binding reports.
    s_time_valid = true;
}

void scripting_on_tz_change(void)
{
    if (!s_queue) return;
    scripting_evt_t ev = { .type = EVT_TZ_CHANGE };
    if (evt_send(&ev) != pdTRUE) {
        __atomic_fetch_or(&s_pending, PENDING_TZ_CHANGE, __ATOMIC_RELAXED);
        ESP_LOGW(TAG, "scripting_on_tz_change: queue full, delivered on the next pass");
    }
}

void scripting_on_time_sync(void)
{
    // Runtime path: an SNTP sync stepped the clock. Tell the scripting task to re-arm
    // cron from the corrected wall-clock (it also sets s_time_valid). If the engine
    // isn't up yet, the boot-time _time_valid() read covers arming.
    if (!s_queue) { s_time_valid = true; return; }
    scripting_evt_t ev = { .type = EVT_TIME_SYNC };
    if (evt_send(&ev) != pdTRUE) {
        __atomic_fetch_or(&s_pending, PENDING_TIME_SYNC, __ATOMIC_RELAXED);
        ESP_LOGW(TAG, "scripting_on_time_sync: queue full, delivered on the next pass");
    }
}

static void post_activity(uint8_t source)
{
    if (!s_queue) return;
    scripting_evt_t ev = { .type = EVT_ACTIVITY, .activity = { .source = source } };
    // Drop silently if the queue is full — a missed health feed only risks a spurious
    // "stale" edge, and flooding the log from a busy RX task would be worse.
    evt_send(&ev);
}

void scripting_on_modbus_activity(void) { post_activity(ACT_MODBUS); }
void scripting_on_can_activity(void)    { post_activity(ACT_CAN); }

bool scripting_on_analog(uint8_t pad, int mv)
{
    if (!s_queue) return false;
    scripting_evt_t ev = { .type = EVT_ANALOG, .analog = { .pad = pad, .mv = mv } };
    /* Not lost when the queue is full: the driver offers the reading again
       with its next sample. */
    return evt_send(&ev) == pdTRUE;
}

void scripting_on_input_change(uint8_t channel, bool state)
{
    if (!s_queue) return;
    scripting_evt_t ev = {
        .type  = EVT_INPUT_CHANGE,
        .input = { .channel = channel, .state = state },
    };
    if (evt_send(&ev) != pdTRUE) {
        /* Remembered, not lost: the engine re-reads the channel's live level
           on its next pass and delivers that as the event, so a condition's
           shadow of the input cannot stay behind for good. */
        __atomic_fetch_or(&s_input_lost, 1u << (channel & 31), __ATOMIC_RELAXED);
        ESP_LOGW(TAG, "Event queue full — input change dropped, resync pending");
    }
}
