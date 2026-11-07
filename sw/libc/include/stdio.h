/*
 * stdio.h - formatted I/O and a minimal FILE layer.
 *
 * Streams:
 *   stdout, stderr  write to the console (the UART); unbuffered, fflush is a no-op.
 *   fopen(name,"r") opens a read-only in-memory file registered with
 *                   memfile_register() (see <memfile.h>); the WAD is one.
 *                   Names match case-insensitively, ignoring directories.
 *   Anything else (unknown names, "w"/"a"/"+" modes) fails with errno set,
 *   as on a machine without storage.  remove() and rename() always fail.
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdarg.h>

typedef struct __libc_file FILE;

#define EOF          (-1)
#define FOPEN_MAX    16
#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2

__BEGIN_DECLS
extern FILE __libc_stdout, __libc_stderr;
#define stdout (&__libc_stdout)
#define stderr (&__libc_stderr)

/* files */
FILE  *fopen(const char *__restrict_ path, const char *__restrict_ mode);
int    fclose(FILE *f);
int    fflush(FILE *f);
size_t fread(void *__restrict_ ptr, size_t size, size_t nmemb, FILE *__restrict_ f);
size_t fwrite(const void *__restrict_ ptr, size_t size, size_t nmemb, FILE *__restrict_ f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
int    remove(const char *path);
int    rename(const char *oldpath, const char *newpath);

/* character I/O */
int    fgetc(FILE *f);
int    ungetc(int c, FILE *f);
int    fputc(int c, FILE *f);
int    putchar(int c);
int    puts(const char *s);

/* formatted output: %d %i %u %o %x %X %c %s %p %n %% %f %F %e %E %g %G,
 * flags - + space # 0, width/precision (incl. *), hh h l ll j z t */
int printf(const char *__restrict_ fmt, ...) __printflike_(1, 2);
int fprintf(FILE *__restrict_ f, const char *__restrict_ fmt, ...) __printflike_(2, 3);
int snprintf(char *__restrict_ s, size_t n, const char *__restrict_ fmt, ...) __printflike_(3, 4);
int vfprintf(FILE *__restrict_ f, const char *__restrict_ fmt, va_list ap) __printflike_(2, 0);
int vsnprintf(char *__restrict_ s, size_t n, const char *__restrict_ fmt, va_list ap) __printflike_(3, 0);

/* formatted input from strings: %d %i %u %o %x %X %p %c %s %[...] %n %%,
 * assignment suppression (*), field width, hh h l ll j z t (no floating point) */
int sscanf(const char *__restrict_ s, const char *__restrict_ fmt, ...) __scanflike_(2, 3);
int vsscanf(const char *__restrict_ s, const char *__restrict_ fmt, va_list ap) __scanflike_(2, 0);
__END_DECLS

#endif /* _STDIO_H */
