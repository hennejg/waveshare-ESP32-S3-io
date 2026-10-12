#pragma once
#include <stddef.h>
#include "cJSON.h"

/* cJSON of exactly the n bytes at data: one JSON value, then only white
   space. NULL for anything else -- also for a NUL byte anywhere in it.
   cJSON_ParseWithLength() and cJSON_Parse() stop after the first value and
   ignore the rest, so "50\0xyz", "50 off" or an object with bytes appended
   were taken as the value at the front: a message acted on although its
   whole payload says something else. */
cJSON *json_parse_exact(const char *data, size_t n);
