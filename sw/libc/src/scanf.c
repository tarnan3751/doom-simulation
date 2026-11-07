/*
 * scanf.c - formatted input from strings (sscanf).
 *
 * Directives: white space, literals, %%, and the conversions
 * d i u o x X p (integers), s, c, [set], n, with '*' suppression, a field
 * width and the length modifiers hh h l ll q j z t.  No floating point: no
 * program here scans it.  Like glibc, "0x" with no hex digit after it scans
 * as 0.  An input failure returns EOF if nothing was assigned yet, as glibc
 * and the BSDs do.
 */
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T };

#define WIDTH_END (-2)          /* "field width exhausted" (nothing consumed) */

typedef struct {
    const unsigned char *s;     /* the input              */
    size_t               count; /* characters consumed (for %n) */
    int                  left;  /* remaining field width  */
} src_t;

static int sget(src_t *in)
{
    int c = *in->s;
    if (!c)
        return EOF;
    in->s++;
    in->count++;
    return c;
}

static void sunget(src_t *in, int c)
{
    if (c < 0)
        return;
    in->s--;
    in->count--;
}

/* Next character within the current field width. */
static inline int wget(src_t *in)
{
    if (in->left <= 0)
        return WIDTH_END;
    in->left--;
    return sget(in);
}

static int digit_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* 1 = converted, 0 = matching failure */
static int scan_int(src_t *in, int base, unsigned long long *out)
{
    int neg = 0, ndig = 0;
    int c = wget(in);
    if (c == '+' || c == '-') {
        neg = (c == '-');
        c = wget(in);
    }
    if ((base == 0 || base == 16) && c == '0') {
        ndig = 1;
        c = wget(in);
        if (c == 'x' || c == 'X') {
            base = 16;
            c = wget(in);
        } else if (base == 0) {
            base = 8;
        }
    }
    if (base == 0)
        base = 10;
    unsigned long long acc = 0;
    while (c >= 0) {
        int d = digit_value(c);
        if (d >= base)
            break;
        acc = acc * (unsigned)base + (unsigned)d;
        ndig++;
        c = wget(in);
    }
    sunget(in, c);
    if (!ndig)
        return 0;
    *out = neg ? 0ull - acc : acc;
    return 1;
}

static void store_int(va_list *ap, int len, unsigned long long v)
{
    switch (len) {
    case LEN_HH: *va_arg(*ap, signed char *) = (signed char)v; break;
    case LEN_H:  *va_arg(*ap, short *) = (short)v; break;
    case LEN_L:  *va_arg(*ap, long *) = (long)v; break;
    case LEN_LL: *va_arg(*ap, long long *) = (long long)v; break;
    case LEN_J:  *va_arg(*ap, intmax_t *) = (intmax_t)v; break;
    case LEN_Z:  *va_arg(*ap, size_t *) = (size_t)v; break;
    case LEN_T:  *va_arg(*ap, ptrdiff_t *) = (ptrdiff_t)v; break;
    default:     *va_arg(*ap, int *) = (int)v; break;
    }
}

int vsscanf(const char *restrict s, const char *restrict fmt, va_list ap_in)
{
    src_t in = { (const unsigned char *)s, 0, 0 };
    va_list ap;
    va_copy(ap, ap_in);
    int assigned = 0, converted = 0, c;
    const unsigned char *p = (const unsigned char *)fmt;

    while (*p) {
        if (isspace(*p)) {
            while (isspace(*p))
                p++;
            while ((c = sget(&in)) != EOF && isspace(c))
                ;
            sunget(&in, c);
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            int want = *p;
            if (*p == '%') {
                p += 2;
                while ((c = sget(&in)) != EOF && isspace(c))
                    ;
            } else {
                p++;
                c = sget(&in);
            }
            if (c == EOF)
                goto input_failure;
            if (c != want) {
                sunget(&in, c);
                goto done;
            }
            continue;
        }
        p++;                                        /* past '%' */
        int suppress = 0, width = 0, len = LEN_NONE;
        if (*p == '*') {
            suppress = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            if (width < INT_MAX / 10)
                width = width * 10 + (*p - '0');
            p++;
        }
        switch (*p) {
        case 'h': p++; if (*p == 'h') { p++; len = LEN_HH; } else len = LEN_H; break;
        case 'l': p++; if (*p == 'l') { p++; len = LEN_LL; } else len = LEN_L; break;
        case 'q': p++; len = LEN_LL; break;
        case 'j': p++; len = LEN_J; break;
        case 'z': p++; len = LEN_Z; break;
        case 't': p++; len = LEN_T; break;
        default: break;
        }
        int conv = *p;
        if (!conv)
            goto done;                              /* malformed format */
        p++;

        if (conv == 'n') {
            if (!suppress)
                store_int(&ap, len, in.count);
            continue;
        }
        if (conv != 'c' && conv != '[') {
            while ((c = sget(&in)) != EOF && isspace(c))
                ;
            if (c == EOF)
                goto input_failure;
            sunget(&in, c);
        }
        in.left = width > 0 ? width : INT_MAX;

        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            unsigned long long v;
            if (!scan_int(&in, base, &v))
                goto done;
            if (!suppress) {
                if (conv == 'p')
                    *va_arg(ap, void **) = (void *)(uintptr_t)v;
                else
                    store_int(&ap, len, v);
            }
            break;
        }
        case 's': {
            char *dst = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while ((c = wget(&in)) >= 0 && !isspace(c)) {
                if (dst)
                    dst[n] = (char)c;
                n++;
            }
            sunget(&in, c);
            if (dst)
                dst[n] = '\0';
            break;
        }
        case 'c': {
            char *dst = suppress ? NULL : va_arg(ap, char *);
            int n = 0, want = width > 0 ? width : 1;
            while (n < want && (c = sget(&in)) != EOF) {
                if (dst)
                    dst[n] = (char)c;
                n++;
            }
            if (n < want)
                goto input_failure;
            break;
        }
        case '[': {
            uint32_t set[8] = { 0 };
            int negate = 0;
            if (*p == '^') {
                negate = 1;
                p++;
            }
            if (*p == ']') {
                set[']' >> 5] |= 1u << (']' & 31);
                p++;
            }
            while (*p && *p != ']') {
                unsigned lo = *p++;
                unsigned hi = lo;
                if (*p == '-' && p[1] && p[1] != ']') {
                    hi = p[1];
                    p += 2;
                }
                for (unsigned ch = lo; ch <= hi; ch++)
                    set[ch >> 5] |= 1u << (ch & 31);
            }
            if (*p == ']')
                p++;
            char *dst = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while ((c = wget(&in)) >= 0) {
                int in_set = (set[c >> 5] >> (c & 31)) & 1;
                if (in_set == negate)
                    break;
                if (dst)
                    dst[n] = (char)c;
                n++;
            }
            sunget(&in, c);
            if (n == 0) {
                if (c == EOF)
                    goto input_failure;
                goto done;
            }
            if (dst)
                dst[n] = '\0';
            break;
        }
        default:
            goto done;                              /* unknown conversion */
        }
        converted++;
        if (!suppress)
            assigned++;
    }
done:
    va_end(ap);
    return assigned;
input_failure:
    /* EOF when nothing was assigned yet - what glibc and the BSDs do (ISO C's
     * wording, "before the first conversion has completed", would also count
     * suppressed conversions) */
    va_end(ap);
    (void)converted;
    return assigned ? assigned : EOF;
}

int sscanf(const char *restrict s, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
