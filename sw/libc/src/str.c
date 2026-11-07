/*
 * str.c - NUL-terminated string functions ("C" locale).
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "libc_impl.h"

size_t strlen(const char *s)
{
    const char *p = s;
    while ((uintptr_t)p & (LIBC_WORD - 1)) {
        if (!*p)
            return (size_t)(p - s);
        p++;
    }
    /* Aligned word reads never cross a page or the end of RAM, so reading
     * up to three bytes past the terminator is harmless. */
    const libc_word *w = (const libc_word *)(const void *)p;
    while (!LIBC_HASZERO(*w))
        w++;
    p = (const char *)w;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen)
{
    const char *p = memchr(s, 0, maxlen);
    return p ? (size_t)(p - s) : maxlen;
}

char *strncpy(char *restrict dst, const char *restrict src, size_t n)
{
    size_t len = strnlen(src, n);
    memcpy(dst, src, len);
    memset(dst + len, 0, n - len);
    return dst;
}

int strcmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    while (*p && *p == *q) {
        p++; q++;
    }
    return *p - *q;
}

int strncmp(const char *a, const char *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    for (; n; n--, p++, q++) {
        if (*p != *q)
            return *p - *q;
        if (!*p)
            break;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    int ca, cb;
    do {
        ca = tolower(*p++);
        cb = tolower(*q++);
    } while (ca == cb && ca);
    return ca - cb;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    for (; n; n--) {
        int ca = tolower(*p++), cb = tolower(*q++);
        if (ca != cb)
            return ca - cb;
        if (!ca)
            break;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    char ch = (char)c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == ch)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return (char *)hay;
    for (; (hay = strchr(hay, needle[0])) != NULL; hay++)
        if (strncmp(hay, needle, nlen) == 0)
            return (char *)hay;
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    return d ? memcpy(d, s, n) : NULL;
}
