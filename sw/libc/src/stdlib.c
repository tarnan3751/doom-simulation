/*
 * stdlib.c - integer conversions and process control.
 */
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <libc_port.h>
#include "libc_impl.h"

/* ---- strto* ------------------------------------------------------------ */

static int digit_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

/* Parses the magnitude with C strto* rules.  Returns the value modulo 2^64
 * with *ovf set if it did not fit in 64 bits; *neg reports a '-' sign.
 * *end receives the first unparsed character (nptr if nothing parsed). */
static unsigned long long scan_magnitude(const char *nptr, char **end, int base,
                                         int *neg, int *ovf)
{
    const char *s = nptr;
    unsigned long long acc = 0;
    int any = 0;

    *neg = 0;
    *ovf = 0;
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (end)
            *end = (char *)nptr;
        return 0;
    }
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '+' || *s == '-')
        *neg = (*s++ == '-');
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')
        && digit_value((unsigned char)s[2]) < 16) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    unsigned long long cutoff = ULLONG_MAX / (unsigned)base;
    unsigned cutlim = (unsigned)(ULLONG_MAX % (unsigned)base);
    for (;; s++) {
        int d = digit_value((unsigned char)*s);
        if (d >= base)
            break;
        any = 1;
        if (acc > cutoff || (acc == cutoff && (unsigned)d > cutlim))
            *ovf = 1;
        acc = acc * (unsigned)base + (unsigned)d;
    }
    if (end)
        *end = (char *)(any ? s : nptr);
    if (!any)
        *neg = 0;
    return acc;
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base)
{
    int neg, ovf;
    unsigned long long v = scan_magnitude(s, end, base, &neg, &ovf);
    if (ovf || v > ULONG_MAX) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return neg ? -(unsigned long)v : (unsigned long)v;
}

long strtol(const char *restrict s, char **restrict end, int base)
{
    int neg, ovf;
    unsigned long long v = scan_magnitude(s, end, base, &neg, &ovf);
    if (neg) {
        if (ovf || v > (unsigned long long)LONG_MAX + 1) {
            errno = ERANGE;
            return LONG_MIN;
        }
        return (long)(0 - (unsigned long)v);
    }
    if (ovf || v > (unsigned long long)LONG_MAX) {
        errno = ERANGE;
        return LONG_MAX;
    }
    return (long)v;
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

/* ---- process control ----------------------------------------------------- */

#define ATEXIT_MAX 32
static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_count;

int atexit(void (*func)(void))
{
    if (atexit_count >= ATEXIT_MAX)
        return -1;
    atexit_fns[atexit_count++] = func;
    return 0;
}

void __libc_run_atexit(void)
{
    while (atexit_count > 0)
        atexit_fns[--atexit_count]();
}

void exit(int status)
{
    __libc_run_atexit();
    fflush(NULL);
    __libc_exit(status);
}

void abort(void)
{
    static const char msg[] = "abort() called\n";
    __libc_console_write(msg, sizeof msg - 1);
    __libc_exit(134);      /* 128 + SIGABRT, as a host shell would report */
}
