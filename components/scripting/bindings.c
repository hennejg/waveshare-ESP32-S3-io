#include "scripting_priv.h"
#include "quickjs.h"
#include <esp_log.h>
#include <stdint.h>

#define TAG "js"

static JSValue js_di_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t ch;
    if (JS_ToInt32(ctx, &ch, argv[0]) < 0) return JS_EXCEPTION;
    if (ch < 0 || ch > 7)
        return JS_ThrowRangeError(ctx, "input channel must be 0-7");
    return JS_NewBool(ctx, g_scripting_io->di_get((uint8_t)ch));
}

static JSValue js_dout_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t ch;
    if (JS_ToInt32(ctx, &ch, argv[0]) < 0) return JS_EXCEPTION;
    if (ch < 0 || ch > 7)
        return JS_ThrowRangeError(ctx, "output channel must be 0-7");
    bool state = JS_ToBool(ctx, argv[1]);
    esp_err_t err = g_scripting_io->dout_set((uint8_t)ch, state);
    if (err != ESP_OK)
        return JS_ThrowInternalError(ctx, "dout_set failed: %d", (int)err);
    return JS_UNDEFINED;
}

static JSValue js_dout_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t ch;
    if (JS_ToInt32(ctx, &ch, argv[0]) < 0) return JS_EXCEPTION;
    if (ch < 0 || ch > 7)
        return JS_ThrowRangeError(ctx, "output channel must be 0-7");
    return JS_NewBool(ctx, g_scripting_io->dout_get((uint8_t)ch));
}

static JSValue js_print(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_EXCEPTION;   /* toString() threw -- do not swallow it */
    ESP_LOGI(TAG, "%s", str);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* Clamp a JS number argument to a 0-255 colour byte. Returns false and leaves
   the pending exception in place if the conversion threw -- an argument whose
   valueOf() throws used to be silently taken as 0, and the exception then sat
   in the runtime until some later operation tripped over it. */
static bool to_u8(JSContext *ctx, JSValue v, uint8_t *out)
{
    int32_t n = 0;
    if (JS_ToInt32(ctx, &n, v) < 0) return false;
    if (n < 0)   n = 0;
    if (n > 255) n = 255;
    *out = (uint8_t)n;
    return true;
}

static JSValue js_led_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    if (!g_scripting_io->led_set) return JS_UNDEFINED;
    uint8_t r, g, b;
    if (!to_u8(ctx, argv[0], &r) || !to_u8(ctx, argv[1], &g) || !to_u8(ctx, argv[2], &b))
        return JS_EXCEPTION;
    g_scripting_io->led_set(r, g, b);
    return JS_UNDEFINED;
}

static JSValue js_buzzer_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    if (!g_scripting_io->buzzer_set) return JS_UNDEFINED;
    int32_t freq = 0;
    if (JS_ToInt32(ctx, &freq, argv[0]) < 0) return JS_EXCEPTION;
    if (freq < 0) freq = 0;
    g_scripting_io->buzzer_set((uint32_t)freq);
    return JS_UNDEFINED;
}

static JSValue js_mqtt_publish(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    if (!g_scripting_io->mqtt_publish) return JS_NewInt32(ctx, -1);

    const char *topic = JS_ToCString(ctx, argv[0]);
    if (!topic) return JS_EXCEPTION;

    /* ToCStringLen, not strlen: a payload containing a NUL would otherwise be
       cut off there. */
    size_t payload_len = 0;
    const char *payload = JS_ToCStringLen(ctx, &payload_len, argv[1]);
    if (!payload) { JS_FreeCString(ctx, topic); return JS_EXCEPTION; }

    int32_t qos = 0;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToInt32(ctx, &qos, argv[2]) < 0) {
        JS_FreeCString(ctx, topic);
        JS_FreeCString(ctx, payload);
        return JS_EXCEPTION;
    }
    if (qos < 0) qos = 0;
    if (qos > 2) qos = 2;

    bool retain = false;
    if (argc > 3 && !JS_IsUndefined(argv[3]))
        retain = (bool)JS_ToBool(ctx, argv[3]);

    int msg_id = g_scripting_io->mqtt_publish(topic, payload, (int)payload_len,
                                              (int)qos, retain);
    JS_FreeCString(ctx, topic);
    JS_FreeCString(ctx, payload);
    return JS_NewInt32(ctx, msg_id);
}

void scripting_register_bindings(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "_di_get",       JS_NewCFunction(ctx, js_di_get,       "_di_get",       1));
    JS_SetPropertyStr(ctx, g, "_dout_set",     JS_NewCFunction(ctx, js_dout_set,     "_dout_set",     2));
    JS_SetPropertyStr(ctx, g, "_dout_get",     JS_NewCFunction(ctx, js_dout_get,     "_dout_get",     1));
    JS_SetPropertyStr(ctx, g, "_led_set",      JS_NewCFunction(ctx, js_led_set,      "_led_set",      3));
    JS_SetPropertyStr(ctx, g, "_buzzer_set",   JS_NewCFunction(ctx, js_buzzer_set,   "_buzzer_set",   1));
    JS_SetPropertyStr(ctx, g, "_mqtt_publish", JS_NewCFunction(ctx, js_mqtt_publish, "_mqtt_publish", 4));
    JS_SetPropertyStr(ctx, g, "print",         JS_NewCFunction(ctx, js_print,        "print",         1));
    JS_FreeValue(ctx, g);
}
