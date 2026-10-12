/* Forced into every host test (-include in the Makefile): what the libc of
   the development machine has and the CI's Linux does not.

   strlcpy()/strlcat() are in macOS's libc and in ESP-IDF's newlib, which the
   firmware is written against, but glibc has them only from 2.38 on and not
   under -std=c11 at all. On Linux the tests get these instead, under their
   own name, so a glibc that declares them does not clash.

   No libc header here: this comes before a test's own feature macros
   (_DEFAULT_SOURCE, _POSIX_C_SOURCE), which must precede the first one.

   glibc under -std=c11 also hides POSIX itself -- strdup(), gmtime_r(),
   usleep(), getaddrinfo() -- which macOS declares regardless and newlib
   has. _DEFAULT_SOURCE opens it, spelled as gen_tz_test.py spells it so
   that its own definition is no redefinition. */
#ifndef HOST_COMPAT_H
#define HOST_COMPAT_H
#if defined(__linux__)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
static inline __SIZE_TYPE__ host_strlcpy(char *d, const char *s, __SIZE_TYPE__ n)
{
    __SIZE_TYPE__ l = __builtin_strlen(s);
    if (n) { __SIZE_TYPE__ c = l < n - 1 ? l : n - 1; __builtin_memcpy(d, s, c); d[c] = 0; }
    return l;
}
static inline __SIZE_TYPE__ host_strlcat(char *d, const char *s, __SIZE_TYPE__ n)
{
    __SIZE_TYPE__ dl = 0;
    while (dl < n && d[dl]) dl++;
    if (dl == n) return n + __builtin_strlen(s);
    return dl + host_strlcpy(d + dl, s, n - dl);
}
#define strlcpy host_strlcpy
#define strlcat host_strlcat
#endif
#endif
