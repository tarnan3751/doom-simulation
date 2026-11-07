/*
 * soc.h - register map of the SoC, as seen from C.
 *
 * This file mirrors docs/ARCHITECTURE.md sections 2, 7 and 9 exactly.
 * It is shared by every program (and by host-side tools that need the same
 * constants), so it only contains constants and trivial inline accessors.
 */
#ifndef SOC_H
#define SOC_H

#include <stdint.h>

/* ---- address map ------------------------------------------------------- */
#define RAM_BASE            0x80000000u
#define RAM_SIZE            0x02000000u   /* 32 MB in simulation               */
#define PROGRAM_RAM_SIZE    0x00800000u   /* program image must fit in 8 MB    */

#define BOOTINFO_ADDR       0x807FF000u
#define BOOTINFO_MAGIC      0xB007F00Du
#define STACK_TOP           0x807FF000u
#define WAD_ADDR            0x80800000u

#define UART_BASE           0x10000000u
#define SYS_BASE            0x10001000u
#define KBD_BASE            0x10002000u
#define VID_BASE            0x10003000u
#define VID_PALETTE_BASE    0x10003400u
#define VID_FB_BASE         0x10010000u

/* ---- UART -------------------------------------------------------------- */
#define UART_TX             (UART_BASE + 0x0)   /* W: byte to send             */
#define UART_STATUS         (UART_BASE + 0x4)   /* R: bit0 = tx_ready          */
#define UART_STATUS_TX_READY  (1u << 0)

/* ---- system control ---------------------------------------------------- */
#define SYS_CLK_HZ          (SYS_BASE + 0x0)    /* R: clock frequency in Hz    */
#define SYS_EXIT            (SYS_BASE + 0x4)    /* W: exit code, halts the run */
#define SYS_FLAGS           (SYS_BASE + 0x8)    /* R: bit0 = FAST build        */
#define SYS_FLAGS_FAST      (1u << 0)

/* ---- keyboard ---------------------------------------------------------- */
#define KBD_DATA            (KBD_BASE + 0x0)    /* R: pops one event           */
#define KBD_STATUS          (KBD_BASE + 0x4)    /* R: bit0 = event waiting     */
#define KBD_DATA_VALID      (1u << 31)
#define KBD_DATA_PRESSED    (1u << 8)
#define KBD_DATA_CODE_MASK  0xFFu

/* ---- video ------------------------------------------------------------- */
#define VID_FLIP            (VID_BASE + 0x0)    /* W: frame complete; R: count */
#define VID_STATUS          (VID_BASE + 0x4)    /* R: bit0 = outside active fb */
#define VID_STATUS_VBLANK   (1u << 0)
#define VID_WIDTH           320
#define VID_HEIGHT          200
#define VID_FB_BYTES        (VID_WIDTH * VID_HEIGHT)

/* ---- boot info page (written by the board before reset is released) ---- */
struct bootinfo {            /* 1040 bytes */
    uint32_t magic;          /* BOOTINFO_MAGIC when valid                      */
    uint32_t wad_addr;       /* WAD_ADDR, or 0 when no WAD was loaded          */
    uint32_t wad_size;       /* bytes                                          */
    uint32_t reserved;
    char     wad_name[64];   /* WAD file name without directories              */
    char     cmdline[960];   /* NUL-terminated, space separated (argv[1..])    */
};
#define BOOTINFO            ((const struct bootinfo *)BOOTINFO_ADDR)

/* ---- accessors (target only) ------------------------------------------- */
#if defined(__riscv)
static inline uint32_t mmio_read(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void mmio_write(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

/* 64-bit cycle counter (Zicntr). Re-reads the high half to stay consistent. */
static inline uint64_t rdcycle64(void)
{
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("rdcycleh %0" : "=r"(hi));
        __asm__ volatile("rdcycle  %0" : "=r"(lo));
        __asm__ volatile("rdcycleh %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

#define VID_FB       ((volatile uint8_t  *)(uintptr_t)VID_FB_BASE)
#define VID_FB32     ((volatile uint32_t *)(uintptr_t)VID_FB_BASE)
#define VID_PALETTE  ((volatile uint32_t *)(uintptr_t)VID_PALETTE_BASE)
#endif /* __riscv */

#endif /* SOC_H */
