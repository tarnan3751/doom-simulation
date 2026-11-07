/*
 * malloc.h - allocator extensions (the standard functions are in <stdlib.h>).
 *
 * The allocator manages one fixed region obtained from the platform at the
 * first call (__libc_heap_region, see <libc_port.h>): boundary-tagged chunks,
 * 16-byte aligned payloads, segregated free lists, immediate coalescing.
 * Out of memory returns NULL with errno = ENOMEM.
 */
#ifndef _MALLOC_H
#define _MALLOC_H

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdlib.h>

struct malloc_stats {
    size_t heap_size;      /* bytes managed (whole region)              */
    size_t in_use;         /* bytes in allocated chunks (incl. headers)  */
    size_t free_bytes;     /* bytes in free chunks                       */
    size_t largest_free;   /* largest free chunk (max allocatable ~this) */
    size_t n_alloc;        /* number of allocated chunks                 */
    size_t n_free;         /* number of free chunks                      */
    size_t peak_in_use;    /* high-water mark of in_use                  */
};

__BEGIN_DECLS
void   malloc_get_stats(struct malloc_stats *st);
/* Walks the whole heap and the free lists; returns 0 if consistent, else a
 * negative code identifying the first inconsistency found. */
int    malloc_check(void);
__END_DECLS

#endif /* _MALLOC_H */
