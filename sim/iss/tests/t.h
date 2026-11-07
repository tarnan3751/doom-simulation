#pragma once
#include <stdint.h>
#include "soc.h"
extern volatile uint32_t tr_cause, tr_mepc, tr_mtval, tr_count;
#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define EXIT(c) do { R32(SYS_EXIT) = (c); for (;;) ; } while (0)
#define CHECK(n, cond) do { if (!(cond)) EXIT(n); } while (0)
