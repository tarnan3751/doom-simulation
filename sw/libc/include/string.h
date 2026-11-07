/*
 * string.h - memory and string functions (C "C" locale only).
 */
#ifndef _STRING_H
#define _STRING_H

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS
void  *memcpy(void *__restrict_ dst, const void *__restrict_ src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n) __pure_;
void  *memchr(const void *s, int c, size_t n) __pure_;

size_t strlen(const char *s) __pure_;
size_t strnlen(const char *s, size_t maxlen) __pure_;
char  *strncpy(char *__restrict_ dst, const char *__restrict_ src, size_t n);
int    strcmp(const char *a, const char *b) __pure_;
int    strncmp(const char *a, const char *b, size_t n) __pure_;
char  *strchr(const char *s, int c) __pure_;
char  *strrchr(const char *s, int c) __pure_;
char  *strstr(const char *hay, const char *needle) __pure_;
char  *strdup(const char *s) __malloclike_;
__END_DECLS

/* strcasecmp & co. live in <strings.h>; many programs expect them here too. */
#include <strings.h>

#endif /* _STRING_H */
