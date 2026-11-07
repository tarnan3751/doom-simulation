/*
 * crt0.S - reset entry and trap entry.
 *
 * The board copies the ELF's loadable segments into RAM and releases reset
 * with pc = 0x8000_0000, where the linker script places .text.init.
 */
    .section .text.init, "ax", @progbits
    .globl  _start
    .type   _start, @function
_start:
    /* gp first: the linker may relax later accesses relative to it */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top             /* STACK_TOP = 0x807FF000, grows down */

    /* Any exception from here on lands in the trap handler, which runs on
     * its own small stack (mscratch) so a broken sp cannot hide the cause. */
    la      t0, __bsp_trap_entry
    csrw    mtvec, t0
    la      t0, __trap_stack_top
    csrw    mscratch, t0

    /* clear .sbss + .bss (16-byte aligned on both ends) */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    sw      zero, 4(t0)
    sw      zero, 8(t0)
    sw      zero, 12(t0)
    addi    t0, t0, 16
    j       1b
2:
    /* Paint the whole stack reserve: the BSP finds the deepest stack use and
     * detects an overflow (guard words at the bottom) from this pattern. */
    la      t0, __stack_limit
    la      t1, __stack_top
    li      t2, 0x57ACC0DE              /* BSP_STACK_PAINT */
3:  bgeu    t0, t1, 4f
    sw      t2, 0(t0)
    sw      t2, 4(t0)
    sw      t2, 8(t0)
    sw      t2, 12(t0)
    addi    t0, t0, 16
    j       3b
4:
    /* board init: argc/argv from the boot-info page, WAD file, constructors */
    addi    sp, sp, -16
    mv      a0, sp                      /* char **argv out-parameter */
    call    __bsp_init                  /* returns argc */
    lw      a1, 0(sp)
    addi    sp, sp, 16
    li      a2, 0                       /* envp */
    call    main
    call    exit                        /* never returns */
5:  j       5b
    .size   _start, . - _start

/*
 * Trap entry (mtvec, direct mode).  There are no interrupts, so every trap
 * is an exception the program did not expect: save all registers on the
 * trap stack and let the C handler print a report and stop the run.
 */
    .section .text.trap, "ax", @progbits
    .balign 64
    .globl  __bsp_trap_entry
    .type   __bsp_trap_entry, @function
__bsp_trap_entry:
    csrrw   sp, mscratch, sp            /* sp = trap stack, mscratch = old sp */
    addi    sp, sp, -128
    sw      zero, 0(sp)
    sw      x1, 4(sp)
    sw      x3, 12(sp)
    sw      x4, 16(sp)
    sw      x5, 20(sp)
    sw      x6, 24(sp)
    sw      x7, 28(sp)
    sw      x8, 32(sp)
    sw      x9, 36(sp)
    sw      x10, 40(sp)
    sw      x11, 44(sp)
    sw      x12, 48(sp)
    sw      x13, 52(sp)
    sw      x14, 56(sp)
    sw      x15, 60(sp)
    sw      x16, 64(sp)
    sw      x17, 68(sp)
    sw      x18, 72(sp)
    sw      x19, 76(sp)
    sw      x20, 80(sp)
    sw      x21, 84(sp)
    sw      x22, 88(sp)
    sw      x23, 92(sp)
    sw      x24, 96(sp)
    sw      x25, 100(sp)
    sw      x26, 104(sp)
    sw      x27, 108(sp)
    sw      x28, 112(sp)
    sw      x29, 116(sp)
    sw      x30, 120(sp)
    sw      x31, 124(sp)
    csrr    t0, mscratch
    sw      t0, 8(sp)                   /* the interrupted sp */
    mv      a0, sp                      /* uint32_t regs[32] */
    .option push
    .option norelax
    la      gp, __global_pointer$       /* in case gp was clobbered */
    .option pop
    call    __bsp_trap_handler          /* never returns */
6:  j       6b
    .size   __bsp_trap_entry, . - __bsp_trap_entry

    .section .bss.trapstack, "aw", @nobits
    .balign 16
__trap_stack:
    .space  2048
    .globl  __trap_stack_top
__trap_stack_top:
