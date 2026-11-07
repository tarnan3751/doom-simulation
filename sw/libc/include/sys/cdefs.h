/*
 * sys/cdefs.h - small helper macros shared by the libc headers.
 */
#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H

#ifdef __cplusplus
# define __BEGIN_DECLS extern "C" {
# define __END_DECLS   }
# define __restrict_   /* nothing */
#else
# define __BEGIN_DECLS
# define __END_DECLS
# define __restrict_   restrict
#endif

#define __noreturn_         __attribute__((__noreturn__))
#define __printflike_(f, a) __attribute__((__format__(__printf__, f, a)))
#define __scanflike_(f, a)  __attribute__((__format__(__scanf__, f, a)))
#define __malloclike_       __attribute__((__malloc__))
#define __pure_             __attribute__((__pure__))

#endif /* _SYS_CDEFS_H */
