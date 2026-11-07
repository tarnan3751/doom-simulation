# sim/model - cycle-exact model of the accurate machine

`model::Machine` (`model.h`, `model.cpp`) runs a program on the ISS (`sim/iss`) and adds the timing of `soc_top`
with FAST=0, UART_DIV=434 and the SDRAM controller at 50 MHz, from the first cycle after reset: every instruction
retires in the same clock cycle as on the RTL, every `cnt_*` counter has the RTL's value in every cycle, and
`rdcycle`/`rdinstret`, MMIO reads, VID_FLIP and the serial line behave as on the RTL. It is the `model` engine of
the virtual board (`sim/vboard/engine_model.cpp`). The RTL stays the reference: `--engine accurate --cosim=model`
checks the model against it. Built and tested with the board (`make -C sim/vboard`, `test`, `test-long`).

## What is modelled

| RTL (rtl/) | in the model |
|---|---|
| `rv32_core` | valid flag and instruction of IF, ID, EX, MEM, WB; one global stall (`advance` = both ports ready and no mul/div busy); load-use bubble; redirect on mispredict, trap, `mret`; the instruction in EX executes in the ISS when it leaves EX, wrong-path instructions exist only as fetch addresses |
| `rv32_decode` | what the timing needs: registers read, rd, load/store, branch/jal/jalr/call/return, mul/div, mret, CSR write; legality |
| `rv32_bpred` | BTB (entry read at the fetch and held), BHT (read in IF, sees updates made meanwhile), RAS with speculative pushes and pops by wrong-path instructions and the committed pointer |
| `rv32_muldiv`, `rv32_csr` | cycles in EX from the operands (mul 3, div n + 3, by zero 2); mcycle/minstret, a write replacing that cycle's increment |
| `icache`, `dcache` | tags, valid and dirty bits, held request, background fill (critical word first, early restart, hit under fill), victim copy, write buffer and its write-back order, store buffer; no data: memory is the ISS's |
| `mem_arb`, `sdram_ctrl` | round robin; power-up, precharge, 8 refreshes, mode register; refresh timer and pending count; ACT/READ/WRITE spacing, beat pipe, bus turnaround |
| `mmio` | UART busy and when the receiver on the pin has the byte; keyboard FIFO (16 entries, overflow); VID_STATUS from the scan-out counters; flip counter; SYS registers; write-only and unmapped registers read 0 |
| `soc_top` | the counters: an event decided in cycle c shows from cycle c + 2 |

Not modelled because nothing observable depends on it: cache and SDRAM data, the SDRAM chip, the scan-out pixels
(the window shows the frame at VID_FLIP, as with the fast engine). Cycle 0 is the first cycle after reset; the
caches' reset sweep makes cycle 512 the first in which the core can move.

Speed comes from two modes. While no miss, fill, write-back or store-buffer entry is in flight every request is a
hit that completes in the next cycle, so a cycle is one pipeline step, the SDRAM controller is left behind and
caught up in closed form when the next miss needs it, and mul/div waits are skipped. Otherwise (about 30% of
Doom's cycles) the caches, the arbiter and the controller are stepped register by register.

## How exactness is proven

`vboard --engine accurate --cosim=model ...` runs the RTL and the model side by side. For every instruction the RTL
retires it runs the model to that cycle and requires the model to retire the same instruction (all trace fields)
in the same cycle with all eleven counters equal; frames are compared with their cycle, serial bytes with the
cycle the receiver decodes them, and at the end of every engine slice counters, halt and exit code. The model gets
no values from the RTL (no MMIO or counter overrides); key events enter both at the same clock edge. The first
deviation stops the run (status 119) with the differing fields and the last 32 retired instructions;
`--cosim-inject N` shifts the expected cycle of instruction N to test that report.

`make -C sim/vboard test` runs it on every riscv-test, hello, testpattern, CoreMark-quick, `tests/corners.c` (counter
writes, UART write while busy, keyboard overflow, VID_STATUS waits, write-only registers, every divide length) with
a key script, the keyboard test and 60 Doom frames; `test-long` on 400 Doom frames and full CoreMark. The full
timedemos are checked with `vboard --engine accurate --cosim=model --headless --wad WAD doom.elf -- -timedemo demoN`.

## When the RTL's timing changes

Any change to a cycle of the core, the predictor, the caches, the arbiter, the controller or the MMIO devices must be
made here as well (the table above says where), then the lockstep runs above must pass again. Fixed here as
constants, because the RTL build never varies them: I-cache 8 KB and D-cache 16 KB direct mapped with 32-byte lines,
32 MB RAM, BTB 256 / BHT 1024 / RAS 8, the SDRAM cycle counts at 50 MHz (`sd_edge`), UART_DIV 434, 800x525 video
timing at clk/2. FAST=1 timing is not modelled.

## Limits

Where the ISS stops (fetch outside RAM, an access outside RAM and the mapped MMIO blocks) the RTL would alias or read
0; the model reports a fault there, as the iss engine does. Self-modifying code is not supported by the platform
(ARCHITECTURE.md R8); the model would fetch the new code where the RTL's I-cache may still hold the old.
