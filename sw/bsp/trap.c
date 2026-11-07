/*
 * trap.c - report an unexpected exception and stop the run.
 *
 * There are no interrupts and no system calls, so any trap is a bug.  The
 * report goes straight to the UART (no libc: the heap or stdio may be the
 * thing that is broken) and contains what is needed to find the bug without
 * a debugger: cause, faulting pc/address/instruction with a plain-language
 * hint, all registers, the stack state, and the code addresses found on the
 * stack (a poor man's backtrace) ready to paste into addr2line.
 */
#include <stdint.h>
#include <stddef.h>
#include "bsp.h"

extern char __text_start[], __text_end[], __stack_limit[], __stack_top[];
extern char __heap_start[], __bss_start[], __bss_end[];

static const char *const abi_names[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1",
    "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static const char *cause_name(uint32_t c)
{
    switch (c) {
    case 0:  return "instruction address misaligned";
    case 1:  return "instruction access fault";
    case 2:  return "illegal instruction";
    case 3:  return "breakpoint (ebreak)";
    case 4:  return "load address misaligned";
    case 5:  return "load access fault";
    case 6:  return "store address misaligned";
    case 7:  return "store access fault";
    case 8:  return "ecall from U-mode";
    case 11: return "ecall from M-mode";
    default: return "unknown cause";
    }
}

#define CSRR(name) ({ uint32_t v_; __asm__ volatile("csrr %0, " #name : "=r"(v_)); v_; })

static int in_text(uint32_t a)
{
    return a >= (uint32_t)(uintptr_t)__text_start && a < (uint32_t)(uintptr_t)__text_end;
}

static void pad_to(const char *s, int width)
{
    int n = 0;
    while (s[n])
        n++;
    bsp_puts(s);
    while (n++ < width)
        bsp_putc(' ');
}

static void hint(const char *s)
{
    bsp_puts("           -> ");
    bsp_puts(s);
    bsp_putc('\n');
}

static void describe_address(uint32_t a)
{
    if (a < 0x1000)
        hint("address is near 0: NULL pointer dereference (plus offset)?");
    else if (a >= (uint32_t)(uintptr_t)__stack_limit - 0x10000 && a < (uint32_t)(uintptr_t)__stack_limit)
        hint("address is just below the stack reserve: stack overflow?");
    else if (a >= 0x80000000u && a < 0x82000000u)
        hint("address is in RAM but misaligned: packed struct / bad cast / corrupted pointer?");
    else if (a >= 0x10000000u && a < 0x10020000u)
        hint("address is in the MMIO window (peripheral registers are 32-bit, word aligned)");
    else
        hint("address is outside RAM and MMIO: wild or uninitialised pointer?");
}

__attribute__((noreturn, used))
void __bsp_trap_handler(uint32_t *regs)
{
    static volatile int depth;
    if (depth++) {
        bsp_puts("\n*** TRAP while reporting a trap (mcause ");
        bsp_putdec(CSRR(mcause));
        bsp_puts(", mepc ");
        bsp_puthex(CSRR(mepc));
        bsp_puts(") -- halting\n");
        bsp_exit(BSP_EXIT_TRAP_NESTED);
    }
    uint32_t mcause = CSRR(mcause), mepc = CSRR(mepc), mtval = CSRR(mtval);
    uint32_t mstatus = CSRR(mstatus);
    uint64_t cycles = bsp_cycles(), instret = bsp_instret();
    uint32_t code = mcause & 0x7FFFFFFFu;
    int exitcode = BSP_EXIT_TRAP_BASE + (int)(code & 0xF);

    bsp_puts("\n\n*** TRAP: ");
    bsp_puts(cause_name(code));
    bsp_puts(" (mcause ");
    bsp_putdec(mcause);
    bsp_puts(")\n");

    /* the faulting instruction */
    bsp_puts("  mepc   = ");
    bsp_puthex(mepc);
    int mepc_ok = in_text(mepc) && !(mepc & 3);
    if (mepc_ok) {
        uint32_t insn = *(const volatile uint32_t *)(uintptr_t)mepc;
        bsp_puts("   insn ");
        bsp_puthex(insn);
        bsp_putc('\n');
        if (insn == 0xC0001073u)
            hint("'unimp': a compiler-generated trap (__builtin_trap / unreachable code reached)");
        else if (insn == 0)
            hint("all-zero instruction word: code was overwritten?");
        else if (code == 3)
            hint("ebreak: __builtin_trap(), or a breakpoint left in the code");
    } else {
        bsp_putc('\n');
        if (code == 0 || code == 1 || code == 2)
            hint("pc is outside the program's code: jump through a corrupted function pointer or return address (check ra below)");
    }

    bsp_puts("  mtval  = ");
    bsp_puthex(mtval);
    switch (code) {
    case 0:
        bsp_puts("   (misaligned jump/branch target)\n");
        break;
    case 4: case 5: case 6: case 7:
        bsp_puts(code <= 5 ? "   (load address)\n" : "   (store address)\n");
        describe_address(mtval);
        break;
    case 2:
        bsp_puts("   (instruction bits, if the core reports them)\n");
        hint("not RV32IM+Zicsr (built for the wrong -march?) or an unsupported CSR, or a jump into data");
        break;
    case 11:
        bsp_putc('\n');
        hint("ecall: there is no operating system to handle system calls here");
        break;
    default:
        bsp_putc('\n');
        break;
    }
    bsp_puts("  mstatus= ");
    bsp_puthex(mstatus);
    bsp_puts("   cycle ");
    bsp_putdec(cycles);
    bsp_puts("   instret ");
    bsp_putdec(instret);
    bsp_putc('\n');

    /* registers, four per line */
    for (int i = 0; i < 32; i += 4) {
        for (int j = i; j < i + 4; j++) {
            bsp_puts("  ");
            pad_to(abi_names[j], 4);
            bsp_putc(' ');
            bsp_puthex(regs[j]);
        }
        bsp_putc('\n');
    }

    /* the stack */
    uint32_t sp = regs[2];
    uint32_t lim = (uint32_t)(uintptr_t)__stack_limit, top = (uint32_t)(uintptr_t)__stack_top;
    bsp_puts("  stack: sp ");
    bsp_puthex(sp);
    bsp_puts(", reserve ");
    bsp_puthex(lim);
    bsp_puts("..");
    bsp_puthex(top);
    bsp_puts(", deepest use ");
    bsp_putdec(bsp_stack_used());
    bsp_puts(" bytes\n");
    if (sp < lim || sp > top)
        hint("sp is OUTSIDE the stack reserve: stack overflow or corrupted sp");
    else if (bsp_stack_overflowed())
        hint("the stack guard at the bottom of the reserve was overwritten: stack overflow");
    if (sp & 15)
        hint("sp is not 16-byte aligned (ABI violation / corrupted sp)");

    /* code addresses on the stack, newest first */
    uint32_t found[16];
    int nfound = 0;
    if (!(sp & 3) && sp >= lim && sp < top) {
        const uint32_t *p = (const uint32_t *)(uintptr_t)sp;
        for (int i = 0; i < 4096 && (uint32_t)(uintptr_t)(p + i) < top && nfound < 16; i++) {
            uint32_t w = p[i];
            if (in_text(w) && !(w & 3) && w != (uint32_t)(uintptr_t)__text_start)
                found[nfound++] = w;
        }
    }
    bsp_puts("  code addresses on the stack (return-address candidates, newest first):\n   ");
    if (!nfound)
        bsp_puts(" (none)");
    for (int i = 0; i < nfound; i++) {
        bsp_putc(' ');
        bsp_puthex(found[i]);
    }
    bsp_puts("\n  to decode: riscv64-elf-addr2line -f -p -i -e <program>.elf ");
    bsp_puthex(mepc);
    if (in_text(regs[1])) {
        bsp_putc(' ');
        bsp_puthex(regs[1]);
    }
    for (int i = 0; i < nfound && i < 8; i++) {
        bsp_putc(' ');
        bsp_puthex(found[i]);
    }
    bsp_puts("\n*** halting, exit code ");
    bsp_puthex((uint32_t)exitcode);
    bsp_putc('\n');
    bsp_exit(exitcode);
}
