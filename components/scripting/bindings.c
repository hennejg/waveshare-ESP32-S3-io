#include "scripting_priv.h"
#include "quickjs.h"
#include <esp_log.h>
#include <stdint.h>

#define TAG "js"

static int do_count(JSContext *ctx)
{
    const scripting_io_t *io = scripting_io_of(ctx);
    return (io && io->do_count) ? io->do_count : 8;
}

static int di_count(JSContext *ctx)
{
    const scripting_io_t *io = scripting_io_of(ctx);
    return (io && io->di_count) ? io->di_count : 8;
}

/* A channel number, checked as the double it arrives as: JS_ToInt32 wraps
   modulo 2^32, so output(4294967296) drove DO1. Like pad_arg() below. */
static int chan_arg(JSContext *ctx, JSValue v, int count, const char *what, int32_t *ch)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0) return -1;
    if (!(d >= 0 && d < count) || d != (double)(int32_t)d) {
        JS_ThrowRangeError(ctx, "%s channel must be 0-%d", what, count - 1);
        return -1;
    }
    *ch = (int32_t)d;
    return 0;
}

static JSValue js_di_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t ch;
    if (chan_arg(ctx, argv[0], di_count(ctx), "input", &ch) < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, scripting_io_of(ctx)->di_get((uint8_t)ch));
}

static JSValue js_dout_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t ch;
    if (chan_arg(ctx, argv[0], do_count(ctx), "output", &ch) < 0) return JS_EXCEPTION;
    bool state = JS_ToBool(ctx, argv[1]);
    esp_err_t err = scripting_io_of(ctx)->dout_set((uint8_t)ch, state);
    if (err == ESP_ERR_INVALID_STATE)
        return JS_ThrowInternalError(ctx, "output %d is a header pad that is not set to output", (int)ch);
    if (err != ESP_OK)
        return JS_ThrowInternalError(ctx, "dout_set failed: %d", (int)err);
    return JS_UNDEFINED;
}

static JSValue js_dout_toggle(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t ch;
    if (chan_arg(ctx, argv[0], do_count(ctx), "output", &ch) < 0) return JS_EXCEPTION;
    esp_err_t err = scripting_io_of(ctx)->dout_toggle((uint8_t)ch);
    if (err == ESP_ERR_INVALID_STATE)
        return JS_ThrowInternalError(ctx, "output %d is a header pad that is not set to output", (int)ch);
    if (err != ESP_OK)
        return JS_ThrowInternalError(ctx, "dout_toggle failed: %d", (int)err);
    return JS_UNDEFINED;
}

static JSValue js_dout_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t ch;
    if (chan_arg(ctx, argv[0], do_count(ctx), "output", &ch) < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, scripting_io_of(ctx)->dout_get((uint8_t)ch));
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
    if (!scripting_io_of(ctx)->led_set) return JS_UNDEFINED;
    uint8_t r, g, b;
    if (!to_u8(ctx, argv[0], &r) || !to_u8(ctx, argv[1], &g) || !to_u8(ctx, argv[2], &b))
        return JS_EXCEPTION;
    scripting_io_of(ctx)->led_set(r, g, b);
    return JS_UNDEFINED;
}

static JSValue js_buzzer_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    if (!scripting_io_of(ctx)->buzzer_set) return JS_UNDEFINED;
    int32_t freq = 0;
    if (JS_ToInt32(ctx, &freq, argv[0]) < 0) return JS_EXCEPTION;
    if (freq < 0) freq = 0;
    scripting_io_of(ctx)->buzzer_set((uint32_t)freq);
    return JS_UNDEFINED;
}

static JSValue js_mqtt_publish(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    if (!scripting_io_of(ctx)->mqtt_publish) return JS_NewInt32(ctx, -1);

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

    int msg_id = scripting_io_of(ctx)->mqtt_publish(topic, payload, (int)payload_len,
                                              (int)qos, retain);
    JS_FreeCString(ctx, topic);
    JS_FreeCString(ctx, payload);
    return JS_NewInt32(ctx, msg_id);
}

/* A pad number 1..3, checked as the number it is: JS_ToInt32 wraps modulo
   2^32, so pwm(4294967297) would otherwise reach pad 1. */
static int pad_arg(JSContext *ctx, JSValue v, int32_t *pad)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0) return -1;
    if (!(d >= 1 && d <= 3) || d != (double)(int32_t)d) {
        JS_ThrowRangeError(ctx, "pad must be 1, 2 or 3");
        return -1;
    }
    *pad = (int32_t)d;
    return 0;
}

/* _aux_pwm_set(pad, duty%, freq) -- pad 1..3; freq undefined or 0 keeps it.
   The ranges are aux_io.h's AUX_PWM_*; this component cannot include it. */
static JSValue js_aux_pwm_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    if (!scripting_io_of(ctx)->aux_pwm_set) return JS_ThrowInternalError(ctx, "no PWM pads on this board");
    int32_t pad;
    double duty, freq = 0;
    if (pad_arg(ctx, argv[0], &pad) < 0) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &duty, argv[1]) < 0) return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &freq, argv[2]) < 0) return JS_EXCEPTION;
    if (!(duty >= 0.0 && duty <= 100.0)) return JS_ThrowRangeError(ctx, "pwm duty must be 0-100 (%%)");
    if (freq != 0 && (!(freq >= 5 && freq <= 1000000) || freq != (double)(uint32_t)freq))
        return JS_ThrowRangeError(ctx, "pwm freq must be a whole number of Hz from 5 to 1000000");
    esp_err_t err = scripting_io_of(ctx)->aux_pwm_set((uint8_t)pad, (uint32_t)freq, (uint16_t)(duty * 10.0 + 0.5));
    if (err == ESP_ERR_INVALID_STATE)
        return JS_ThrowInternalError(ctx, "pad %d is not set to PWM output", (int)pad);
    if (err != ESP_OK) return JS_ThrowInternalError(ctx, "pwm set failed: %d", (int)err);
    return JS_UNDEFINED;
}

/* _aux_pwm_get(pad) -> {duty, freq} or null when the pad is not a PWM output. */
static JSValue js_aux_pwm_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t pad;
    if (pad_arg(ctx, argv[0], &pad) < 0) return JS_EXCEPTION;
    uint32_t f; uint16_t d;
    if (!scripting_io_of(ctx)->aux_pwm_get || !scripting_io_of(ctx)->aux_pwm_get((uint8_t)pad, &f, &d)) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "duty", JS_NewFloat64(ctx, d / 10.0));
    JS_SetPropertyStr(ctx, o, "freq", JS_NewInt64(ctx, f));
    return o;
}

/* _aux_analog_get(pad) -> millivolts, or null when the pad is not analog. */
static JSValue js_aux_analog_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    int32_t pad;
    if (pad_arg(ctx, argv[0], &pad) < 0) return JS_EXCEPTION;
    int mv;
    if (!scripting_io_of(ctx)->aux_analog_get || scripting_io_of(ctx)->aux_analog_get((uint8_t)pad, &mv) != ESP_OK)
        return JS_NULL;
    return JS_NewInt32(ctx, mv);
}

void scripting_register_bindings(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "_di_get",       JS_NewCFunction(ctx, js_di_get,       "_di_get",       1));
    JS_SetPropertyStr(ctx, g, "_dout_set",     JS_NewCFunction(ctx, js_dout_set,     "_dout_set",     2));
    JS_SetPropertyStr(ctx, g, "_dout_get",     JS_NewCFunction(ctx, js_dout_get,     "_dout_get",     1));
    JS_SetPropertyStr(ctx, g, "_dout_toggle",  JS_NewCFunction(ctx, js_dout_toggle,  "_dout_toggle",  1));
    JS_SetPropertyStr(ctx, g, "_aux_pwm_set",  JS_NewCFunction(ctx, js_aux_pwm_set,  "_aux_pwm_set",  3));
    JS_SetPropertyStr(ctx, g, "_aux_pwm_get",  JS_NewCFunction(ctx, js_aux_pwm_get,  "_aux_pwm_get",  1));
    JS_SetPropertyStr(ctx, g, "_aux_analog_get", JS_NewCFunction(ctx, js_aux_analog_get, "_aux_analog_get", 1));
    JS_SetPropertyStr(ctx, g, "_led_set",      JS_NewCFunction(ctx, js_led_set,      "_led_set",      3));
    JS_SetPropertyStr(ctx, g, "_buzzer_set",   JS_NewCFunction(ctx, js_buzzer_set,   "_buzzer_set",   1));
    JS_SetPropertyStr(ctx, g, "_mqtt_publish", JS_NewCFunction(ctx, js_mqtt_publish, "_mqtt_publish", 4));
    JS_SetPropertyStr(ctx, g, "print",         JS_NewCFunction(ctx, js_print,        "print",         1));
    JS_FreeValue(ctx, g);
}
