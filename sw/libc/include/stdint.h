/*
 * stdint.h - fixed-width integer types.
 *
 * The compiler already knows every type and limit; this header only routes to
 * its freestanding implementation.  GCC's own <stdint.h> would `#include_next`
 * a C library's stdint.h in hosted mode, so in hosted builds this file must
 * come first in the include path (it does: -isystem sw/libc/include).
 */
#ifndef _LIBC_STDINT_H
#define _LIBC_STDINT_H

#if defined(__clang__)
/* Host unit tests (clang, -ffreestanding -nostdlibinc): clang's header is
 * self-contained when there is no system stdint.h to chain to. */
# include_next <stdint.h>
#else
# include <stdint-gcc.h>
#endif

#endif /* _LIBC_STDINT_H */
