#pragma once
// Portability shims for QuickJS on ESP-IDF (newlib/Xtensa).
// Force-included into every QuickJS source file via CMakeLists.txt.

#include <stddef.h>

// newlib's struct tm uses __tm_gmtoff when __TM_GMTOFF is defined;
// quickjs.c always references tm_gmtoff — bridge the rename.
#if defined(__TM_GMTOFF) && !defined(tm_gmtoff)
#  define tm_gmtoff __tm_gmtoff
#endif

// newlib does not provide malloc_usable_size; the default QuickJS allocator
// uses it, but since we supply a custom allocator it is never called at
// runtime.  Provide a stub so the (dead) code path still compiles.
#ifndef malloc_usable_size
static inline size_t malloc_usable_size(const void *p)
    { (void)p; return 0; }
#endif
