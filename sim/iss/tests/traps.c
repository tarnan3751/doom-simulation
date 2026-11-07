#include "t.h"

/* Check that exactly one more trap happened since *seen, with the given fields. */
static uint32_t seen;
static int expect(uint32_t cause, uint32_t mepc, uint32_t mtval)
{
    /* mtval as the RTL core sets it: illegal instruction -> the instruction word,
     * ebreak -> its own pc; callers pass 0 for those two causes. */
    if (cause == 2) mtval = *(const volatile uint32_t *)mepc;
    else if (cause == 3) mtval = mepc;
    int ok = tr_count == seen + 1 && tr_cause == cause && tr_mepc == mepc && tr_mtval == mtval;
    seen = tr_count;
    return ok;
}

static uint32_t rd_csr_mstatus(void) { uint32_t v; __asm__ volatile("csrr %0, mstatus" : "=r"(v)); return v; }

volatile uint32_t mem_word = 0x11223344;
volatile uint8_t scratch[16] __attribute__((aligned(4)));

int main(void)
{
    uint32_t a, b, c, d, e, f, v = 0, pc;

    /* ---- counters: first, while no trap has happened ---- */
    __asm__ volatile(
        "csrr %0, mcycle\n csrr %1, mcycle\n csrr %2, minstret\n csrr %3, cycle\n csrr %4, instret\n"
        "csrr %5, cycleh\n"
        : "=r"(a), "=r"(b), "=r"(c), "=r"(d), "=r"(e), "=r"(f));
    CHECK(1, b == a + 1);          /* one cycle per instruction */
    CHECK(2, c == b + 1);          /* mcycle == minstret while nothing trapped */
    CHECK(3, d == c + 1);          /* cycle shadows mcycle */
    CHECK(4, e == d + 1);          /* instret shadows minstret */
    CHECK(5, f == 0);
    CHECK(6, tr_count == 0);

    /* counters are writable */
    __asm__ volatile("csrw mcycle, %1\n csrr %0, mcycle" : "=r"(a) : "r"(1000u));
    CHECK(7, a == 1001);
    __asm__ volatile("csrw mcycleh, %1\n csrr %0, mcycleh" : "=r"(a) : "r"(5u));
    CHECK(8, a == 5);

    /* ---- plain CSRs ---- */
    __asm__ volatile("csrr %0, misa" : "=r"(a));
    CHECK(10, a == 0x40001100u);
    __asm__ volatile("csrw misa, zero\n csrr %0, misa" : "=r"(a));
    CHECK(11, a == 0x40001100u && tr_count == 0);       /* writes ignored, no trap */
    __asm__ volatile("csrr %0, mhartid\n csrr %1, mvendorid\n csrr %2, marchid\n csrr %3, mimpid"
                     : "=r"(a), "=r"(b), "=r"(c), "=r"(d));
    CHECK(12, !a && !b && !c && !d);
    __asm__ volatile("csrw mscratch, %1\n csrr %0, mscratch" : "=r"(a) : "r"(0xCAFEF00Du));
    CHECK(13, a == 0xCAFEF00Du);
    __asm__ volatile("csrw mie, %2\n csrr %0, mie\n csrr %1, mip" : "=&r"(a), "=&r"(c) : "r"(0xFFFFFFFFu));
    CHECK(14, a == 0xFFFFFFFFu && c == 0);
    __asm__ volatile("csrw mie, zero");
    __asm__ volatile("csrr %0, mtvec" : "=r"(c));        /* remember the handler address */
    __asm__ volatile("csrw mtvec, %1\n csrr %0, mtvec\n csrw mtvec, %2" : "=&r"(a) : "r"(0x80001237u), "r"(c));
    CHECK(15, a == 0x80001234u);                         /* direct mode, 4-byte aligned */
    __asm__ volatile("csrw mepc, %1\n csrr %0, mepc" : "=r"(a) : "r"(0x80001237u));
    CHECK(16, a == 0x80001234u);
    __asm__ volatile("csrw mepc, zero");

    /* ---- illegal CSR accesses ---- */
    __asm__ volatile("lla %0, 1f\n 1: csrr %1, 0x7c0" : "=&r"(pc), "+r"(v));  /* unknown CSR */
    CHECK(20, expect(2, pc, 0));
    v = 0x1234;
    __asm__ volatile("lla %0, 1f\n 1: csrr %1, 0x7c0" : "=&r"(pc), "+r"(v));
    CHECK(21, v == 0x1234);                                /* rd not written by a trapping insn */
    seen = tr_count;
    __asm__ volatile("lla %0, 1f\n 1: csrw cycle, zero" : "=&r"(pc));        /* read-only shadow */
    CHECK(22, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: csrw instreth, zero" : "=&r"(pc));
    CHECK(23, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: csrs mhartid, %1" : "=&r"(pc) : "r"(1u));   /* read-only, rs1 != 0 */
    CHECK(24, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: csrwi mvendorid, 1" : "=&r"(pc));
    CHECK(25, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: csrr %1, time" : "=&r"(pc), "=r"(v));        /* not implemented */
    CHECK(26, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: csrr %1, 0xC03" : "=&r"(pc), "=r"(v));       /* hpmcounter3 */
    CHECK(27, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: csrr %1, 0x320" : "=&r"(pc), "=r"(v));       /* mcountinhibit */
    CHECK(28, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: csrr %1, sstatus" : "=&r"(pc), "=r"(v));
    CHECK(29, expect(2, pc, 0));
    /* read-only CSR read with rs1 = x0 / set with rs1 = x0 must NOT trap */
    __asm__ volatile("csrr %0, cycle\n csrrs %1, instret, zero\n csrrsi %2, mhartid, 0" : "=r"(a), "=r"(b), "=r"(c));
    CHECK(30, tr_count == seen);

    /* ---- illegal instructions ---- */
    __asm__ volatile("lla %0, 1f\n 1: .word 0xFFFFFFFF" : "=&r"(pc));
    CHECK(31, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: .word 0" : "=&r"(pc));
    CHECK(32, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: .word 0x02001013" : "=&r"(pc));   /* slli x0,x0,32: shamt[5] set */
    CHECK(33, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: .word 0x10200073" : "=&r"(pc));   /* sret */
    CHECK(34, expect(2, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: .word 0x12000073" : "=&r"(pc));   /* sfence.vma */
    CHECK(35, expect(2, pc, 0));

    /* ---- ecall / ebreak ---- */
    __asm__ volatile("lla %0, 1f\n 1: ecall" : "=&r"(pc));
    CHECK(40, expect(11, pc, 0));
    __asm__ volatile("lla %0, 1f\n 1: ebreak" : "=&r"(pc));
    CHECK(41, expect(3, pc, 0));

    /* ---- wfi / fence / fence.i are no-ops ---- */
    __asm__ volatile("wfi\n fence\n .word 0x0000100f" ::: "memory");
    CHECK(42, tr_count == seen);

    /* ---- mstatus ---- */
    __asm__ volatile("csrci mstatus, 8");
    __asm__ volatile("lla %0, 1f\n 1: ecall" : "=&r"(pc));
    seen = tr_count;
    CHECK(50, rd_csr_mstatus() == 0x1880u);        /* MIE=0, MPIE=1 (mret), MPP=3 */
    __asm__ volatile("csrsi mstatus, 8");
    __asm__ volatile("ecall");
    seen = tr_count;
    CHECK(51, rd_csr_mstatus() == 0x1888u);        /* MIE restored from MPIE, MPIE=1 */
    __asm__ volatile("csrw mstatus, zero");
    CHECK(52, rd_csr_mstatus() == 0x1800u);        /* MPP always reads 3 */

    /* ---- misaligned data accesses ---- */
    uintptr_t base = (uintptr_t)&mem_word;
    v = 0x5A5A;
    __asm__ volatile("lla %0, 1f\n 1: lw %1, 1(%2)" : "=&r"(pc), "+r"(v) : "r"(base));
    CHECK(60, expect(4, pc, base + 1) && v == 0x5A5A);
    __asm__ volatile("lla %0, 1f\n 1: lw %1, 2(%2)" : "=&r"(pc), "+r"(v) : "r"(base));
    CHECK(61, expect(4, pc, base + 2));
    __asm__ volatile("lla %0, 1f\n 1: lh %1, 1(%2)" : "=&r"(pc), "+r"(v) : "r"(base));
    CHECK(62, expect(4, pc, base + 1));
    __asm__ volatile("lla %0, 1f\n 1: lhu %1, 3(%2)" : "=&r"(pc), "+r"(v) : "r"(base));
    CHECK(63, expect(4, pc, base + 3));
    uintptr_t sb_ = (uintptr_t)scratch;
    __asm__ volatile("lla %0, 1f\n 1: sw %1, 1(%2)" : "=&r"(pc) : "r"(0xFFFFFFFFu), "r"(sb_) : "memory");
    CHECK(64, expect(6, pc, sb_ + 1));
    __asm__ volatile("lla %0, 1f\n 1: sh %1, 3(%2)" : "=&r"(pc) : "r"(0xFFFFFFFFu), "r"(sb_) : "memory");
    CHECK(65, expect(6, pc, sb_ + 3));
    __asm__ volatile("lla %0, 1f\n 1: sw %1, 2(%2)" : "=&r"(pc) : "r"(0xFFFFFFFFu), "r"(sb_) : "memory");
    CHECK(66, expect(6, pc, sb_ + 2));
    for (int i = 0; i < 8; i++) CHECK(67, scratch[i] == 0);   /* nothing was stored */
    /* aligned sub-word accesses work */
    __asm__ volatile("lh %0, 2(%1)" : "=r"(a) : "r"(base));
    CHECK(68, a == 0x1122);
    __asm__ volatile("lb %0, 3(%1)" : "=r"(a) : "r"(base));
    CHECK(69, a == 0x11);
    __asm__ volatile("lbu %0, 0(%1)" : "=r"(a) : "r"(base));
    CHECK(70, a == 0x44);
    CHECK(71, tr_count == seen);
    /* misaligned MMIO access also traps (checked before the address is decoded) */
    __asm__ volatile("lla %0, 1f\n 1: lw %1, 1(%2)" : "=&r"(pc), "+r"(v) : "r"(KBD_STATUS));
    CHECK(72, expect(4, pc, KBD_STATUS + 1));
    CHECK(73, R32(KBD_STATUS) == 0);

    /* ---- misaligned control transfers ---- */
    __asm__ volatile(
        "lla %0, 1f\n"
        ".option push\n.option norvc\n"
        "1: beq zero, zero, 2f\n"       /* taken, target = 2 mod 4: traps, handler resumes at the next insn */
        "j 3f\n"
        ".2byte 0\n"
        "2: .2byte 0\n"
        "3:\n"
        ".option pop\n"
        : "=&r"(pc));
    CHECK(80, tr_count == seen + 1 && tr_cause == 0 && tr_mepc == pc && tr_mtval == pc + 10);
    seen = tr_count;
    __asm__ volatile(
        "lla %0, 1f\n"
        ".option push\n.option norvc\n"
        "1: bne zero, zero, 2f\n"       /* NOT taken: no trap */
        "j 3f\n"
        ".2byte 0\n"
        "2: .2byte 0\n"
        "3:\n"
        ".option pop\n"
        : "=&r"(pc));
    CHECK(81, tr_count == seen);
    v = 0x77;
    __asm__ volatile(
        "lla %0, 1f\n"
        ".option push\n.option norvc\n"
        "1: jal %1, 2f\n"               /* link register must stay unwritten */
        "j 3f\n"
        ".2byte 0\n"
        "2: .2byte 0\n"
        "3:\n"
        ".option pop\n"
        : "=&r"(pc), "+r"(v));
    CHECK(82, tr_count == seen + 1 && tr_cause == 0 && tr_mepc == pc && tr_mtval == pc + 10 && v == 0x77);
    seen = tr_count;
    /* jalr: target = (rs1 + imm) & ~1, then alignment check */
    v = 0x77;
    __asm__ volatile("lla %0, 1f\n lla %1, 1f\n addi %1, %1, 2\n 1: jalr %2, 0(%1)" : "=&r"(pc), "=&r"(a), "+r"(v));
    CHECK(83, tr_count == seen + 1 && tr_cause == 0 && tr_mepc == pc && tr_mtval == pc + 2 && v == 0x77);
    seen = tr_count;
    __asm__ volatile("lla %0, 1f\n lla %1, 1f\n addi %1, %1, 3\n 1: jalr zero, 0(%1)" : "=&r"(pc), "=&r"(a));
    CHECK(84, tr_count == seen + 1 && tr_cause == 0 && tr_mepc == pc && tr_mtval == pc + 2);
    return 0;
}
