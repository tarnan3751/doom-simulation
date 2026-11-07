/*
 * link.ld - memory layout of every program (docs/ARCHITECTURE.md sections 2, 9).
 *
 *   0x8000_0000  .text (.text.init first: reset entry), .rodata, init/fini
 *                arrays, .data, .sdata, .sbss, .bss
 *   __heap_start ... __heap_end    malloc's region, grows up
 *   __stack_limit ... __stack_top  stack reserve (__stack_size, default 256 KB),
 *                                  sp starts at __stack_top and grows down
 *   0x807F_F000  boot-info page (board) -- not part of the program
 *
 * The program must end below 0x807F_F000; the link fails if the image plus
 * the stack reserve does not fit.  Override the reserve with
 * -Wl,--defsym=__stack_size=<bytes> (common.mk: STACK_SIZE).
 */
OUTPUT_ARCH("riscv")
ENTRY(_start)

__program_base  = 0x80000000;
__program_limit = 0x807FF000;           /* == STACK_TOP == BOOTINFO_ADDR */
__stack_size    = DEFINED(__stack_size) ? __stack_size : 0x40000;

MEMORY
{
    PROG (rwx) : ORIGIN = 0x80000000, LENGTH = 0x7FF000
}

SECTIONS
{
    .text : {
        __text_start = .;
        KEEP(*(.text.init))
        *(.text.unlikely .text.unlikely.*)
        *(.text.startup .text.startup.*)
        *(.text.hot .text.hot.*)
        *(.text .text.*)
        KEEP(*(.text.trap))
        __text_end = .;
    } > PROG

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    } > PROG

    .preinit_array : ALIGN(4) {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > PROG
    .init_array : ALIGN(4) {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > PROG
    .fini_array : ALIGN(4) {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > PROG

    .data : ALIGN(8) {
        __data_start = .;
        *(.data .data.*)
    } > PROG

    /* small data, addressed relative to gp */
    .sdata : ALIGN(8) {
        __global_pointer$ = . + 0x800;
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2)
        *(.srodata .srodata.*)
        *(.sdata .sdata.*)
        __data_end = .;
    } > PROG

    .sbss : ALIGN(16) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.scommon)
    } > PROG
    .bss : ALIGN(16) {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > PROG

    __heap_start  = ALIGN(__bss_end, 16);
    __stack_top   = __program_limit;
    __stack_limit = __stack_top - __stack_size;
    __heap_end    = __stack_limit;
    _end = __heap_start;

    ASSERT(__stack_size >= 4096 && (__stack_size % 16) == 0,
           "__stack_size must be a multiple of 16 and at least 4096")
    ASSERT(__heap_start <= __stack_limit,
           "program image + stack reserve do not fit below 0x807FF000 (ARCHITECTURE.md section 2)")

    /* debug sections keep their default placement */
    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) }
}
