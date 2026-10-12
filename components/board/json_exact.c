#include "json_exact.h"
#include <stdbool.h>
#include <string.h>

/* Whether a string in the JSON text escapes U+0000. cJSON decodes "\u0000"
   into a NUL byte inside valuestring, and every reader after it measures
   the string with strlen(): ["on\u0000off"] became "on" and switched the
   output. Scanned on the raw text, where the length is still known: inside
   a string a backslash takes the next character, and only \u with 0000
   after it is the NUL ("\\u0000" is a backslash and the text u0000). */
static bool escapes_nul(const char *d, size_t n)
{
    bool in_str = false;
    for (size_t i = 0; i < n; i++) {
        char c = d[i];
        if (!in_str) { if (c == '"') in_str = true; continue; }
        if (c == '"') { in_str = false; continue; }
        if (c != '\\' || i + 1 >= n) continue;
        if (d[i + 1] == 'u' && i + 5 < n && !memcmp(d + i + 2, "0000", 4)) return true;
        i++;   /* the escaped character */
    }
    return false;
}

cJSON *json_parse_exact(const char *data, size_t n)
{
    if (!data || n == 0 || memchr(data, '\0', n) || escapes_nul(data, n)) return NULL;
    const char *end = NULL;
    cJSON *j = cJSON_ParseWithLengthOpts(data, n, &end, 0);
    if (!j) return NULL;
    for (const char *p = end; p < data + n; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') { cJSON_Delete(j); return NULL; }
    }
    return j;
}
