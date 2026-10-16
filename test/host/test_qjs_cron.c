/* QuickJS -- the firmware's engine, with its patches -- and the real dsl.js,
 * on the host: local-time Date conversion around the DST switches, and a
 * minute cron across them. TZ is the firmware default for Germany.
 *
 * The cron case is what an outside review found: at 2026-03-29 00:30 UTC
 * the engine put "the next minute" an hour in the past, so the timer fired
 * at once, re-armed into the past again, and the action ran over and over.
 *
 *   make -C test/host qjscron
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "quickjs.h"

static int64_t fake_now;                       /* ms since epoch */
#define MAX_TIMERS 64
static struct { int used; int64_t due; JSValue fn; } timers[MAX_TIMERS];
static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FEHLER: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)ctx; (void)argc; (void)argv;
    return JS_UNDEFINED;
}
static JSValue js_now(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{ (void)t; (void)c; (void)v; return JS_NewFloat64(ctx, (double)fake_now); }
static JSValue js_true(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{ (void)ctx; (void)t; (void)c; (void)v; return JS_TRUE; }
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

static int eval(JSContext *ctx, const char *src, const char *name)
{
    JSValue r = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx); const char *m = JS_ToCString(ctx, e);
        printf("  JS-Fehler in %s: %s\n", name, m); JS_FreeCString(ctx, m); JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, r); return -1;
    }
    JS_FreeValue(ctx, r); return 0;
}

static double eval_num(JSContext *ctx, const char *expr)
{
    JSValue r = JS_Eval(ctx, expr, strlen(expr), "<expr>", JS_EVAL_TYPE_GLOBAL);
    double d = NAN; JS_ToFloat64(ctx, &d, r); JS_FreeValue(ctx, r); return d;
}

/* Runs the engine's timers in due order until fake_now passes end. */
static void run_until(JSContext *ctx, int64_t end)
{
    for (int guard = 0; guard < 100000; guard++) {
        int best = -1;
        for (int i = 0; i < MAX_TIMERS; i++)
            if (timers[i].used && (best < 0 || timers[i].due < timers[best].due)) best = i;
        if (best < 0 || timers[best].due > end) break;
        if (timers[best].due > fake_now) fake_now = timers[best].due;
        JSValue fn = timers[best].fn; timers[best].used = 0;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, fn);
    }
    fake_now = end;
}

static void cron_case(const char *dsl, const char *label, const char *expr, int64_t start, int minutes, int want)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    memset(timers, 0, sizeof timers);
    fake_now = start;
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "print",        JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, g, "_now",         JS_NewCFunction(ctx, js_now, "_now", 0));
    JS_SetPropertyStr(ctx, g, "_time_valid",  JS_NewCFunction(ctx, js_true, "_time_valid", 0));
    JS_SetPropertyStr(ctx, g, "_set_timer",   JS_NewCFunction(ctx, js_set_timer, "_set_timer", 2));
    JS_SetPropertyStr(ctx, g, "_clear_timer", JS_NewCFunction(ctx, js_clear_timer, "_clear_timer", 1));
    JS_FreeValue(ctx, g);
    if (eval(ctx, dsl, "dsl.js") || eval(ctx,
            "var fires = [];", "rules.js")) {
        failures++; return;
    }
    char rules[160];
    snprintf(rules, sizeof rules, "rule('m').when(cron('%s')).then(function(){ fires.push(_now()); });", expr);
    if (eval(ctx, rules, "rules.js")) {
        failures++; return;
    }
    run_until(ctx, start + (int64_t)minutes * 60000);
    int n = (int)eval_num(ctx, "fires.length");
    int same = (int)eval_num(ctx, "(function(){var k=0;for(var i=1;i<fires.length;i++) if(fires[i]-fires[i-1]<60000) k++; return k;})()");
    (void)0;
    int off  = (int)eval_num(ctx, "(function(){var k=0;for(var i=0;i<fires.length;i++) if(fires[i]%60000!==0) k++; return k;})()");
    printf("  %s, cron '%s': %d Ausfuehrungen in %d Minuten, %d davon sofort wiederholt\n", label, expr, n, minutes, same);
    CHECK(n == want, "%s: %d statt %d Ausfuehrungen", label, n, want);
    CHECK(same == 0, "%s: %d Ausfuehrungen weniger als eine Minute nach der vorigen", label, same);
    CHECK(off == 0, "%s: %d Ausfuehrungen nicht auf einer vollen Minute", label, off);
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].used) { JS_FreeValue(ctx, timers[i].fn); timers[i].used = 0; }
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
}

int main(int argc, char **argv)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    const char *dsl = slurp(argc > 1 ? argv[1] : "../../components/scripting/dsl.js");
    printf("QuickJS: lokale Zeit an der Sommerzeitumstellung, Minuten-Cron\n");

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    static const struct { const char *expr; double want; const char *what; } dates[] = {
        { "new Date(2026,2,29,1,31).getTime()", 1774744260000.0, "29.03. 01:31 MEZ, vor der Umstellung" },
        { "new Date(2026,2,29,3,30).getTime()", 1774747800000.0, "29.03. 03:30 MESZ, nach der Umstellung" },
        { "new Date(2026,6,9,12,0).getTime()",  1783591200000.0, "09.07. 12:00 MESZ" },
        { "new Date(2026,0,10,12,0).getTime()", 1768042800000.0, "10.01. 12:00 MEZ" },
        { "new Date(2026,9,25,3,30).getTime()", 1792895400000.0, "25.10. 03:30 MEZ, nach der Rueckstellung" },
    };
    for (size_t i = 0; i < sizeof dates / sizeof dates[0]; i++) {
        double got = eval_num(ctx, dates[i].expr);
        CHECK(got == dates[i].want, "%s: %.0f statt %.0f (%+.0f min)", dates[i].what, got, dates[i].want,
              (got - dates[i].want) / 60000.0);
    }
    printf("  lokale Datumsfelder -> UTC an 5 Stichtagen geprueft\n");
    JS_FreeContext(ctx); JS_FreeRuntime(rt);

    cron_case(dsl, "Umstellung 29.03.2026 ab 00:30 UTC",    "* * * * *",  1774744200000LL, 120, 120);
    cron_case(dsl, "Rueckstellung 25.10.2026 ab 00:30 UTC", "* * * * *",  1792888200000LL, 120, 120);
    /* A fixed hour runs once in the hour repeated in autumn, not twice. */
    cron_case(dsl, "Rueckstellung 25.10.2026 ab 00:00 UTC", "30 2 * * *", 1792886400000LL, 240, 1);
    /* An hourly job runs every real hour, the repeated one included. */
    cron_case(dsl, "Rueckstellung 25.10.2026 ab 00:00 UTC", "0 * * * *",  1792886400000LL, 240, 4);

    printf("%s\n", failures ? "FEHLER" : "alles in Ordnung");
    return failures ? 1 : 0;
}
