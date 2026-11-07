# Minimal start-up for the directed tests. Installs a trap handler that records
# mcause/mepc/mtval, counts traps, and resumes after the faulting instruction.
# Uses x30/x31, which the tests compile with -ffixed-x30 -ffixed-x31.
    .section .text.init
    .globl _start
_start:
    la   sp, 0x807FF000
    la   t0, trap_entry
    csrw mtvec, t0
    call main
    li   t0, 0x10001004
    sw   a0, 0(t0)
1:  j    1b

    .balign 4
trap_entry:
    la   x31, tr_cause
    csrr x30, mcause
    sw   x30, 0(x31)
    csrr x30, mepc
    sw   x30, 4(x31)
    csrr x30, mtval
    sw   x30, 8(x31)
    lw   x30, 12(x31)
    addi x30, x30, 1
    sw   x30, 12(x31)
    csrr x30, mepc
    addi x30, x30, 4
    csrw mepc, x30
    mret

    .data
    .globl tr_cause, tr_mepc, tr_mtval, tr_count
tr_cause: .word 0
tr_mepc:  .word 0
tr_mtval: .word 0
tr_count: .word 0
