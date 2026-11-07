ENTRY(_start)
SECTIONS
{
    . = 0x80000000;
    .text   : { *(.text.init) *(.text .text.*) }
    .rodata : { . = ALIGN(4); *(.rodata .rodata.*) }
    .data   : { . = ALIGN(4); *(.data .data.* .sdata .sdata.*) }
    .bss    : { . = ALIGN(4); *(.bss .bss.* .sbss .sbss.* COMMON) }
    . = ALIGN(4);
    _end = .;
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame*) }
}
