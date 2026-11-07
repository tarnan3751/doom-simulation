/*
 * inttypes.h - printf format macros for <stdint.h> types.
 *
 * Written for GCC's riscv32 (ilp32) ABI, where int32_t is `long` and
 * int64_t / intmax_t are `long long`; the fallbacks keep the header correct
 * for the LP64 hosts the unit tests run on.
 */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <sys/cdefs.h>
#include <stdint.h>

#if defined(__riscv) && (__riscv_xlen == 32) && !defined(__clang__)
# define __PRI32  "l"       /* int32_t  = long      */
# define __PRIPTR ""        /* intptr_t = int       */
#else
# define __PRI32  ""
# if __SIZEOF_POINTER__ == 8
#  define __PRIPTR "l"
# else
#  define __PRIPTR ""
# endif
#endif
#if __SIZEOF_LONG__ == 8 && !defined(__APPLE__)
# define __PRI64  "l"
#else
# define __PRI64  "ll"
#endif
#if __SIZEOF_LONG__ == 8
# define __PRIMAX "l"
#else
# define __PRIMAX "ll"
#endif

#define PRId8   "d"
#define PRIi8   "i"
#define PRIo8   "o"
#define PRIu8   "u"
#define PRIx8   "x"
#define PRIX8   "X"
#define PRId16  "d"
#define PRIi16  "i"
#define PRIo16  "o"
#define PRIu16  "u"
#define PRIx16  "x"
#define PRIX16  "X"
#define PRId32  __PRI32 "d"
#define PRIi32  __PRI32 "i"
#define PRIo32  __PRI32 "o"
#define PRIu32  __PRI32 "u"
#define PRIx32  __PRI32 "x"
#define PRIX32  __PRI32 "X"
#define PRId64  __PRI64 "d"
#define PRIi64  __PRI64 "i"
#define PRIo64  __PRI64 "o"
#define PRIu64  __PRI64 "u"
#define PRIx64  __PRI64 "x"
#define PRIX64  __PRI64 "X"
#define PRIdMAX __PRIMAX "d"
#define PRIiMAX __PRIMAX "i"
#define PRIoMAX __PRIMAX "o"
#define PRIuMAX __PRIMAX "u"
#define PRIxMAX __PRIMAX "x"
#define PRIXMAX __PRIMAX "X"
#define PRIdPTR __PRIPTR "d"
#define PRIiPTR __PRIPTR "i"
#define PRIoPTR __PRIPTR "o"
#define PRIuPTR __PRIPTR "u"
#define PRIxPTR __PRIPTR "x"
#define PRIXPTR __PRIPTR "X"

#endif /* _INTTYPES_H */
