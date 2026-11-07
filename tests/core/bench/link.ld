/* Link script for tests/core benchmarks: code and data in RAM at the reset PC. */
OUTPUT_ARCH("riscv")
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text : { *(.text.init) *(.text .text.*) }
  . = ALIGN(16);
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  . = ALIGN(0x1000);
  .data : { *(.data .data.* .sdata .sdata.*) }
  __bss_start = .;
  .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
  . = ALIGN(4);
  _end = .;
  . = ALIGN(0x1000);
  .tohost : { *(.tohost) }
}
