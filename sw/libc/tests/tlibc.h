/*
 * tlibc.h - the library under test, as seen from host test code.
 *
 * Test sources are compiled against the HOST headers (the host libc is the
 * oracle).  The library itself was compiled with every public name renamed
 * to tl_<name> (HOST_RENAME in the Makefile).  The types below mirror the
 * library's own definitions.
 */
#ifndef TLIBC_H
#define TLIBC_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

typedef struct __libc_file TL_FILE;
extern TL_FILE __libc_stdout, __libc_stderr;
#define tl_stdout (&__libc_stdout)
#define tl_stderr (&__libc_stderr)
extern int __libc_errno;
extern const unsigned short __libc_ctype[384];      /* <ctype.h> tables, index c + 128 */
extern const short __libc_toupper_tab[384], __libc_tolower_tab[384];

struct tl_malloc_stats {              /* mirrors sw/libc/include/malloc.h */
    size_t heap_size, in_use, free_bytes, largest_free, n_alloc, n_free, peak_in_use;
};

/* string */
void  *tl_memcpy(void *, const void *, size_t);
void  *tl_memmove(void *, const void *, size_t);
void  *tl_memset(void *, int, size_t);
int    tl_memcmp(const void *, const void *, size_t);
void  *tl_memchr(const void *, int, size_t);
size_t tl_strlen(const char *);
size_t tl_strnlen(const char *, size_t);
char  *tl_strncpy(char *, const char *, size_t);
int    tl_strcmp(const char *, const char *);
int    tl_strncmp(const char *, const char *, size_t);
int    tl_strcasecmp(const char *, const char *);
int    tl_strncasecmp(const char *, const char *, size_t);
char  *tl_strchr(const char *, int);
char  *tl_strrchr(const char *, int);
char  *tl_strstr(const char *, const char *);
char  *tl_strdup(const char *);

/* stdlib */
void  *tl_malloc(size_t);
void  *tl_calloc(size_t, size_t);
void  *tl_realloc(void *, size_t);
void   tl_free(void *);
void   tl_malloc_get_stats(struct tl_malloc_stats *);
int    tl_malloc_check(void);
int    tl_atoi(const char *);
long          tl_strtol(const char *, char **, int);
unsigned long tl_strtoul(const char *, char **, int);
int    tl_atexit(void (*)(void));
void   tl_exit(int) __attribute__((noreturn));
void   tl_abort(void) __attribute__((noreturn));
void   tl___assert_fail(const char *, const char *, int, const char *) __attribute__((noreturn));

/* stdio */
TL_FILE *tl_fopen(const char *, const char *);
int      tl_fclose(TL_FILE *);
size_t   tl_fread(void *, size_t, size_t, TL_FILE *);
size_t   tl_fwrite(const void *, size_t, size_t, TL_FILE *);
int      tl_fseek(TL_FILE *, long, int);
long     tl_ftell(TL_FILE *);
int      tl_fputc(int, TL_FILE *);
int      tl_puts(const char *);
int      tl_putchar(int);
int      tl_remove(const char *);
int      tl_rename(const char *, const char *);
int      tl_printf(const char *, ...);
int      tl_fprintf(TL_FILE *, const char *, ...);
int      tl_snprintf(char *, size_t, const char *, ...);
int      tl_sscanf(const char *, const char *, ...);
int      tl_memfile_register(const char *, const void *, size_t);
const void *tl_memfile_lookup(const char *, size_t *);

/* harness (host_harness.c) */
extern char   tl_console[];
extern size_t tl_console_len;
void tl_console_reset(void);
/* Runs fn(arg); returns the status passed to the library's exit path, or
 * -1000 if fn returned normally. */
int  tl_catch_exit(void (*fn)(void *), void *arg);

#endif /* TLIBC_H */
