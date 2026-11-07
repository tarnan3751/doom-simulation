/*
 * libc_impl.h - internal declarations shared by the library's sources.
 */
#ifndef _LIBC_IMPL_H
#define _LIBC_IMPL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

/* A machine word for the word-at-a-time string routines.  Always 32 bits so
 * that the host unit tests exercise exactly the code that runs on RV32.
 * may_alias: these loads/stores legitimately alias any object type. */
typedef uint32_t __attribute__((__may_alias__)) libc_word;
#define LIBC_WORD   ((size_t)sizeof(libc_word))
#define LIBC_ONES   0x01010101u
#define LIBC_HIGHS  0x80808080u
/* nonzero iff some byte of w is zero */
#define LIBC_HASZERO(w) (((w) - LIBC_ONES) & ~(w) & LIBC_HIGHS)

/* ---- printf core (printf.c) ------------------------------------------- */
/* Output sink: receives the formatted text in chunks. */
typedef void (*libc_sink_fn)(void *ctx, const char *s, size_t n);
/* Formats `fmt` into `sink`; returns the number of characters produced
 * (which can exceed what a bounded sink stored), or -1 on overflow of int. */
int __libc_vformat(libc_sink_fn sink, void *ctx, const char *fmt, va_list ap);

/* ---- exact decimal conversion (fpconv.c, for printf.c) ----------------- */
/* Exact decimal expansion of a finite positive double: writes the
 * significant digits (no leading or trailing zeros) as ASCII into `digits`
 * (capacity LIBC_DTOA_MAX) and returns their count n; *dexp receives E with
 * value = 0.d1d2...dn x 10^E.  Returns 0 for zero. */
#define LIBC_DTOA_MAX 800
int __libc_dtoa_exact(double v, char *digits, int *dexp);

/* ---- stdio internals (stdio.c) ----------------------------------------- */
struct __libc_file {
    unsigned             flags;
    const unsigned char *data;      /* memfile contents                   */
    size_t               size;
    size_t               pos;
};
#define LIBC_F_INUSE   0x01u
#define LIBC_F_READ    0x02u
#define LIBC_F_WRITE   0x04u
#define LIBC_F_CONSOLE 0x08u        /* writes go to __libc_console_write  */
#define LIBC_F_MEM     0x10u        /* reads come from data/size/pos      */

/* Basename comparison used by fopen/remove/rename/memfile_lookup. */
const char *__libc_basename(const char *path);

/* ---- process (stdlib.c) ------------------------------------------------ */
void __libc_run_atexit(void);

#endif /* _LIBC_IMPL_H */
