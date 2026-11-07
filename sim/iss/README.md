# ISS - golden instruction-set model

Instruction-level model of the whole SoC (ARCHITECTURE.md sections 2, 4, 7, 9, 11): RV32IM + Zicsr + Zicntr, machine mode,
32 MB RAM at 0x80000000, UART/system/keyboard/video MMIO. Exact architectural behaviour, one cycle per instruction (`mcycle`
counts executed instructions, `minstret` retired ones), precise traps, a retire record equal in content to the RTL retire
trace. No caches, pipeline, SDRAM or video timing, no interrupts, no compressed instructions, no self-modifying-code semantics
(fetch always sees current RAM), no JIT or predecode cache (not needed at several hundred MIPS).

Build / test: `make -C sim/iss` (-> `build/iss/iss-run`), `make -C sim/iss test` (riscv-tests, API unit tests, iss-run CLI
tests, a throughput benchmark), `make -C sim/iss bench`. Library: `iss.cpp` plus `sim/common/loader.cpp`; include paths
`sim/iss`, `sim/common`, `sw/include` (the ISS takes its addresses from `sw/include/soc.h`; sim/common does not depend on it).

## API (`namespace iss`, iss.h)

```cpp
iss::Iss m;                                   // or iss::Iss m(iss::Config{clk_hz, sys_flags, illegal_mtval_is_insn})
m.write_mem(addr, data, len);                 // backdoor into RAM (also m.ram(), m.read_mem)
m.reset(entry);                               // regs/CSRs/counters/devices cleared, RAM and the store watch kept
Retire r = m.step(const uint32_t* override = nullptr);   // exactly one instruction
uint64_t n = m.run(max);                      // fast path, no record; returns instructions executed
m.push_key(pressed, code);  m.on_uart = ...; m.on_flip = ...;
m.stop_reason()/stop_message()/exit_code()/set_store_watch(addr)/watch_value()/clear_stop()
m.pc() reg(i) set_reg csr_peek(csr, v) cycles() instret() flip_count() framebuffer() palette() kbd_pending()
```

`Retire`: `valid trap pc insn rd rd_wdata mem_addr mem_wstrb mem_wdata next_pc` (the RTL trace fields) plus `cause is_load
override_used taken csr_wdata` (`taken`: a jal/jalr or a branch whose condition held; `csr_wdata`: the value a Zicsr
instruction wrote to its CSR). `m.mmio_read = fn(word address) -> word`, if set, supplies MMIO load values instead of the
ISS's devices (sim/model uses it for device timing; the ISS's devices then see no reads). Traps: `rd=0`, no mem fields (`mem_addr = 0`; the RTL may show the faulting address there), `next_pc` = the
handler. Store data sits in its byte lanes with unstrobed lanes 0: compare strobed lanes only. A stopped machine (exit,
fault, watch) returns `valid=false`; a bad fetch/access does not execute the instruction, the state stays at the faulting pc
and `stop_message()` says why, with the pc.

## Override rules (lockstep)

`step(&value)`, where `value` is what the hardware wrote to `rd` (`trace_rd_wdata`, after sign/zero extension), is used only by
1. **loads with address bit 31 clear (MMIO)** that do not trap: the result is `value`. For `KBD_DATA` the model's FIFO stays
   consistent: it pops one event iff `value != 0`. Other MMIO registers have no read side effects; the address must still lie
   in a mapped block (else the usual unmapped-access stop);
2. **Zicsr instructions that read a counter CSR** (cycle[h], instret[h], mcycle[h], minstret[h]) - every form except
   `csrrw/csrrwi` with `rd=x0`, which does not read. `value` is the old CSR value (so `csrrs/csrrc` use it for the write-back).

Every other instruction ignores it (`Retire.override_used` tells). These are the only intended divergences; key events must
be pushed into the model before the load that consumes them, in the order the RTL received them.

## Readings of ambiguous or silent points in ARCHITECTURE.md

1. **mtval**: illegal instruction = the instruction bits, ebreak = its pc (as the RTL core and Spike); `Config::
   illegal_mtval_is_insn = false` gives 0 for both. ecall: 0. Misaligned load/store/jump: the address / misaligned target
   (bit 0 cleared for jalr).
2. **mcycle / minstret**: mcycle counts every executed instruction including trapping ones, minstret retired ones only (as
   `cnt_instret`). A counter CSR read returns the number of instructions executed before it. Writes to mcycle[h]/minstret[h]
   work.
3. **CSRs**: writing `misa` is ignored (no trap); the read-only range 0xC00-0xFFF (cycle/instret shadows, mhartid, mvendorid,
   marchid, mimpid) traps on any write (csrrw, csrrs/csrrc with rs1 != x0, csrrsi/csrrci with uimm != 0). `mip` reads 0, writes
   ignored; `mie` is a plain 32-bit register; `mtvec`/`mepc` bits [1:0] read 0; `time`, hpm counters, `mcountinhibit` and
   everything else trap as illegal.
4. **MMIO sub-word accesses**: loads return the addressed bytes of the register word (sign/zero extended), stores apply byte
   lanes. `UART_TX` sends `wdata[7:0]` only if lane 0 is strobed; `SYS_EXIT` latches the full lane-positioned word; `VID_FLIP`
   fires on any store to it; palette entries keep 24 bits; `KBD_DATA` pops on any load of that word.
5. **Write-only / unmapped registers** inside a mapped block read 0. Framebuffer and palette read back what was written (the
   spec says write-only: software must not depend on it); framebuffer stores at 64000..0xFFFF are ignored.
6. **Stop conditions** (diagnostics the hardware lacks): fetch outside RAM; load/store outside RAM and the five mapped MMIO
   blocks (0x10000000, 0x10001000, 0x10002000, 0x10003000 4 KB each, 0x10010000 64 KB); RAM accesses at or above 0x82000000
   (the RTL would alias them). Misalignment is checked first, so a misaligned access to an unmapped address traps.
7. **VID_STATUS**: bit0 always 1; the frame counter in [31:16] is `(mcycle / 840000) & 0xFFFF` (800*525 pixels at clk/2) so
   vsync-wait loops progress. `SYS_FLAGS` = 1 by default (the model is the FAST machine).
8. **fence/fence.i/wfi** are no-ops; `fence` with funct3 > 1, `sret`, `sfence.vma`, 16-bit encodings and shift-immediates with
   shamt[5] set are illegal. The register file starts at zero; `sp` is not set by the model. Interrupts are never raised
   (mstatus.MIE/MPIE are only stored and updated by traps/mret); misaligned fetch targets are checked only on taken transfers.

## iss-run

```
iss-run [options] program.elf [-- arg ...]
  --wad FILE  --max-insns N  --dump-frames FILE  --trace FILE  --isa-test  --stats
```
The dependency-free runner (no Verilator, no SDL): `make -C sw run-iss` and the CLI tests use it; `--trace` writes a text
retire trace. It starts at the ELF entry and writes the boot-info page (`wad_addr=0` without a WAD) except in `--isa-test`,
which watches the aligned `tohost` word. Exit status: low byte of SYS_EXIT; `--isa-test` 0 pass / 1 fail; 124 max-insns; 125
bad fetch or access (one line on stderr with the pc); 126 setup error (file, ELF, WAD); 2 usage. `build_bootinfo` rejects a
WAD name over 63 bytes and a command line over 959 bytes rather than truncating.

## Files

| File | Purpose |
|---|---|
| `iss.h`, `iss.cpp` | the machine model |
| `iss_run.cpp` | command-line runner |
| `Makefile`, `tests/tests.mk` | build, `test`, `bench` |
| `tests/test_api.cpp` | host unit tests: loader (ELF, boot info, WAD), DFRM, step/override API, keyboard, video, step == run, bench vs the host compiler |
| `tests/test_cli.sh` | iss-run tests: 56 riscv-tests, exit codes, faults, trace, frame dump, boot-info |
| `tests/{exit42,mmio,video,kbd,bootinfo,fault,traps,bench}.c`, `crt.S`, `link.ld`, `t.h`, `bench_kernel.h` | directed target programs |
| `../common/loader.{h,cpp}`, `../common/dfrm.h` | shared host code: ELF / WAD / boot-info loading, the DFRM writer and reader (also used by vboard and tools/) |
