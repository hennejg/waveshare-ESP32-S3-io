#include "json_exact.h"
#include <string.h>

cJSON *json_parse_exact(const char *data, size_t n)
{
    if (!data || n == 0 || memchr(data, '\0', n)) return NULL;
    const char *end = NULL;
    cJSON *j = cJSON_ParseWithLengthOpts(data, n, &end, 0);
    if (!j) return NULL;
    for (const char *p = end; p < data + n; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') { cJSON_Delete(j); return NULL; }
    }
    return j;
}
