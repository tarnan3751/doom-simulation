# rtl/mem: caches, SDRAM controller, ideal memory

`memsys` is the slave on the CPU instruction port and on the RAM part of the
data port (docs/ARCHITECTURE.md sections 3, 5, 6). Tests: `tests/mem`
(`make -C tests/mem quick | test | synth | penalty | mutate`).

```
                  memsys
  i_req_* --> +---------+ lp (read)                      FAST=0
  i_rsp_* <-- | icache  |-----------+               +------------+  sdram_* pins
              +---------+           |    bk_*       | sdram_ctrl |<===========> chip
                  | tag port A      +--> mem_arb -->|            |
              +---------+           |   (round      +------------+
              | u_tags  | 1 BSRAM   |    robin,       FAST=1
              | ram_tdp |           |    id per     +------------+
              +---------+           |    request)   | mem_ideal  |  32 MB array,
                  | tag port B      |               |            |  verilator public
  d_req_* --> +---------+ lp (r/w)  |               +------------+
  d_rsp_* <-- | dcache  |-----------+
              +---------+
```

## Caches

Address split (offset = `addr[RAM_AW-1:0]`, `RAM_AW = 4 + SDRAM_ROW_BITS +
SDRAM_COL_BITS` = 25 in simulation, 23 on the board; higher bits ignored):

| cache   | lines | index         | word     | tag            | data storage                   |
|---------|-------|---------------|----------|----------------|--------------------------------|
| I 8 KB  | 256   | `off[12:5]`   | `off[4:2]` | `off[24:13]` (12 b) | 2048 x 32 SDP RAM (4 BSRAM)   |
| D 16 KB | 512   | `off[13:5]`   | `off[4:2]` | `off[24:14]` (11 b) | 8 byte lanes x 2048 x 8 SDP RAM = one word pair per address (8 BSRAM) |

* **Tags: one true-dual-port block RAM for both caches** (`u_tags`, 1024 x 13,
  entry `{valid, tag}`): port A = I-cache at `{0, idx}`, port B = D-cache at
  `{1, idx}`, so the halves never collide. 8 KB + 16 KB of data need 12 BSRAMs,
  and the 13-block budget leaves exactly one for tags. Valid bits are cleared by a
  reset sweep (one line per cycle, I and D in parallel, `rsp_ready = 0`
  meanwhile, as R6 allows: 256 / 512 cycles with the default sizes). A miss
  writes its tag at once, in its lookup cycle (no request can be accepted then),
  so lookups during the refill see the new line.
* **D-cache dirty bits in LUT RAM** (128 x 4, read-modify-write, async read), not
  in the tag RAM: a store hit sets its bit at the edge where the next request
  reads the tag port, which the single D tag port could not also write. Set by
  store hits and at the start of a store miss.
* **D-cache data as eight byte-lane RAMs holding a word pair per address**: byte
  stores and refill beats are plain lane write enables (no byte-enable
  emulation), and the pair width lets a dirty victim be copied out in 4 cycles,
  ahead of the refill beats. Cost: a 2:1 word select on the response.
* **Hit responses come straight from the RAM output register.** The data RAMs
  are read only at an accept (address `req_valid ? req_addr : held`), in the
  I-cache also to re-read a word written at its read edge, in the D-cache for the
  victim copy (nothing completes meanwhile), so the output holds for as long as
  the master waits. A request completed by a refill beat answers with the beat
  (`lp_rdata`) in that cycle and from a one-word register afterwards (I `last_d`,
  D `hold_q`): a RAM read at the beat's own edge would collide with its write.
* **Explicit read-during-write bypass (D).** All RAMs are `no_rw_check`, so Yosys
  adds no emulation logic. `byp_mask/byp_data` record, at each accept, the lanes
  of the requested word written at that edge (store hit, refill beat, store
  buffer drain) and those of a store held in the store buffer after it, and
  override those lanes of the RAM output: the FPGA's undefined collision value is
  never used.
* Requests are captured on `req_valid` alone (legal masters assert it only while
  `rsp_ready = 1`, R1), keeping the hit signal off those enables. `rsp_rdata`
  for a store is the word as it was before the store (unspecified by the contract).

### Misses: refill in the background ("fill")

```
icache, dcache:  INIT --sweep--> READY <-> LOOKUP --miss--> WAIT --word in--> READY / LOOKUP
                                                \--hit----> READY / LOOKUP
```

* Lookup (cycle after the accept): `rsp_ready = READY || ((LOOKUP || WAIT) && ok)`.
  A hit completes in the LOOKUP cycle; back-to-back hits accept one request per cycle.
* A miss starts a fill in its lookup cycle (or, in WAIT, as soon as the previous
  fill is over): tag and dirty bit written, line request raised from a flop
  (`lp_req_q`), held until granted. The flop keeps the tag compare off the SDRAM
  command path: presenting the request combinationally saves a cycle per miss
  (-1.4 % Doom cycles) but made 1 of 10 place-and-route runs fail to route in two
  netlists. Beats arrive critical word first (wrap order) and are written as they
  come; `fill_mask` records the words that are in.
* **Early restart, hits under the fill.** The missing request completes in the
  cycle its beat is on `lp_rdata` (merged with the store of a store miss). In the
  Doom timedemo the next D request came 0 or 1 cycles after a miss completed in
  80 % of the cases, mostly a hit, so restarting early only pays because the
  refill does not block hits: a fetch or load of the line being filled completes
  as soon as its word is in (sequential fetches stream behind the beats at one per
  cycle), any other hit as usual. One fill per cache: another miss waits.
* **Store buffer (D, one entry).** The beats own the data RAM's write port for 8
  consecutive cycles, so a store hit during the fill is posted to `sb_*` and
  completes at once; the buffer is written at the first edge after the fill
  (merged into its beat if that word had not arrived) and loads see it through the
  bypass. A second store waits for the buffer; a miss starts only with an empty
  buffer (its victim copy must see the store).
* **Write buffer (D, one line, 4 x 64 bit LUT RAM).** A dirty victim is copied by
  four pair reads in the four cycles after the miss starts, the critical word's
  pair first, so every pair is read before a beat can overwrite it even when the
  first beat comes two cycles after the start (FAST). Nothing completes during the
  copy. The buffer is written back lazily, when the fill is over and no miss is
  starting: a miss with a clean victim for another line goes first (also while
  the write-back is being granted or streamed); one with a dirty victim, or for
  the buffered line itself, waits until the buffer is empty (no forwarding from it).
* Perf strobes: `perf_*_access` is high in the LOOKUP cycle of every accepted
  request (the cycle after the accept), `perf_*_miss` in the following cycle for a
  miss; a request to the line being filled counts as a hit.

## Cache <-> backend line interface (`lp_*` cache side, `bk_*` backend side)

| signal | dir | meaning |
|--------|-----|---------|
| `req` | C->B | a line request is pending; held until `gnt` |
| `we` | C->B | 1 = write back a line, 0 = read (refill) |
| `addr[RAM_AW-6:0]` | C->B | line address = offset >> 5 = `{ba, row, col[COL-1:3]}` |
| `word[2:0]` | C->B | first word of a read; beats wrap within the line |
| `id` | arb->B | requester (0 = I, 1 = D), echoed as `rid` with every beat |
| `gnt` | B->C | request taken at this edge (combinational, from state + `req`) |
| `rvalid, rid, rdata` | B->C | one read beat; 8 beats per read, gaps allowed |
| `wdata` | C->B | current write-back word, valid from the cycle after `gnt` |
| `wnext` | B->C | `wdata` consumed at this edge; the next word is on `wdata` in the next cycle (never in the `gnt` cycle) |

The backend serves one request at a time in order; a new request may be granted
while the previous read's last beats are still in flight (`rid` routes them).
`mem_arb` is round robin on a tie, so neither cache can starve the other.

## SDRAM controller (`sdram_ctrl`)

Closed page: every READ/WRITE uses auto-precharge, one 8-beat burst per line. At
50 MHz tRCD = tRP = 1 cycle, so an open-page hit would save at most 1 cycle and a
row miss would cost 2, and with all banks idle between transactions refresh never
waits for a precharge.

```
S_PWR (T_POWERUP NOPs) -> PRECHARGE ALL -> S_IREF (8 x AUTO REFRESH) -> S_IMRS (MRS 0x023)
  -> S_IDLE --cnt == 0, refresh pending--> REF (wait tRFC) -> S_IDLE
           --cnt == 0, bk_req (gnt)-----> ACT -> S_ACT (tRCD) -> READ+AP  -> S_IDLE (wait W_RD)
                                                            -> WRITE+AP -> S_WBURST (7 beats) -> S_IDLE (wait W_WR)
```

* Cycle counts come from `CLK_HZ` (rounded up from picoseconds, as the chip model
  does): `T_POWERUP = 200 us`, `T_RP = T_RCD = 18 ns`, `T_RAS = 42 ns`,
  `T_RC = T_RFC = 60 ns`, `T_WR = T_MRD = 2`, `CL = 2`, `BL = 8`,
  `T_REFI = floor(CLK_HZ * 64 ms / 4096)` (781 cycles at 50 MHz). Spacing after a
  READ issued at edge e: next ACT/REF at `max(e + BL, ACT + tRAS) + tRP` and
  `>= ACT + tRC`; after a WRITE: `max(last beat + tWR, ACT + tRAS) + tRP` (the
  model applies the same auto-precharge rule). A WRITE starts only once the
  previous read's data has left the bus, plus one idle turnaround cycle.
* Refresh: free-running timer, one pending refresh per `T_REFI`; a pending
  refresh wins over new requests whenever the controller is idle (latency <= one
  transaction), so refresh cannot starve and traffic still progresses. It costs
  0.38 % of SDRAM time.
* Init issues 8 auto refreshes (contract: >= 2). The table row tRC / tRFC is read
  as ACT -> ACT (same bank) and ACT -> REF, resp. REF -> ACT / REF / MRS, both 60 ns.
* All pin outputs are flops except `sdram_dqm`, the constant 0 (a constant has no
  timing); `sdram_dq_i` is captured in `dq_i_q` and only that register is used.
  `cke_q = 0`, `cs_n_q = 1`, `dq_oe_q = 0` have declaration initialisers equal to
  their reset values (the FPGA flop INIT, narrow `PROCASSINIT` waiver): with
  randomised initial state the chip could otherwise see a command at the very
  first edge, before any reset edge (this corrupted chip storage in the bench).

## Latency (`make -C tests/mem penalty`; accept to completion, hit = 1)

| | SDRAM 50 MHz | SDRAM 100 MHz | FAST |
|---|---|---|---|
| I miss, D miss with clean/invalid victim | 7 | 8 | 3 |
| D miss with dirty victim (copied to the write buffer) | 7 | 8 | 5 |
| a miss + the other 7 words of the line, back to back | 15 | 16 | 11 |

These are isolated misses with an idle backend. A miss also waits for the previous
refill of its cache and (D, dirty victim) for an empty write buffer; a write-back
occupies the SDRAM for about 13 cycles. With FAST=0 the first miss after reset
waits for SDRAM init: 200 us + 8 refreshes + MRS, about 10 030 cycles at 50 MHz.

## `mem_ideal` (FAST=1) and preloading

`logic [31:0] mem [0:2**23-1] /* verilator public */`, word i = RAM bytes
4i..4i+3 (little endian), i.e. byte offset N = `((uint8_t*)mem)[N]`, the same
indexing as the SDRAM model's `bytes()`; only the first `2**RAM_AW` bytes are
used. Line read: beats in cycles t+1..t+8 after the grant cycle t; line write:
words taken in cycles t+1..t+8. Hierarchical name `<memsys>.g_ideal.u_ideal.mem`
(`soc_top.u_mem.g_ideal.u_ideal.mem` in the SoC). Verilator's class names carry
parameter hashes, so find it through the scope table (independent of instance
names; `find_mem_ideal()` in `tests/mem/tb_memsys.cpp`):

```cpp
#include <verilated_syms.h>
for (const auto& kv : *contextp->scopeNameMap()) {
    const std::string n = kv.first;                        // "...g_ideal.u_ideal"
    if (n.size() >= 15 && n.compare(n.size() - 15, 15, "g_ideal.u_ideal") == 0)
        if (const VerilatedVar* v = kv.second->varFind("mem"))
            ram = static_cast<uint32_t*>(v->datap());     // 2**23 words
}
```

With FAST=0 preload through `SdramModel::bytes()` at byte offset
`address - 0x8000_0000`, and call `SdramModel::tick()` every cycle from the first
one (also while `rst = 1`), with the same `CLK_HZ` as the RTL
(`SdramTiming::from_ns(CLK_HZ)`), as in ARCHITECTURE.md section 6;
`violations()` must stay 0.

## Parameters and contract readings

* `LINE_BYTES` must be 32 (`$error` otherwise); `ICACHE_BYTES` >= 64 and
  `DCACHE_BYTES` >= 128, powers of two; `SDRAM_ROW_BITS` >= 11,
  4 <= `SDRAM_COL_BITS` <= 10. CL = 2 is fixed: fine up to about 100 MHz.
* RAM offset: section 5 says `addr[24:0]`; memsys uses `RAM_AW` bits, identical
  (25) for the simulation geometry and 23 on the 8 MB board part, where a literal
  `addr[24:0]` would make the caches treat aliases as different lines while the
  SDRAM merges them.
* Section 5 lists data, tags and valid bits; the dirty bits are the LUT RAM above.
* A reset in the middle of a run re-executes the full SDRAM init (200 us without
  refresh), which the chip model reports as refresh-rate violations: reset only
  at power-up. Both ports are independent (R8): D-cache dirty data is not visible
  to fetches until it is written back.

## Verification (`tests/mem`)

* `tb_memsys.cpp`: two randomised port drivers (phases of access patterns, gap
  profiles, CPU-like lockstep issue), a reference memory and shadow caches that
  predict hit/miss and the exact completion cycle of every hit, also during a
  refill and while the store buffer is full; per-cycle contract checks (R1-R5,
  held responses, perf strobes), backdoor preload, a final flush and a compare of
  the whole backing store; pin-fault injection (`--inject`, each fault must be
  reported by the chip model) and the latency measurement (`--penalty`).
* `sdram_model_test.cpp`: directed test of the chip model (`sim/models`): legal
  traffic (0 violations, data, wrap/interleave order, DQM, CL 2 and 3, BL 8 and
  4) and 26 single-violation cases.
* Six configurations (Makefile): sdram_def, sdram_tiny (I 256 B / D 128 B),
  sdram_row11 (8 MB), sdram_100m (CLK_HZ 100 MHz), ideal_def, ideal_tiny
  (I 128 B / D 256 B). `test` runs them with 16-48 seeds of 300 000-500 000
  operations, four pin faults, both latency runs and `synth` (Yosys `synth_gowin`
  of the board configuration: at most 13 block RAMs, every memory mapped).
* Mutation testing (`mutants.txt`, driver `../core/mutate.py`): 36 planted bugs
  must each be caught in every configuration (s = sdram_def, f = ideal_tiny)
  that instantiates the mutated logic, and a no-op control must not be.

## Known limitations

* One refill per cache at a time and a one-line write buffer.
* Direct mapped: the tag block RAM has no room for a second way and there is no
  spare block RAM. On the Doom timedemo stream an 8-line victim buffer (2 kbit LUT
  RAM, 8 tag comparators) would cut D misses from 54.8 M to 42.3 M, roughly 1.5 %
  of the cycles (2-way: 38.9 M).
* One SDRAM transaction at a time (no bank interleaving): with `ba` in the top
  address bits, the program lives in one bank anyway.

## Files

| file | contents |
|------|----------|
| `memsys.sv` | top: caches, shared tag RAM, arbiter, backend selection (FAST) |
| `icache.sv` | read-only direct-mapped I-cache, background refill with early restart and streaming |
| `dcache.sv` | write-back / write-allocate D-cache: background refill, write buffer, store buffer, dirty LUT RAM, bypass |
| `mem_arb.sv` | round-robin line arbiter with request ids |
| `sdram_ctrl.sv` | SDRAM controller (init, refresh, line bursts, timing from CLK_HZ) |
| `mem_ideal.sv` | 32 MB ideal line memory for the FAST build (simulation only) |
| `ram_sdp.sv`, `ram_tdp.sv` | simple / true dual-port synchronous RAM (block RAM; the latter holds the tags) |
| `tests/mem/run.sh` | the step runner of `quick` / `test` |
