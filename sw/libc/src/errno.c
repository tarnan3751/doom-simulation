/*
 * errno.c - errno storage and the assert() back end.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int __libc_errno;

void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    fprintf(stderr, "Assertion failed: %s (%s:%d: %s)\n", expr, file, line, func);
    abort();
}
