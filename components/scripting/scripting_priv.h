#pragma once
#include "quickjs.h"
#include "scripting.h"

extern const scripting_io_t *g_scripting_io;

void scripting_register_bindings(JSContext *ctx);
