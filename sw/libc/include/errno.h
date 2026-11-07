/*
 * errno.h - the single-threaded errno and the error numbers (Linux values)
 * that the library and its programs use.
 */
#ifndef _ERRNO_H
#define _ERRNO_H

#include <sys/cdefs.h>

__BEGIN_DECLS
extern int __libc_errno;
__END_DECLS
#define errno __libc_errno

#define ENOENT        2
#define EBADF         9
#define ENOMEM       12
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ESPIPE       29
#define EROFS        30
#define ERANGE       34
#define EOVERFLOW    75

#endif /* _ERRNO_H */
