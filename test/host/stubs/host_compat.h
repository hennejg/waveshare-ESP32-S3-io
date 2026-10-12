/* Forced into every host test (-include in the Makefile): what the libc of
   the development machine has and the CI's Linux does not by default.

   glibc under -std=c11 hides POSIX and BSD -- strdup(), gmtime_r(),
   usleep(), getaddrinfo(), strlcpy() -- which macOS declares regardless and
   ESP-IDF's newlib, the firmware's libc, has. _DEFAULT_SOURCE opens it,
   spelled as gen_tz_test.py spells it so that its own definition is no
   redefinition. Defined here, it precedes every libc header, as a feature
   macro must.

   strlcpy()/strlcat() came to glibc only with 2.38; an older one gets
   them here. */
#ifndef HOST_COMPAT_H
#define HOST_COMPAT_H
#if defined(__linux__)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <features.h>
#if defined(__GLIBC__) && !__GLIBC_PREREQ(2, 38)
#include <stddef.h>
#include <string.h>
static inline size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
static inline size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
#endif
#endif
#endif
