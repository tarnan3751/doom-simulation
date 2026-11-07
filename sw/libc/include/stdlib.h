/*
 * stdlib.h - memory allocation, conversions, process control.
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <sys/cdefs.h>
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

__BEGIN_DECLS
/* memory (see <malloc.h> for the extensions) */
void *malloc(size_t size) __malloclike_;
void *calloc(size_t nmemb, size_t size) __malloclike_;
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* numeric conversions (integers only: no program here parses floating point) */
int           atoi(const char *s);
/* Declared, not defined: doomgeneric's m_config.c calls it in code that the
 * link discards (no config files).  A call that is ever linked fails to link. */
double        atof(const char *s);
long          strtol(const char *__restrict_ s, char **__restrict_ end, int base);
unsigned long strtoul(const char *__restrict_ s, char **__restrict_ end, int base);

int abs(int j);   /* GCC always expands it inline: there is no library copy */

/* process control */
int   atexit(void (*func)(void));
__noreturn_ void exit(int status);
__noreturn_ void abort(void);
__END_DECLS

#endif /* _STDLIB_H */
