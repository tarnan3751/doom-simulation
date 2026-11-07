/*
 * memfile.h - read-only in-memory files for the stdio layer.
 *
 * The BSP registers the WAD from the boot-info page at startup; a program
 * may register more (e.g. tests).  fopen(path, "r"/"rb") succeeds when the
 * last component of `path` equals a registered name, compared without
 * regard to ASCII case ("./DOOM1.WAD" opens "doom1.wad").
 */
#ifndef _MEMFILE_H
#define _MEMFILE_H

#include <sys/cdefs.h>
#include <stddef.h>

#define MEMFILE_MAX      8    /* registered files */
#define MEMFILE_NAME_MAX 64   /* including the NUL */

__BEGIN_DECLS
/* Registers `size` bytes at `data` (not copied; must stay valid) under
 * `name` (copied; directories are stripped).  Re-registering a name replaces
 * it.  Returns 0, or -1 with errno = EINVAL (bad name) / ENFILE (table full). */
int memfile_register(const char *name, const void *data, size_t size);

/* Zero-copy access: returns the data of the file `path` would open and
 * stores its size in *size (if non-NULL), or returns NULL (errno = ENOENT). */
const void *memfile_lookup(const char *path, size_t *size);
__END_DECLS

#endif /* _MEMFILE_H */
