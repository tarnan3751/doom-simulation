/*
 * assert.h - assert() prints "Assertion failed: ..." on stderr and calls
 * abort() (exit status 134).  Re-includable: honours NDEBUG at each include.
 */
#include <sys/cdefs.h>

#undef assert
#ifdef NDEBUG
# define assert(e) ((void)0)
#else
__BEGIN_DECLS
__noreturn_ void __assert_fail(const char *expr, const char *file, int line, const char *func);
__END_DECLS
# define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#if !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(static_assert)
# define static_assert _Static_assert
#endif
