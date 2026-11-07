# rtl/core: `rv32_core`

RV32IM + Zicsr + Zicntr, machine mode only, classic 5-stage in-order pipeline.
Interface and architectural contract: `docs/ARCHITECTURE.md` sections 3 and 4.
Tests: `tests/core` (`make -C tests/core quick | test | bench | lint | mutate`).

## Pipeline

```
          imem_req (npc)                          dmem_req (agu)
               |                                        |
   +-----------v--+    +--------+    +---------------+--v-----+    +-----------+    +----+
   | IF           |    | ID     |    | EX            |        |    | MEM       |    | WB |
   | f_pc, BTB/BHT|--->| decode |--->| fwd mux, ALU, |        |--->| load data |--->|    |--> regfile write
   | RAS predict  | IF | regfile| ID | AGU, branch,  |        | EX | align     | MEM|    |    (write-through)
   | imem_rsp     | ID | read   | EX | CSR, trap,    |        | MEM| dmem_rsp  | WB |    |--> trace register
   +--------------+    | fwd sel|    | mul/div       |        |    +-----------+    +----+    (trace_* outputs)
         ^             +--------+    +-------+-------+--------+
         |                                   |
         +---------- redirect (ex_npc) ------+   mispredict / exception / mret
```

* **IF** - `f_pc` is the instruction whose fetch response is (or will be) on
  `imem_rsp_rdata`. The request for the *next* instruction is presented while
  this one is in IF: `npc` = EX redirect target, else predicted target, else
  `f_pc + 4` (the very first fetch after reset uses `RESET_PC`).
* **ID** - decode (strict: every unimplemented encoding is illegal), register
  read with write-through, forwarding-source selection for EX, `pc + imm`
  (branch/jal target, auipc), the branch-target-correct bit, two jalr
  comparison values, load-use detection.
* **EX** - forwarding muxes, ALU, address adder (load/store address and jalr
  target), branch condition, mispredict and exception detection, CSR access,
  trap entry, `mret`, mul/div unit, D-port request. **EX is the commit point**:
  every exception is detected by the end of EX, so an instruction leaving EX can
  never be cancelled, and minstret, CSR writes and trap entry happen there.
* **MEM** - consumes the D-port response (byte/half select, sign extension).
* **WB** - register file write; the retire trace is registered from WB (it
  appears on `trace_*` in the cycle after the register write).

## Stall and flush rules

```
advance  = !rst && imem_rsp_ready && dmem_rsp_ready && !ex_busy      // whole pipeline steps
ex_busy  = e_valid && e_is_muldiv && !md_done
redirect = e_valid && (exception || mret || mispredict)
mispredict = (taken != pred_taken) || (taken && !target_matches_prediction)
load_use = d_valid && e_valid && e_is_load && e_rd != 0 &&
           ((uses_rs1 && rs1 == e_rd) || (uses_rs2 && rs2 == e_rd))
fetch_go = advance && !(load_use && !redirect)                        // = imem_req_valid

at an edge with advance:
  IF/ID  : if fetch_go:  d_valid <= f_valid && !redirect   (else hold)
  ID/EX  : e_valid <= d_valid && !redirect && !load_use      (bubble on load-use or redirect)
  EX/MEM : m_valid <= e_valid
  MEM/WB : w_valid <= m_valid
  retire : regfile write and trace_valid <= w_valid
without advance: every stage holds, no request is issued, trace_valid <= 0.
dmem_req_valid = advance && e_valid && (load || store) && !misaligned
```

* One global stall: nothing moves unless both ports are ready and EX is not
  busy, so there is never more than one request outstanding per port and
  nothing is ever cancelled. Requests are only issued at advancing edges.
* Load-use: one bubble. IF and ID hold (no new fetch, the I-port response is
  reused per rule R3), EX gets a bubble, the load moves to MEM.
* Redirect: the instructions in IF and ID are dropped and the correct-path
  fetch is issued in the same cycle: two bubbles. A redirect wins over a
  load-use stall in the same cycle.
* Only correct-path instructions reach EX and nothing in EX is cancelled, so a
  store or MMIO access is issued exactly once, in program order, and never for a
  flushed or trapping instruction. Fetches are speculative and may go to any
  address the predictor or a wrong-path jump produces.

## Forwarding

| into EX operand | from | condition (decided in ID, registered) |
|---|---|---|
| rs1 / rs2 | MEM stage result (`m_result`) | instruction now in EX writes the register |
| rs1 / rs2 | WB stage result (`w_result`, includes load data) | instruction now in MEM writes it |
| rs1 / rs2 | register file value read in ID | otherwise (write-through covers WB in the ID cycle) |

The MEM-stage result is never a load (load-use stall), so no path runs from
`dmem_rsp_rdata` into EX. All consumers use the forwarded values: ALU, address
adder, store data, branch compare, jalr target, CSR source, mul/div operands.

## Branch prediction (`rv32_bpred`)

* BTB: `BTB_ENTRIES` x 36 bits in one block RAM: valid, kind (branch / jump /
  call / return), 12-bit partial tag, target[22:2]. target[31:23] come from the
  fetch PC: all code lives in the 8 MB program region, and a jump into another
  8 MB region is always mispredicted (2 cycles), never wrong. Read synchronously
  with the fetch request address at fetch issue; the entry is held across stalls.
* BHT: `BHT_ENTRIES` 2-bit counters in distributed (LUT) RAM, read
  asynchronously with `f_pc` while the instruction is in IF: on the FPGA all
  block RAMs are used elsewhere, and a LUT-RAM read addressed by `npc` would
  lengthen the critical next-fetch path. So a counter used for a prediction
  includes an update made by EX while the instruction waits in IF.
* Both memories carry `no_rw_check`: on the FPGA a lookup in the same cycle as an
  update of the same entry may read either value. Only predictions are affected.
* Prediction for the instruction in IF: taken if BTB hit and (kind != branch or
  counter[1]); target = RAS top for returns, else the BTB target. A correctly
  predicted taken branch or jump costs 0 bubbles.
* RAS: `RAS_DEPTH` entries, pushed/popped in IF on predicted calls/returns
  (call = jal/jalr with rd in {x1,x5}; return = jalr x0 with rs1 in {x1,x5});
  a committed pointer is kept from EX and restores the speculative pointer on
  every redirect.
* Update from EX (resolved): a taken branch/jump writes its BTB entry, a
  non-branch that was predicted taken invalidates it, branches update their counter.
* Predictor state can never affect results: every predicted next PC is checked
  in EX. The jalr check avoids the address adder: EX compares `rs1[31:2]` with
  `pred - imm[31:2] - carry(rs1[1:0]+imm[1:0])`, both candidates precomputed in ID.

## CSRs, counters and traps (`rv32_csr`)

Read and written by the instruction in EX at the edge where it leaves EX. Trap
entry: `mepc = pc`, `mcause`, `mtval`, `MPIE = MIE`, `MIE = 0`, redirect to
`mtvec`. `mret`: `MIE = MPIE`, `MPIE = 1`, redirect to `mepc`. Readings of points
the contract leaves open (the ISS makes the same choices; the lockstep test
holds both to them):

* **Counters.** A CSR instruction reads `mcycle` = the cycle number since reset
  in which it leaves EX, and `minstret` = the number of non-trapping
  instructions before it (= the trace count; trapping instructions, ecall and
  ebreak included, do not count). minstret is incremented when an instruction
  leaves EX (it can no longer be cancelled), up to 3 instructions ahead of the
  trace, which software cannot observe. A write to `mcycle[h]`/`minstret[h]`
  replaces that cycle's / instruction's increment; writing one half leaves the
  other unchanged (no carry).
* **CSR set.** Every CSR not listed in section 4 is illegal, including
  `time[h]` (software must use `rdcycle`), `mcounteren`, `mcountinhibit`,
  `mstatush`, PMP and S/U-mode CSRs. Only the read-only addresses
  (`csr[11:10] == 3`: cycle[h], instret[h], mhartid, mvendorid, marchid, mimpid)
  trap on a write; `misa` reads `0x40001100` and ignores writes (WARL;
  `rv32mi-p-ma_fetch` writes it). `csrrs/csrrc` with rs1 = x0 and
  `csrrsi/csrrci` with uimm = 0 do not write; `csrrw/csrrwi` always write.
* **Values.** `mstatus` reads `MPIE<<7 | MIE<<3 | 3<<11`, other bits 0; `mie`
  stores 32 bits; `mip` reads 0, writes ignored; `mtvec[1:0]` and `mepc[1:0]`
  read 0; `mcause` is fully writable. Everything resets to 0.
* **mtval** (as Spike): illegal instruction = the instruction word, ebreak = its
  pc, ecall = 0, misaligned target = the target, misaligned load/store = the address.
* **Decode is strict.** 16-bit encodings, reserved opcodes and wrong
  funct3/funct7 trap (slli/srli/srai with shamt[5], OP funct7 other than 0, 0x20
  for add/sub/srl/sra, 1; jalr funct3 != 0; fence funct3 > 1; SYSTEM funct3 = 4);
  SYSTEM funct3 = 0 is legal only as exactly ecall, ebreak, mret or wfi (so sret,
  sfence.vma, ecall with rd != 0 trap). `fence`/`fence.i` with other field
  values and `wfi` are no-ops.
* Only a *taken* branch to a misaligned target traps; `jalr` clears bit 0 and
  traps if bit 1 is set, without writing rd.
* **Trace.** A trapping instruction shows `rd = 0` and all memory fields 0,
  `next_pc = mtvec`. Loads show their address with `wstrb = wdata = 0`. Store
  data, on the D-port and in the trace, is 0 in the disabled lanes.

## Mul / div (`rv32_muldiv`)

| op | cycles in EX | stall cycles |
|---|---|---|
| mul, mulh, mulhsu, mulhu | 3 (operand register, 33x33 DSP product register, select) | 2 |
| div, divu, rem, remu | n + 3 (magnitudes, normalisation, n radix-2 iterations, sign fix) | n + 2 |
| divide by zero | 2 | 1 |

n = clz(\|divisor\|) - clz(\|dividend\|) + 1, the number of quotient bits that can be
non-zero (1..32); the normalisation cycle starts the radix-2 loop at that bit
({remainder, quotient} = \|dividend\| << (32 - n)). n < 1 (\|dividend\| < \|divisor\|):
quotient 0, no iteration, 2 stall cycles. Doom's divides (mostly the 16-bit steps of
libgcc's 64-by-32 division in FixedDiv) average n = 12.5. The multiplier sits
between fabric registers on one DSP block.

## Measured (zero-latency memory, `make -C tests/core bench`)

| | cycles |
|---|---|
| base CPI, independent or back-to-back dependent ALU ops (forwarding is free) | 1.00 |
| load without / with an immediate use | 0 / 1 |
| branch/jump mispredict (LFSR-random branch: 2.0 extra cycles per miss) | 2 |
| correctly predicted taken branch, call, return | 0 |
| mul / div penalty | 2 / n + 2 (2..34) |

## Integration notes

* `imem_req_valid` and `dmem_req_valid` depend combinationally on both
  `rsp_ready` inputs (one global stall), so every slave must obey R5 (no
  combinational path from `req_*` to `rsp_*`); all other outputs are functions of
  core state. `imem_req_addr` is always 4-byte aligned. After reset the core
  waits for both ports to be ready before the first fetch.
* `trace_*` and `perf_*` are registered. `perf_branch` / `perf_jump` pulse for
  every conditional branch / jal / jalr leaving EX, `perf_*_miss` when its next
  PC was mispredicted and it did not trap. A non-branch wrongly predicted taken
  (BTB alias) also costs a 2-cycle redirect but is counted in neither.
  `cnt_instret` (counting `trace_valid && !trace_trap`) equals `minstret` at the
  same program point.
* Not reset, so that they map to RAM: register file (LUT RAM; 0 at FPGA power-up
  and with Verilator's default `randReset(0)`), BTB, BHT. Garbage predictor
  contents only cost mispredictions. A program that reads a register before
  writing it diverges from the ISS (which starts at 0) under random initialisation.
* The `ifndef SYNTHESIS` block at the end of `rv32_core.sv` computes passive
  coverage points for tests/core (`cov_events`, read by nothing in the design);
  yosys-slang does not predefine `SYNTHESIS`, so pass `-D SYNTHESIS` (harmless if
  forgotten: the logic has no loads). `soc_top` sets `COVERAGE = 0` so the board
  simulation does not compute them. Resources and timing: `docs/results/fpga.md`.

## Verification (`tests/core`)

* `rv32_core` alone against a C++ memory implementing section 3 on both ports
  with seeded random latency (modes `fast`, `randi`, `randd`, `both`, `burst` =
  phases of 10-130-cycle latencies), a random `rsp_ready = 0` period after reset,
  and garbage `rdata` while a response is not ready.
* Contract checks: no request during reset; `req_valid` only while that port's
  `rsp_ready = 1`; aligned fetches; legal store strobes for the address, zero data
  in disabled lanes, `wstrb = 0` on loads; every D-port request matched one-to-one,
  in order, with a retired load/store (duplicate, flushed, missing or wrong
  requests); trace rules (pc continuity, `rd = 0` => `wdata = 0`, traps without
  effects, instruction word = memory); perf strobe consistency; hang watchdog.
* Lockstep: every retired instruction (pc, insn, trap, rd, rd value, memory
  address, strobes, data, next pc) against the golden ISS (`sim/iss`). The ISS
  has no clock, so the bench supplies the counter reads by the rules above:
  `mcycle` from each instruction's EX-commit cycle (passive signal
  `cov_events[47]`), `minstret` from its own count; both are thus checked exactly.
* Programs: the 56 riscv-tests (`tests/isa`), the benchmarks (`bench/`), and
  constrained-random programs (`tb/rand_prog.cpp`; invariants in `rand_prog.h`)
  that must hit all 44 functional coverage points (forwarding per consumer,
  load-use, write-through, every redirect and trap kind, stall combinations,
  divide corner cases and 8-24 quotient bits, counter writes, BTB aliasing through
  a code section 4 MB above the main code).
* Half of all runs randomise all un-reset state (register file, BTB/BHT/RAS,
  pipeline data registers). A second build with a tiny predictor
  (`BTB_ENTRIES=16, BHT_ENTRIES=32, RAS_DEPTH=3`) runs riscv-tests and random
  programs too.
* Mutation testing (`mutants.txt`, driver `mutate.py`, shared with tests/mem): 33
  planted RTL bugs (3 in the divider's normalisation) must be caught, 3
  predictor-only corruptions must not change any result.
* Reproduce a failing run: `build/core/obj/tb_core --random SEED --mode M --seed S
  [--rand-reset 1] [--items N] [--vcd FILE]` (or `--elf FILE`), as printed.

## Known limitations

* No interrupts (by contract); `wfi` is a no-op.
* A mul/mulh pair with the same operands (FixedMul) could reuse the 64-bit
  product register and save 1 cycle (0.1 % of Doom's cycles): not done.
* RAS entries are not repaired after wrong-path pushes (only the pointer is);
  the jalr target is the BTB's last target (no indirect history).
* Reset in the middle of a run and `RESET_PC` other than `0x8000_0000` are not
  simulated (all control state is synchronously reset).

## Files

| file | contents |
|---|---|
| `rv32_pkg.sv` | opcodes, enums, CSR addresses, cause codes, decoded-instruction struct |
| `rv32_core.sv` | top level: pipeline registers, hazards, forwarding, branch resolution, exceptions, D-port, trace |
| `rv32_decode.sv` | combinational decoder (ID) |
| `rv32_alu.sv` | ALU |
| `rv32_regfile.sv` | 31 x 32 register file, 2 async read ports, write-through, not reset (LUT RAM) |
| `rv32_muldiv.sv` | multi-cycle multiplier (DSP) and radix-2 divider with leading-zero skip |
| `rv32_csr.sv` | machine-mode CSRs, counters, trap entry / mret |
| `rv32_bpred.sv` | BTB, BHT, return-address stack |
| `tests/core/tb/tb_main.cpp` | the testbench: port models, contract checks, ISS lockstep, suites |
| `tests/core/tb/rand_prog.{h,cpp}` | random program generator |
| `tests/core/bench/` | microbenchmarks and a C kernel with their start-up code |
