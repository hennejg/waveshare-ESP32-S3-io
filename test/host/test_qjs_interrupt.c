/* QuickJS with the firmware's patches and the real dsl.js: what the time
 * budget's interrupt leaves behind. The interrupt is uncatchable -- it skips
 * every catch and finally -- so flags a pass sets around its work stay set:
 *   - a noLoop rule kept _firing and never ran again,
 *   - an mqtt().once() kept _armed and fired on a later input without a
 *     new message,
 *   - an every() whose tick was cut short never re-armed.
 * The host calls _after_interrupt() after an interrupted call, as
 * scripting.c's recover_interrupt() does; RECOVER=0 shows the failure.
 *
 *   make -C test/host qjsint
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"

static int64_t fake_now;
#define MAX_TIMERS 64
static struct { int used; int64_t due; JSValue fn; } timers[MAX_TIMERS];
static int failures, recover = 1;
static struct timespec deadline;
static int budget_on, interrupted;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FEHLER: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int js_interrupt(JSRuntime *rt, void *op)
{
    (void)rt; (void)op;
    if (!budget_on) return 0;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec < deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec <= deadline.tv_nsec)) return 0;
    interrupted = 1;
    return 1;
}
static void budget_start(void)
{
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 30 * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    budget_on = 1;
}

static JSValue js_print(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{ (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
static JSValue js_now(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{ (void)t; (void)c; (void)v; return JS_NewFloat64(ctx, (double)fake_now); }
static JSValue js_false(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{ (void)ctx; (void)t; (void)c; (void)v; return JS_FALSE; }
static JSValue js_set_timer(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{
    (void)t; (void)c;
    double ms; JS_ToFloat64(ctx, &ms, v[0]);
    for (int i = 0; i < MAX_TIMERS; i++) if (!timers[i].used) {
        timers[i].used = 1; timers[i].due = fake_now + (int64_t)(ms < 0 ? 0 : ms);
        timers[i].fn = JS_DupValue(ctx, v[1]);
        return JS_NewInt32(ctx, i + 1);
    }
    return JS_NewInt32(ctx, -1);
}
static JSValue js_clear_timer(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{
    (void)t; (void)c;
    int id; JS_ToInt32(ctx, &id, v[0]);
    if (id >= 1 && id <= MAX_TIMERS && timers[id - 1].used) {
        JS_FreeValue(ctx, timers[id - 1].fn); timers[id - 1].used = 0;
    }
    return JS_UNDEFINED;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2); b[n] = 0; fclose(f);
    return b;
}

/* One entry into JS the way the firmware's loop makes it: a fresh budget,
   then the recovery if the budget ran out. */
static void after_call(JSContext *ctx)
{
    budget_on = 0;
    if (!interrupted) return;
    interrupted = 0;
    if (!recover) return;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, g, "_after_interrupt");
    JS_FreeValue(ctx, g);
    budget_start();
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    budget_on = 0;
    if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); CHECK(0, "_after_interrupt warf"); }
    JS_FreeValue(ctx, r); JS_FreeValue(ctx, fn);
}

static void run(JSContext *ctx, const char *src)
{
    budget_start();
    JSValue r = JS_Eval(ctx, src, strlen(src), "<host>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    after_call(ctx);
}

static double num(JSContext *ctx, const char *expr)
{
    JSValue r = JS_Eval(ctx, expr, strlen(expr), "<expr>", JS_EVAL_TYPE_GLOBAL);
    double d = -1; JS_ToFloat64(ctx, &d, r); JS_FreeValue(ctx, r); return d;
}

static void run_until(JSContext *ctx, int64_t end)
{
    for (int guard = 0; guard < 10000; guard++) {
        int best = -1;
        for (int i = 0; i < MAX_TIMERS; i++)
            if (timers[i].used && (best < 0 || timers[i].due < timers[best].due)) best = i;
        if (best < 0 || timers[best].due > end) break;
        if (timers[best].due > fake_now) fake_now = timers[best].due;
        JSValue fn = timers[best].fn; timers[best].used = 0;
        budget_start();
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, fn);
        after_call(ctx);
    }
    fake_now = end;
}

int main(int argc, char **argv)
{
    if (getenv("RECOVER")) recover = atoi(getenv("RECOVER"));
    const char *dsl = slurp(argc > 1 ? argv[1] : "../../components/scripting/dsl.js");
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetInterruptHandler(rt, js_interrupt, NULL);
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "print",        JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, g, "_now",         JS_NewCFunction(ctx, js_now, "_now", 0));
    JS_SetPropertyStr(ctx, g, "_di_get",      JS_NewCFunction(ctx, js_false, "_di_get", 1));
    JS_SetPropertyStr(ctx, g, "_set_timer",   JS_NewCFunction(ctx, js_set_timer, "_set_timer", 2));
    JS_SetPropertyStr(ctx, g, "_clear_timer", JS_NewCFunction(ctx, js_clear_timer, "_clear_timer", 1));
    JS_FreeValue(ctx, g);
    fake_now = 1000000;
    JSValue r = JS_Eval(ctx, dsl, strlen(dsl), "dsl.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { printf("dsl.js laedt nicht\n"); return 2; }
    JS_FreeValue(ctx, r);
    run(ctx,
        "var T = { n: 0, m: 0, e: 0, spin: 1 };"
        "function spin() { if (T.spin) for (;;) {} }"
        "rule('n').when(input(0).isOn()).noLoop().then(function(){ T.n++; spin(); });"
        "rule('m').when(mqtt('x').once(), input(1).isOn()).then(function(){ T.m++; spin(); });"
        "rule('e').when(every(1000)).then(function(){ T.e++; spin(); });");

    /* Each rule is cut short once by the budget, then runs normally. */
    run(ctx, "_on_input(0, true)");
    run(ctx, "_on_input(1, true)");
    run(ctx, "_on_mqtt('x', '1')");
    run_until(ctx, fake_now + 1000);
    run(ctx, "T.spin = 0");

    run(ctx, "_on_input(0, false)");
    run(ctx, "_on_input(0, true)");
    int n = (int)num(ctx, "T.n");
    printf("  noLoop-Regel nach dem Abbruch: %d Ausfuehrungen (erwartet 2)\n", n);
    CHECK(n == 2, "noLoop-Regel feuert nach dem Abbruch nicht mehr");

    run(ctx, "_on_input(1, false)");
    run(ctx, "_on_input(1, true)");
    int m = (int)num(ctx, "T.m");
    printf("  mqtt().once() ohne neue Nachricht: %d Ausfuehrungen (erwartet 1)\n", m);
    CHECK(m == 1, "mqtt().once() feuert ohne neue Nachricht erneut");

    run_until(ctx, fake_now + 5000);
    int e = (int)num(ctx, "T.e");
    printf("  every(1000) in den 5 s danach: %d Ausfuehrungen insgesamt (erwartet 6)\n", e);
    CHECK(e == 6, "every() hat sich nach dem Abbruch nicht neu gestellt");

    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].used) { JS_FreeValue(ctx, timers[i].fn); timers[i].used = 0; }
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    printf(failures ? "qjsint: %d FEHLER\n" : "qjsint: alles OK\n", failures);
    return failures ? 1 : 0;
}
