/*
 * libc_port.h - the platform hooks the C library needs.
 *
 * The library itself never touches hardware.  These three functions are
 * implemented by the board support package (sw/bsp) on the SoC, and by the
 * test harness when the library is unit-tested on the host.
 */
#ifndef _LIBC_PORT_H
#define _LIBC_PORT_H

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS
/* Write `len` bytes to the console (stdout and stderr both end up here). */
void __libc_console_write(const char *buf, size_t len);

/* Called once, at the first allocation: the region [*start, *end) that
 * malloc may manage.  It must not overlap anything else. */
void __libc_heap_region(void **start, void **end);

/* Terminates the program with `status`.  Called by exit() after the atexit
 * handlers ran, and directly by _Exit()/abort().  Must not return. */
__noreturn_ void __libc_exit(int status);
__END_DECLS

#endif /* _LIBC_PORT_H */
