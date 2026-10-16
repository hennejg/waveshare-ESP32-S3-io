#pragma once
#include "quickjs.h"
#include "scripting.h"

extern const scripting_io_t *g_scripting_io;

/* The I/O a context's bindings drive: its own when it has one (the trial
   load's dry set), else the hardware's. Per context, never by swapping
   g_scripting_io: other tasks read that -- the MQTT connect callback
   subscribed rules/# through it, and a reconnect during a trial subscribed
   through the dry set, i.e. not at all. */
static inline const scripting_io_t *scripting_io_of(JSContext *ctx)
{
    const scripting_io_t *io = (const scripting_io_t *)JS_GetContextOpaque(ctx);
    return io ? io : g_scripting_io;
}

void scripting_register_bindings(JSContext *ctx);
