/*
 * stdio.c - streams: console output and read-only in-memory files (no storage).
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <memfile.h>
#include <libc_port.h>
#include "libc_impl.h"

FILE __libc_stdout = { LIBC_F_INUSE | LIBC_F_WRITE | LIBC_F_CONSOLE, NULL, 0, 0 };
FILE __libc_stderr = { LIBC_F_INUSE | LIBC_F_WRITE | LIBC_F_CONSOLE, NULL, 0, 0 };

static FILE file_pool[FOPEN_MAX];

/* ---- memfile registry ------------------------------------------------------ */

static struct {
    char        name[MEMFILE_NAME_MAX];
    const void *data;
    size_t      size;
} memfiles[MEMFILE_MAX];
static int n_memfiles;

const char *__libc_basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static int memfile_find(const char *path)
{
    if (!path)
        return -1;
    const char *base = __libc_basename(path);
    if (!*base)
        return -1;
    for (int i = 0; i < n_memfiles; i++)
        if (strcasecmp(memfiles[i].name, base) == 0)
            return i;
    return -1;
}

int memfile_register(const char *name, const void *data, size_t size)
{
    const char *base = name ? __libc_basename(name) : "";
    size_t len = strlen(base);
    if (len == 0 || len >= MEMFILE_NAME_MAX || (!data && size)) {
        errno = EINVAL;
        return -1;
    }
    int i = memfile_find(base);
    if (i < 0) {
        if (n_memfiles >= MEMFILE_MAX) {
            errno = ENFILE;
            return -1;
        }
        i = n_memfiles++;
    }
    memcpy(memfiles[i].name, base, len + 1);
    memfiles[i].data = data;
    memfiles[i].size = size;
    return 0;
}

const void *memfile_lookup(const char *path, size_t *size)
{
    int i = memfile_find(path);
    if (i < 0) {
        errno = ENOENT;
        return NULL;
    }
    if (size)
        *size = memfiles[i].size;
    return memfiles[i].data ? memfiles[i].data : (const void *)"";
}

/* ---- opening and closing ------------------------------------------------------ */

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }
    int writing = mode[0] != 'r' || strchr(mode, '+') != NULL;
    int i = memfile_find(path);
    if (i < 0) {
        errno = writing ? EROFS : ENOENT;
        return NULL;
    }
    if (writing) {
        errno = EROFS;
        return NULL;
    }
    for (int k = 0; k < FOPEN_MAX; k++) {
        FILE *f = &file_pool[k];
        if (!(f->flags & LIBC_F_INUSE)) {
            f->flags = LIBC_F_INUSE | LIBC_F_READ | LIBC_F_MEM;
            f->data = memfiles[i].data;
            f->size = memfiles[i].size;
            f->pos = 0;
            return f;
        }
    }
    errno = EMFILE;
    return NULL;
}

static int is_pool_file(FILE *f)
{
    return f >= file_pool && f < file_pool + FOPEN_MAX;
}

int fclose(FILE *f)
{
    if (!f || !(f->flags & LIBC_F_INUSE)) {
        errno = EBADF;
        return EOF;
    }
    if (is_pool_file(f))
        f->flags = 0;
    return 0;                   /* the standard streams stay usable */
}

int fflush(FILE *f)
{
    (void)f;                    /* nothing is ever buffered */
    return 0;
}

/* ---- reading -------------------------------------------------------------------- */

static int readable(FILE *f)
{
    if (!f || !(f->flags & LIBC_F_INUSE) || !(f->flags & LIBC_F_READ)) {
        errno = EBADF;
        return 0;
    }
    return 1;
}

size_t fread(void *restrict ptr, size_t size, size_t nmemb, FILE *restrict f)
{
    size_t total;
    if (!readable(f))
        return 0;
    if (size == 0 || nmemb == 0)
        return 0;
    if (__builtin_mul_overflow(size, nmemb, &total))
        total = (size_t)-1;
    size_t got = 0;
    if (f->flags & LIBC_F_MEM) {
        size_t avail = f->pos < f->size ? f->size - f->pos : 0;
        got = total < avail ? total : avail;
        memcpy(ptr, f->data + f->pos, got);
        f->pos += got;
    }
    return got / size;
}

/* ---- positioning ------------------------------------------------------------------ */

int fseek(FILE *f, long offset, int whence)
{
    if (!f || !(f->flags & LIBC_F_INUSE)) {
        errno = EBADF;
        return -1;
    }
    if (!(f->flags & LIBC_F_MEM)) {
        errno = ESPIPE;
        return -1;
    }
    long base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (long)f->pos; break;
    case SEEK_END: base = (long)f->size; break;
    default: errno = EINVAL; return -1;
    }
    if ((offset > 0 && base > LONG_MAX - offset) || base + offset < 0) {
        errno = EINVAL;
        return -1;
    }
    f->pos = (size_t)(base + offset);
    return 0;
}

long ftell(FILE *f)
{
    if (!f || !(f->flags & LIBC_F_INUSE)) {
        errno = EBADF;
        return -1;
    }
    if (!(f->flags & LIBC_F_MEM)) {
        errno = ESPIPE;
        return -1;
    }
    return (long)f->pos;
}

/* ---- writing ---------------------------------------------------------------------- */

static int writable(FILE *f)
{
    if (!f || (f->flags & (LIBC_F_INUSE | LIBC_F_CONSOLE)) != (LIBC_F_INUSE | LIBC_F_CONSOLE)) {
        errno = EBADF;
        return 0;
    }
    return 1;
}

size_t fwrite(const void *restrict ptr, size_t size, size_t nmemb, FILE *restrict f)
{
    size_t total;
    if (size == 0 || nmemb == 0)
        return 0;
    if (!writable(f))
        return 0;
    if (__builtin_mul_overflow(size, nmemb, &total))
        return 0;
    __libc_console_write(ptr, total);
    return nmemb;
}

int fputc(int c, FILE *f)
{
    if (!writable(f))
        return EOF;
    char ch = (char)c;
    __libc_console_write(&ch, 1);
    return (unsigned char)c;
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int puts(const char *s)
{
    __libc_console_write(s, strlen(s));
    __libc_console_write("\n", 1);
    return 0;
}

/* ---- no storage: removing and renaming fail ------------------------------------------ */

int remove(const char *path)
{
    errno = memfile_find(path) >= 0 ? EROFS : ENOENT;
    return -1;
}

int rename(const char *oldpath, const char *newpath)
{
    (void)newpath;
    errno = memfile_find(oldpath) >= 0 ? EROFS : ENOENT;
    return -1;
}
