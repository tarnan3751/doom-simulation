# Architecture contract

This document is the single source of truth for every interface that crosses a
module, language or team boundary. If code and this document disagree, one of
them is a bug: fix it and say so.

Contents: 1 System · 2 Memory map · 3 Core↔memory port contract · 4 `rv32_core`
· 5 `memsys` · 6 SDRAM pins and chip model · 7 Peripherals · 8 `soc_top` ·
9 Boot conventions · 10 Software stack · 11 Golden model (ISS) · 12 Coding rules ·
13 Frame dump format

---------------------------------------------------------------------------

## 1. System

```
              soc_top  (synthesizable; the "FPGA")
 +--------------------------------------------------------------------+
 |  rv32_core            memsys                                       |
 |  +---------+  I-port  +--------+ line                              |      virtual board (C++)
 |  | IF ID   |--------->| icache |-----+    +------------+  SDRAM    |    +------------------+
 |  | EX MEM  |          +--------+     +--->| sdram_ctrl |<==pins===>|<==>| SDRAM chip model |
 |  | WB      |  D-port  +--------+     |    |  (FAST=0)  |           |    +------------------+
 |  | bpred   |--+------>| dcache |-----+    +------------+           |
 |  | csr     |  |       +--------+     |    | mem_ideal  |           |
 |  +---------+  |                      +--->|  (FAST=1)  |           |
 |               |                           +------------+           |
 |               |   mmio                                             |
 |               +-->+------+ uart_tx ------------------------------->| serial decoder -> terminal
 |                   | uart |                                         |
 |                   | sys  | halted/exit_code ---------------------->| run control
 |                   | kbd  |<--------------------- key_valid/key_data| SDL keyboard
 |                   | video| vid_* pixel stream -------------------->| SDL window ("monitor")
 |                   +------+                                         |
 +--------------------------------------------------------------------+
```

* CPU: RV32IM + Zicsr + Zicntr, machine mode only, 5-stage pipeline, no OS.
* One clock `clk` (nominally **50 MHz**), one synchronous active-high reset `rst`.
  Everything is posedge-clocked. No latches, no negedge logic, no gated clocks.
* Two build flavours of the same RTL, selected by the `FAST` parameter of `soc_top`:
  * **accurate** (`FAST=0`): real SDRAM controller driving SDRAM pins (chip modelled
    in C++ with real timing), real video scan-out. This is the configuration that
    is synthesized and the one used for every reported number.
  * **fast** (`FAST=1`): cache line fills are served by an ideal in-RTL memory,
    the video timing generator is not built, the board reads the framebuffer
    arrays directly. Same CPU, same caches, same software.

---------------------------------------------------------------------------

## 2. Memory map

| Range                       | What                                    | Path            |
|-----------------------------|-----------------------------------------|-----------------|
| `0x1000_0000–0x1000_0FFF`   | UART                                    | MMIO (uncached) |
| `0x1000_1000–0x1000_1FFF`   | System control                          | MMIO            |
| `0x1000_2000–0x1000_2FFF`   | Keyboard                                | MMIO            |
| `0x1000_3000–0x1000_3FFF`   | Video control + palette                 | MMIO            |
| `0x1001_0000–0x1001_FFFF`   | Video framebuffer (64 000 bytes used)   | MMIO            |
| `0x8000_0000–0x81FF_FFFF`   | RAM (SDRAM), 32 MB in simulation        | caches → SDRAM  |

Decode rule (in `soc_top`): `addr[31] == 1` → RAM (D-cache); otherwise MMIO.
Instruction fetch always goes to RAM (I-cache). Reset PC = `0x8000_0000`.

RAM is split by convention (enforced by the linker script, not by hardware):

| Range                       | Use                                                        |
|-----------------------------|------------------------------------------------------------|
| `0x8000_0000–0x807F_EFFF`   | program: text, rodata, data, bss, heap, stack (top-down)   |
| `0x807F_F000–0x807F_FFFF`   | boot-info page written by the board (section 9)            |
| `0x8080_0000–0x81FF_FFFF`   | WAD image preloaded by the board (raw file bytes)          |

The program therefore fits the 8 MB of SDRAM a Tang Nano 20K really has; the
upper 24 MB only stands in for the storage device a real board would use.

All MMIO registers are 32-bit, little-endian, at word-aligned addresses.
The exact register list is in section 7 and in `sw/include/soc.h` (which must
match this document bit for bit).

---------------------------------------------------------------------------

## 3. Core ↔ memory port contract ("request / held response")

Both CPU ports (instruction and data) and every slave behind them (I-cache,
D-cache, MMIO block, the test memories) follow this one contract.

Signals (master = CPU side):

```
req_valid   M→S   a request is presented this cycle
req_addr    M→S   byte address (32 bit)
req_we      M→S   D-port only: 1 = store, 0 = load
req_wstrb   M→S   D-port only: byte-lane write enables for the addressed word
req_wdata   M→S   D-port only: write data, already positioned in its byte lanes
rsp_ready   S→M   the most recently accepted request has completed (or none is outstanding)
rsp_rdata   S→M   read data for the most recently accepted request (the whole aligned word)
```

Rules:

* **R1 (accept).** A request is *accepted* at a rising clock edge at which
  `req_valid == 1`. There is no `req_ready`: the master may only assert
  `req_valid` in a cycle in which that port's `rsp_ready == 1`.
* **R2 (response).** After an accept at edge E the slave drives `rsp_ready = 0`
  starting in the cycle after E until the request has completed. In the
  completion cycle `rsp_ready = 1` and, for a read, `rsp_rdata` is valid.
  The fastest legal response is completion in the cycle right after E
  (a cache hit / an MMIO access). There is no upper bound on latency.
* **R3 (hold).** Once complete, `rsp_ready` stays 1 and `rsp_rdata` stays
  **stable** until the cycle after the next accept on that port. The master may
  take arbitrarily long to consume a response and may sample it repeatedly.
* **R4 (exactly once).** Each accepted request is performed exactly once
  (matters for stores and for MMIO with side effects), no matter how long the
  master waits afterwards.
* **R5 (no combinational feedback).** `rsp_ready` and `rsp_rdata` are functions
  of slave state only; they must not depend combinationally on any `req_*`
  input of the same cycle. (The master computes `req_valid` from `rsp_ready`.)
* **R6 (reset).** While `rst` is high no request is presented. After reset a
  slave may hold `rsp_ready = 0` for an initialization period; the master waits.
* **R7 (data lanes).** Little-endian. For an access at byte address A the
  slave uses word `A[31:2]`; `req_wstrb[i]` enables byte lane i
  (`req_wdata[8*i +: 8]`), and loads return the whole word — the core selects
  and sign/zero-extends. Stores are `sb` (one strobe), `sh` (two adjacent,
  aligned), `sw` (all four). `req_wstrb` is 0 when `req_we` is 0.
* **R8 (ordering).** Requests on one port complete in the order accepted, and a
  load observes every earlier store on the same port. The I-port and D-port
  are independent: stores are *not* guaranteed to be visible to instruction
  fetch (no self-modifying code; `fence.i` is not supported).

How the core uses it: the pipeline has one global stall. A cycle is "not
stalled" only if both ports report `rsp_ready` (plus the core's own conditions).
The fetch request for the next PC and the data request of the instruction in
EX are issued only in non-stalled cycles, so there is never more than one
outstanding request per port and nothing ever needs to be cancelled.

---------------------------------------------------------------------------

## 4. `rv32_core` (rtl/core/)

```systemverilog
module rv32_core #(
    parameter logic [31:0] RESET_PC    = 32'h8000_0000,
    parameter int unsigned BTB_ENTRIES = 256,    // power of two
    parameter int unsigned BHT_ENTRIES = 1024,   // power of two
    parameter int unsigned RAS_DEPTH   = 8
) (
    input  logic        clk,
    input  logic        rst,                // synchronous, active high

    // instruction port (section 3)
    output logic        imem_req_valid,
    output logic [31:0] imem_req_addr,
    input  logic        imem_rsp_ready,
    input  logic [31:0] imem_rsp_rdata,

    // data port (section 3)
    output logic        dmem_req_valid,
    output logic        dmem_req_we,
    output logic [31:0] dmem_req_addr,
    output logic [3:0]  dmem_req_wstrb,
    output logic [31:0] dmem_req_wdata,
    input  logic        dmem_rsp_ready,
    input  logic [31:0] dmem_rsp_rdata,

    // retire trace: at most one instruction per cycle
    output logic        trace_valid,        // an instruction retired, or trapped, this cycle
    output logic        trace_trap,         // qualifies trace_valid: it trapped instead of retiring
    output logic [31:0] trace_pc,
    output logic [31:0] trace_insn,
    output logic [4:0]  trace_rd,           // register written (0 = none)
    output logic [31:0] trace_rd_wdata,     // value written (0 when trace_rd == 0)
    output logic [31:0] trace_mem_addr,     // byte address of a load/store (else 0)
    output logic [3:0]  trace_mem_wstrb,    // store byte lanes as on the D-port (0 = not a store)
    output logic [31:0] trace_mem_wdata,    // store data as on the D-port (lane-positioned)
    output logic [31:0] trace_next_pc,      // PC of the next instruction in program order

    // performance event strobes (one-cycle pulses)
    output logic        perf_branch,        // a conditional branch resolved
    output logic        perf_branch_miss,   // ... and it was mispredicted
    output logic        perf_jump,          // a jal/jalr resolved
    output logic        perf_jump_miss      // ... and its target was mispredicted
);
```

Architectural requirements:

* ISA: RV32I + M + Zicsr + Zicntr. Machine mode only. No interrupts.
* CSRs: `mstatus` (MIE/MPIE stored, MPP reads 3), `misa` (RV32IM, read-only),
  `mie`, `mip` (stored/zero, no effect), `mtvec` (direct mode), `mscratch`,
  `mepc`, `mcause`, `mtval`, `mhartid`=0, `mvendorid`/`marchid`/`mimpid`=0,
  `mcycle[h]`, `minstret[h]`, and the read-only shadows `cycle[h]` (0xC00/0xC80)
  and `instret[h]` (0xC02/0xC82). `mcycle` counts every clock cycle since reset
  (this is the machine's clock). `minstret` counts retired instructions.
  Access to any other CSR raises an illegal-instruction exception.
* Exceptions (precise): illegal instruction (2), breakpoint/`ebreak` (3),
  `ecall` from M (11), misaligned instruction address on a taken jump/branch
  (0), misaligned load (4) / store (6). `mret` returns to `mepc`.
  `wfi` and `fence`/`fence.i` execute as no-ops.
* Pipeline: IF, ID, EX, MEM, WB. Full forwarding into EX, load-use stall,
  branches and jumps resolved in EX, a dynamic branch predictor in IF
  (BTB + 2-bit counters; return-address stack), one global stall for memory.
* Multiply and divide are multi-cycle (the pipeline waits); neither may put a
  full 32×32 multiplier or a divider in a single-cycle path.
* Register file: 31×32 flops/LUT-RAM with asynchronous read and write-through,
  not reset (so it can map to distributed RAM). x0 reads 0.

Trace semantics: exactly one `trace_valid` pulse per instruction in program
order. `trace_trap=1` means the instruction raised an exception: no register
or memory effect, and `trace_next_pc` is the handler address. The order and
content of the trace must equal a sequential execution of the program
(this is what the golden-model lockstep check relies on).

---------------------------------------------------------------------------

## 5. `memsys` (rtl/mem/)

Caches plus the memory backend. Slave on both CPU ports for the RAM range.

```systemverilog
module memsys #(
    parameter bit          FAST           = 1'b0,        // 1: ideal backend, no SDRAM pins activity
    parameter int unsigned CLK_HZ         = 50_000_000,
    parameter int unsigned ICACHE_BYTES   = 8192,        // direct mapped, power of two
    parameter int unsigned DCACHE_BYTES   = 16384,       // direct mapped, power of two
    parameter int unsigned LINE_BYTES     = 32,          // 8 words = one SDRAM burst
    parameter int unsigned SDRAM_ROW_BITS = 13,          // 13 in simulation (32 MB), 11 on Tang Nano 20K (8 MB)
    parameter int unsigned SDRAM_COL_BITS = 8
) (
    input  logic        clk,
    input  logic        rst,

    // CPU instruction port (slave, section 3)
    input  logic        i_req_valid,
    input  logic [31:0] i_req_addr,
    output logic        i_rsp_ready,
    output logic [31:0] i_rsp_rdata,

    // CPU data port, RAM range only (slave, section 3)
    input  logic        d_req_valid,
    input  logic        d_req_we,
    input  logic [31:0] d_req_addr,
    input  logic [3:0]  d_req_wstrb,
    input  logic [31:0] d_req_wdata,
    output logic        d_rsp_ready,
    output logic [31:0] d_rsp_rdata,

    // SDRAM pins (section 6). With FAST=1 they are driven to the idle/NOP state.
    output logic        sdram_cke,
    output logic        sdram_cs_n,
    output logic        sdram_ras_n,
    output logic        sdram_cas_n,
    output logic        sdram_we_n,
    output logic [1:0]  sdram_ba,
    output logic [SDRAM_ROW_BITS-1:0] sdram_addr,
    output logic [3:0]  sdram_dqm,
    output logic [31:0] sdram_dq_o,
    output logic        sdram_dq_oe,
    input  logic [31:0] sdram_dq_i,

    // performance event strobes (one-cycle pulses)
    output logic        perf_ic_access,     // an instruction fetch request was accepted
    output logic        perf_ic_miss,       // ... and it missed
    output logic        perf_dc_access,     // a data request was accepted
    output logic        perf_dc_miss        // ... and it missed
);
```

* RAM offset = `addr[24:0]` (address bits above the RAM size are ignored).
  SDRAM word index = `offset >> 2`, split as `{ba, row, col}` with `col` in the
  low `SDRAM_COL_BITS` bits. The chip model's backing array is indexed by that
  same word index, so byte offset N of RAM lives in model word `N >> 2`,
  lane `N & 3`.
* I-cache: read-only, direct mapped. D-cache: direct mapped, write-back with
  dirty bits, write-allocate. Cache data and tags use synchronous-read memories
  that infer block RAM (no reset on the arrays; valid bits may be flops).
* Hit latency is the minimum the contract allows (response in the cycle after
  accept). Misses stall via `rsp_ready`: a miss completes when its word arrives
  (critical word first) while the rest of the line is refilled in the
  background. Requests accepted meanwhile are served if they hit; one to a word
  of that line not yet in, or a D-cache store while the one-entry store buffer
  is still full, completes when it can (R2), another miss waits for the refill.
  The D-cache writes a dirty victim back after the refill, from a one-line
  write buffer.
* `FAST=1` instantiates `mem_ideal` instead of `sdram_ctrl`: a plain
  `logic [31:0] mem [0:2**23-1]` array (32 MB) answering line requests in the
  minimum number of cycles. The array is `/* verilator public */` so the board
  can preload it; its hierarchical name is documented in rtl/mem/README.md.

---------------------------------------------------------------------------

## 6. SDRAM pins and chip model

Device modelled: single-data-rate SDRAM, 32-bit data bus, 4 banks,
`SDRAM_ROW_BITS` row bits, `SDRAM_COL_BITS` column bits (the Tang Nano 20K's
embedded part is the 11-row/8-column, 8 MB member of this family; simulation
uses 13 rows = 32 MB with identical timing). Standard command encoding on
`cs_n/ras_n/cas_n/we_n`; `addr[10]` is the auto-precharge / precharge-all bit.

Timing at 50 MHz (20 ns cycle). The controller derives cycle counts from
`CLK_HZ`; the chip model checks them:

| Parameter | Value | Cycles @50 MHz |
|-----------|-------|----------------|
| Power-up delay before first command | 200 µs | 10 000 |
| tRP  (precharge → next command)     | 18 ns  | 1 |
| tRCD (ACTIVE → READ/WRITE)          | 18 ns  | 1 |
| tRAS (ACTIVE → PRECHARGE, min)      | 42 ns  | 3 |
| tRC / tRFC (ACTIVE→ACTIVE, refresh) | 60 ns  | 3 |
| tWR  (last write data → PRECHARGE)  | 2 clk  | 2 |
| tMRD (mode register set → command)  | 2 clk  | 2 |
| CAS latency                         | 2      | 2 |
| Refresh                             | 4096 per 64 ms | one per ≤ 781 cycles on average |

Init sequence: power-up delay with NOPs, PRECHARGE ALL, ≥ 2 AUTO REFRESH,
MODE REGISTER SET (CL=2, sequential burst, burst length = line length in
words), then normal operation.

Pin timing convention (this is what makes the C++ model and the RTL agree):

* All `sdram_*` outputs come straight from flip-flops. The chip samples them at
  the rising edge of `clk`.
* For a READ sampled at edge k, data beat n is on `sdram_dq_i` during the cycle
  *before* edge k + CL + n, i.e. the controller's input flops capture beat n at
  edge k + CL + n.
* For a WRITE sampled at edge k, beat n must be on `sdram_dq_o` (with
  `sdram_dq_oe = 1`) at edge k + n.
* `sdram_dqm` is held 0 (all lanes enabled) during bursts.

C++ chip model (`sim/models/sdram_model.h`): one call per clock cycle, made
*before* the rising-edge `eval()`, takes the pin values the DUT is currently
driving and returns the value `sdram_dq_i` must take *after* that edge:

```cpp
struct SdramPins { bool cke, cs_n, ras_n, cas_n, we_n; uint8_t ba; uint16_t addr;
                   uint8_t dqm; uint32_t dq_o; bool dq_oe; };
class SdramModel {
public:
    SdramModel(unsigned row_bits, unsigned col_bits, const SdramTiming& t);
    uint32_t tick(const SdramPins& pins);     // returns dq_i for the cycle after this edge
    uint8_t* bytes();  size_t size_bytes();   // backdoor to the storage (preload / inspect)
    uint64_t violations() const;              // protocol/timing violations seen (must stay 0)
};
// per cycle:
//   uint32_t dq_next = sdram.tick(pins_from(top));
//   top->clk = 1; top->eval();
//   top->sdram_dq_i = dq_next;
//   top->clk = 0; top->eval();
```

---------------------------------------------------------------------------

## 7. Peripherals (rtl/periph/), all MMIO

MMIO accesses complete in the cycle after accept. Unmapped MMIO addresses read
0 and ignore writes. Register reads have no side effects except `KBD_DATA`.

### UART — `0x1000_0000`
| Offset | Name        | Access | Bits |
|--------|-------------|--------|------|
| 0x0    | UART_TX     | W      | [7:0] byte to send. Ignored if the transmitter is busy. |
| 0x4    | UART_STATUS | R      | bit0 `tx_ready` (1 = a byte may be written) |

8N1, `UART_DIV` clock cycles per bit (parameter; 434 = 115 200 baud at 50 MHz).
Idle line is high. Transmit only.

### System control — `0x1000_1000`
| Offset | Name        | Access | Bits |
|--------|-------------|--------|------|
| 0x0    | SYS_CLK_HZ  | R      | clock frequency in Hz (the `CLK_HZ` parameter) |
| 0x4    | SYS_EXIT    | W      | any write latches `exit_code` = value and sets `halted` (sticky). The board ends the run. |
| 0x8    | SYS_FLAGS   | R      | bit0 = 1 in the FAST build |

Time is read with `rdcycle`/`rdcycleh`; `ms = cycles / (SYS_CLK_HZ / 1000)`.

### Keyboard — `0x1000_2000`
| Offset | Name       | Access | Bits |
|--------|------------|--------|------|
| 0x0    | KBD_DATA   | R      | pops one event: bit31 valid, bit8 1=press 0=release, [7:0] key code. Reads 0 when empty. |
| 0x4    | KBD_STATUS | R      | bit0 = an event is waiting (no side effect) |

Events arrive from the board on `key_valid`/`key_data[8:0]` (`{pressed, code}`)
into a 16-entry FIFO; events arriving while full are dropped. Key codes are
Doom's (`doomkeys.h`); the board translates.

### Video — `0x1000_3000` and `0x1001_0000`
| Address                   | Name         | Access | Bits |
|---------------------------|--------------|--------|------|
| `0x1000_3000`             | VID_FLIP     | W      | any write: "the framebuffer now holds a complete frame". Pulses `vid_flip`, increments the flip counter. |
| `0x1000_3000`             | VID_FLIP     | R      | flip counter |
| `0x1000_3004`             | VID_STATUS   | R      | bit0 = scan-out is outside the 400 active framebuffer lines (always 1 in FAST); [31:16] = scan-out frame counter |
| `0x1000_3400–0x1000_37FC` | VID_PALETTE  | W      | entry i at `+4*i`: `0x00RRGGBB` |
| `0x1001_0000–0x1001_F9FF` | VID_FB       | W      | 320×200 bytes, row-major, one palette index per pixel; byte/half/word stores all work (byte lanes) |

Scan-out (accurate build): 640×480@60 timing, 800×525 total, pixel rate
`clk/2` (`vid_pix_ce` pulses every other cycle). Horizontal: 640 active, 16
front porch, 96 sync, 48 back porch. Vertical: 480 active, 10 front porch,
2 sync, 33 back porch. Sync pulses are active low. The 320×200 framebuffer is
pixel-doubled to 640×400 and shown on active lines 40–439; active lines 0–39
and 440–479 are black. `vid_de` is high for all 640×480 active pixels.
Pixel value = palette[framebuffer[y][x]], 8 bits per channel.

The framebuffer (four byte-lane memories of 16 000 entries) and the palette
(256×24) are synchronous-read memories; the framebuffer infers block RAM (32
blocks) and the palette distributed RAM, which is what lets the whole design
fit the device's 46 blocks. Both are `/* verilator public */` so the board can
snapshot them. A CPU store and a scan-out read of the same word in the same
cycle may return either the old or the new value (it is one pixel for one
frame); nothing else reads these memories.

---------------------------------------------------------------------------

## 8. `soc_top` (rtl/soc/)

```systemverilog
module soc_top #(
    parameter bit          FAST           = 1'b0,
    parameter int unsigned CLK_HZ         = 50_000_000,
    parameter int unsigned UART_DIV       = 434,
    parameter int unsigned ICACHE_BYTES   = 8192,
    parameter int unsigned DCACHE_BYTES   = 16384,
    parameter int unsigned SDRAM_ROW_BITS = 13,
    parameter int unsigned SDRAM_COL_BITS = 8
) (
    input  logic        clk,
    input  logic        rst,

    // SDRAM (section 6)
    output logic        sdram_cke,
    output logic        sdram_cs_n,
    output logic        sdram_ras_n,
    output logic        sdram_cas_n,
    output logic        sdram_we_n,
    output logic [1:0]  sdram_ba,
    output logic [SDRAM_ROW_BITS-1:0] sdram_addr,
    output logic [3:0]  sdram_dqm,
    output logic [31:0] sdram_dq_o,
    output logic        sdram_dq_oe,
    input  logic [31:0] sdram_dq_i,

    // serial
    output logic        uart_tx,

    // keyboard events from the board
    input  logic        key_valid,
    input  logic [8:0]  key_data,

    // video scan-out (constant/idle when FAST=1)
    output logic        vid_pix_ce,
    output logic        vid_hsync,
    output logic        vid_vsync,
    output logic        vid_de,
    output logic [7:0]  vid_r,
    output logic [7:0]  vid_g,
    output logic [7:0]  vid_b,

    // run control / observation (board only; unconnected on FPGA)
    output logic        halted,
    output logic [31:0] exit_code,
    output logic        vid_flip,           // one-cycle pulse on a VID_FLIP write
    output logic        uart_tx_valid,      // one-cycle pulse: a byte was accepted by the UART
    output logic [7:0]  uart_tx_byte,

    // retire trace, straight from rv32_core
    output logic        trace_valid,
    output logic        trace_trap,
    output logic [31:0] trace_pc,
    output logic [31:0] trace_insn,
    output logic [4:0]  trace_rd,
    output logic [31:0] trace_rd_wdata,
    output logic [31:0] trace_mem_addr,
    output logic [3:0]  trace_mem_wstrb,
    output logic [31:0] trace_mem_wdata,
    output logic [31:0] trace_next_pc,

    // free-running event counters (64 bit, reset to 0)
    output logic [63:0] cnt_cycle,
    output logic [63:0] cnt_instret,
    output logic [63:0] cnt_ic_access,
    output logic [63:0] cnt_ic_miss,
    output logic [63:0] cnt_dc_access,
    output logic [63:0] cnt_dc_miss,
    output logic [63:0] cnt_branch,
    output logic [63:0] cnt_branch_miss,
    output logic [63:0] cnt_jump,
    output logic [63:0] cnt_jump_miss,
    output logic [63:0] cnt_flip
);
```

`cnt_instret` counts `trace_valid && !trace_trap`.

---------------------------------------------------------------------------

## 9. Boot conventions

There is no boot ROM in simulation: the board preloads RAM and releases reset.

* The ELF's loadable segments are copied to RAM; execution starts at
  `0x8000_0000` (`_start`, in `.text.init`).
* Boot-info page at `0x807F_F000`, written by the board (all zero if absent):

```c
#define BOOTINFO_MAGIC 0xB007F00D
struct bootinfo {            /* little-endian, 1040 bytes */
    uint32_t magic;          /* BOOTINFO_MAGIC when the page is valid */
    uint32_t wad_addr;       /* 0x80800000, or 0 if no WAD was loaded */
    uint32_t wad_size;       /* bytes */
    uint32_t reserved;
    char     wad_name[64];   /* file name of the WAD without directories, e.g. "doom1.wad" */
    char     cmdline[960];   /* NUL-terminated, space-separated arguments (argv[1..]) */
};
```
  `argv[0]` is the fixed string `"prog"`. When a WAD is loaded the C library
  exposes it as a read-only file that `fopen(wad_name, "rb")` can open; no
  other file exists and nothing can be created (opens fail, as on a machine
  without storage).
* Stack pointer starts at `0x807F_F000` and grows down; the heap grows up from
  the end of `.bss` and must stay below the stack reserve.
* A program ends by writing its exit status to `SYS_EXIT`, then spinning.
* riscv-tests end by storing to their `tohost` symbol instead (1 = pass,
  anything else = fail with `tohost >> 1` = failing test number); the test
  runners watch for that store on the retire trace.

---------------------------------------------------------------------------

## 10. Software stack (sw/)

Compiler: `riscv64-elf-gcc -march=rv32im_zicsr -mabi=ilp32` (see `config.mk`).
The toolchain ships **no C library**; the project has its own small
freestanding one.

```
sw/include/soc.h     register map (C view of section 2 and 7) — shared, do not fork
sw/bsp/              crt0.S, link.ld, board support (console, exit, clock, heap)
sw/libc/             the C library (headers + libc.a) used by every program
sw/hello/            serial hello world
sw/testpattern/      palette + framebuffer test pattern
sw/coremark/         CoreMark port
sw/doom/             Doom (doomgeneric) port, native reference build, patches
```

---------------------------------------------------------------------------

## 11. Golden model (sim/iss/)

An instruction-set simulator of the same machine (same memory map, same MMIO
behaviour, same boot conventions) used three ways: to develop software before
and independently of the RTL, as the reference in lockstep comparison against
the RTL's retire trace, and as a third engine for the virtual board.
Its clock is defined as one cycle per instruction (`mcycle == minstret`).
It is also the functional core of the cycle-exact model (`sim/model`), the
board's fourth engine, which adds the accurate machine's timing and is itself
checked against the RTL in lockstep (`--cosim=model`).

---------------------------------------------------------------------------

## 12. Coding rules

RTL
* SystemVerilog, synthesizable, accepted by **both** Verilator 5.054 and Yosys
  with the slang frontend. `logic` everywhere, `always_ff` / `always_comb`,
  packages and packed structs/enums are fine. No interfaces, no classes, no
  `initial` blocks with delays, no `#delays`, no tri-states inside the SoC.
* One module per file, file name = module name. `default_nettype none` at the
  top of each file, `default_nettype wire` at the end.
* Synchronous active-high reset. Memories that should infer RAM are not reset.
* Must be clean under `verilator --lint-only -Wall` (waive narrowly, with a
  comment, only when the warning is wrong for the design).
* Simulation-only code is wrapped in `` `ifndef SYNTHESIS ``.

C / C++
* C11 for target software, C++17 for host tools. No dependencies beyond the
  standard library and SDL2.
* Warnings on (`-Wall -Wextra`), builds clean.

Repository
* Each directory builds from its own `Makefile`, which includes `config.mk`
  for tool paths and writes all products under `build/<area>/`.
* `third_party/` is fetched, never edited. Changes to third-party code live in
  this repository as patch files applied to a build copy.

---------------------------------------------------------------------------

## 13. Frame dump format ("DFRM")

Every engine that can show Doom (native reference build, ISS, RTL board) can
write the frames it presents to a file in this one format, so any two runs can
be compared pixel for pixel.

```
header, 16 bytes:  "DFRM"            4 bytes magic
                   uint32 version    = 1          (little-endian)
                   uint32 width      = 320
                   uint32 height     = 200
then one record per presented frame, 64 768 bytes each:
                   768 bytes  palette: R,G,B for entries 0..255 (as shown, i.e. after gamma)
                   64 000 bytes  palette indices, row-major
```

A frame is "presented" each time the program completes one: on the SoC that is
the `VID_FLIP` write (the board snapshots framebuffer + palette at that
moment); in the native build it is the same point in the code. Two frames are
equal when every pixel has the same RGB value after palette lookup.
