/*
 * host_harness.c - the library's platform hooks (<libc_port.h>) for the
 * host unit tests: console output is captured in a buffer, the heap is a
 * static array, and exit() longjmps back into the test.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tlibc.h"

#define CONSOLE_CAP (1u << 20)
char   tl_console[CONSOLE_CAP + 1];
size_t tl_console_len;

void tl_console_reset(void)
{
    tl_console_len = 0;
    tl_console[0] = '\0';
}

void __libc_console_write(const char *buf, size_t len)
{
    size_t room = CONSOLE_CAP - tl_console_len;
    size_t k = len < room ? len : room;
    memcpy(tl_console + tl_console_len, buf, k);
    tl_console_len += k;
    tl_console[tl_console_len] = '\0';
}

#define HEAP_SIZE (64u << 20)
static unsigned char *heap_area;

void __libc_heap_region(void **start, void **end)
{
    heap_area = malloc(HEAP_SIZE + 64);          /* host malloc */
    if (!heap_area) {
        *start = *end = NULL;
        return;
    }
    /* deliberately misaligned bounds: the allocator must align itself */
    *start = heap_area + 3;
    *end = heap_area + 3 + HEAP_SIZE - 5;
}

static jmp_buf exit_jmp;
static int     exit_armed;
static int     exit_status;

void __libc_exit(int status)
{
    if (exit_armed) {
        exit_status = status;
        longjmp(exit_jmp, 1);
    }
    fprintf(stderr, "FATAL: library exit path reached outside tl_catch_exit (status %d)\n", status);
    fprintf(stderr, "console so far:\n%s\n", tl_console);
    exit(99);
}

int tl_catch_exit(void (*fn)(void *), void *arg)
{
    exit_armed = 1;
    if (setjmp(exit_jmp)) {
        exit_armed = 0;
        return exit_status;
    }
    fn(arg);
    exit_armed = 0;
    return -1000;
}
