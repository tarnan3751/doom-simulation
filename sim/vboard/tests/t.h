#pragma once
#include <stdint.h>
#include "soc.h"
#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define EXIT(c) do { R32(SYS_EXIT) = (c); for (;;) ; } while (0)
static inline void putc_(char c) { while (!(R32(UART_STATUS) & 1)) ; R32(UART_TX) = (uint8_t)c; }
static inline void puts_(const char *s) { while (*s) putc_(*s++); }
static inline void puthex(uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) putc_("0123456789abcdef"[(v >> (4 * i)) & 15]);
}
