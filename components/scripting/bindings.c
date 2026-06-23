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
    if (str) {
        ESP_LOGI(TAG, "%s", str);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

void scripting_register_bindings(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "_di_get",   JS_NewCFunction(ctx, js_di_get,   "_di_get",   1));
    JS_SetPropertyStr(ctx, g, "_dout_set", JS_NewCFunction(ctx, js_dout_set, "_dout_set", 2));
    JS_SetPropertyStr(ctx, g, "_dout_get", JS_NewCFunction(ctx, js_dout_get, "_dout_get", 1));
    JS_SetPropertyStr(ctx, g, "print",     JS_NewCFunction(ctx, js_print,     "print",     1));
    JS_FreeValue(ctx, g);
}
