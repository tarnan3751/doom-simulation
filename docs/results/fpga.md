# FPGA implementation: Sipeed Tang Nano 20K

Target: Sipeed Tang Nano 20K, Gowin GW2AR-LV18QN88C8/I7 (20 736 LUT4, 15 552 flops,
46 block RAMs of 18 kbit, 48 18x18 multipliers, 2 PLLs, 27 MHz oscillator, 8 MB
in-package SDR SDRAM, HDMI connector). Open tool chain only: Yosys with the slang
frontend, nextpnr-himbaechel, gowin_pack.

**The design has never been run on hardware.** Everything below is what synthesis,
place and route and simulation of the FPGA-only parts show; how far the timing
numbers can be trusted is in section 4.

Reproduce: `make -C synth -j3` (about 45 min on 10 cores); generated details in
`build/synth/report.md`; flow, decisions and limits in `synth/README.md`.
Numbers below: the RTL after the performance pass (background cache refill, write
and store buffers, early-out divider).

## 1. Result

The SoC (`soc_top` with the accurate SDRAM backend, 8 KB I-cache, 16 KB D-cache,
DVI video output) fits the device and meets its 50.4 MHz system clock.

| design | block RAM | LUT4 | flops | system clock Fmax, MHz (best / median / worst) |
|---|---|---|---|---|
| **full SoC on the board** (5 seeds) | **46 / 46 (100 %)** | 7 613 (37 %) | 1 933 + 57 in I/O blocks (12 %) | **63.8 / 60.2 / 57.1** |
| SoC without the video output path (3 seeds) | 14 | 7 886 (38 %) | 1 697 (11 %) | 63.9 / 62.3 / 62.1 |
| CPU core alone, 8 KB local RAM instead of caches (3 seeds) | 5 | 5 068 (24 %) | 1 080 (7 %) | 75.0 / 71.8 / 70.1 |

* Target: 50.4 MHz (19.84 ns). Each seed's Fmax is the worse of its two timing
  views (section 4); the worst seed of the full SoC has 13 % margin.
  Per seed: 57.1, 60.2, 59.0, 63.8, 61.9 MHz. Before the performance pass the
  same design reached 67.4 / 66.5 / 62.9 with 6 100 LUT4: the background cache
  refill, the write and store buffers and the divider's normalisation step cost
  about 1 500 LUT4 and 6 MHz of worst-seed margin. A further cache change that
  saved 1.4 % of cycles was dropped because one seed failed to route with it.
* The 25.2 MHz pixel clock domain reaches 77.3 MHz or more.
* The LUT counts of the smaller designs are not proportionally smaller: Yosys
  maps the same logic into 10-30 % more or fewer LUTs from one netlist to the
  next, so only the flop and RAM counts compare exactly.
* Other resources of the full SoC: 231 of 648 LUT-RAM sites (36 %), 1 of 12
  MULT36X36, both PLLs, 1 clock divider; 17 package pins plus the 55 pads of the
  in-package SDRAM.
* The design uses every block RAM of the device: framebuffer 32, I-cache data 4,
  D-cache data 8, cache tags 1, branch-target buffer 1. The palette and the branch
  history table are distributed (LUT) RAM. With those two in block RAM as well the
  design would need 48 and not fit.

## 2. Resources

Full SoC, Yosys cells per module (flattened netlist; cells are attributed by
their instance path, so the split of LUT, ALU, MUX2 and flop cells between
neighbouring modules is approximate; RAM and DSP counts are exact).

| module | LUT1-4 | ALU | MUX2 | flops | LUT RAM | block RAM | DSP |
|---|---|---|---|---|---|---|---|
| core: pipeline, decode, ALU, forwarding | 1 476 | 156 | 327 | 524 | - | - | - |
| core: CSRs and counters | 655 | 203 | 199 | 258 | - | - | - |
| core: mul/div unit | 651 | 137 | 101 | 240 | - | - | 1 |
| core: register file | 182 | - | 64 | 37 | 32 | - | - |
| core: branch predictor (BTB, BHT, RAS) | 647 | 69 | 74 | 14 | 72 | 1 | - |
| memsys: I-cache | 79 | 11 | 13 | 66 | - | 4 | - |
| memsys: D-cache (fill engine, store buffer, write buffer) | 790 | 54 | 421 | 251 | 24 | 8 | - |
| memsys: shared tags, SDRAM controller, arbiter, other | 248 | 48 | 48 | 130 | - | 1 | - |
| mmio: video (framebuffer, palette, scan-out) | 484 | 141 | 56 | 152 | 96 | 32 | - |
| mmio: UART, keyboard, system control, decode | 70 | 18 | 15 | 30 | - | - | - |
| soc_top glue | 74 | - | 28 | 34 | - | - | - |
| board: DVI (clock-crossing FIFO, TMDS encoders, serialisers) | 263 | 66 | 86 | 142 | 7 | - | - |
| board: clocks, resets, I/O, LEDs | 98 | 47 | 5 | 107 | - | - | - |
| **total** | **5 717** | **950** | **1 437** | **1 985** | **231** | **46** | **1** |

Synthesis adds no read-during-write logic to any memory: the framebuffer, palette
and predictor memories are declared `no_rw_check`, like the cache memories, since
a same-cycle write and read of one word may return either value without harm (one
pixel of one frame, or one prediction that EX checks anyway).

The CPU core alone: 4 060 LUT1-4, 591 ALU, 1 210 MUX2, 1 082 flops, 104 LUT RAM
(register file 32, return stack 8, branch history 64), 1 block RAM (branch-target
buffer), 1 multiplier.

Cell counts of logic vary by 10-30 % between functionally identical netlists
(Yosys' LUT mapping); flop and RAM counts are exact.

## 3. Critical paths

The worst paths of the full SoC, all between 14.9 and 17.5 ns:

1. **Memory-ready loop** (15.5-17.5 ns, the worst in 9 of 10 runs): cache tag RAM
   output -> tag compare -> port ready -> the core's global `advance` -> data-port
   request valid -> SoC address decode -> MMIO accept -> framebuffer write enable
   (17.5 ns, of which 9.8 ns routing), or -> the D-cache's RAM enables, bypass and
   store-buffer registers (15.5-17.0 ns). Every request valid and pipeline enable
   depends on the ready signals of both ports, and the D-cache's ready now also
   folds in the fill, store-buffer and write-buffer state.
2. **I-cache data RAM address** (15.6 ns, only visible in the port-swapped view):
   the fill engine's address register -> read address of the data RAM.
3. **Execute-stage resolution** (14.9 ns): forwarding multiplexer -> branch
   compare / exception detection -> trap register (`mtval`, `mcause`) write enables.

Before the performance pass the branch-prediction path into the I-cache address
(15.9 ns) was the worst; it is unchanged and now sits below the memory-ready
loop.

Without the video path and in the core alone, the decode-stage path instruction
word -> immediate -> `pc + imm` adder -> compare with the predicted target also
reaches 13-15 ns. The multiplier is not among these because it is not analysed
(section 4).

## 4. How far the timing analysis can be trusted

* **Block-RAM read addresses.** This nextpnr build has no setup check on the
  port-B address pins of the true-dual-port block RAM primitives (measured with
  probe designs, `make -C synth probes`). Synthesis puts every memory's read port
  there, so the fetch- and load/store-address paths into the cache RAMs would be
  missing from a single analysis. The flow therefore routes every design twice:
  as built, and with ports A and B of every true-dual-port block RAM exchanged
  (a functionally identical netlist in which those pins are analysed). The Fmax of
  a seed is the worse of the two; the formerly hidden paths are real and among
  the longest (about 15 ns).
* **Multiplier.** The tool chain has no timing data for the DSP blocks at all; the
  multiply path (registers -> MULT36X36 -> register, one full cycle) is not
  analysed. The routing around it takes at most 5.3 ns, leaving at
  least 13.9 ns of the 19.84 ns cycle for the multiplier itself, whose
  delay is not documented in any source available here.
* **I/O.** No I/O timing model. The SDRAM interface timing is an argument with
  typical datasheet numbers, not a measurement (section 5).
* **Everything else** (logic, carry chains, LUT RAM, flops, block-RAM outputs,
  routing) uses the vendor timing tables for speed grade C8/I7. Seed-to-seed
  placement noise is about +-2 MHz; Yosys' LUT mapping of the same logic varies by
  10-30 % in cell count, which can move a marginal path by more.

## 5. The board top level (`rtl/fpga/`)

* **Clocks.** 27 MHz -> PLL A: 126 MHz TMDS bit clock (DDR), divided by 5 to the
  25.2 MHz pixel clock, and 21 MHz -> PLL B: 50.4 MHz system clock plus a copy
  shifted by 315 degrees for the SDRAM. The system clock is exactly twice the
  pixel clock because the SoC generates its 640x480 raster at clk/2; 25.2 MHz is
  the 60.00 Hz pixel clock of that mode. The SoC is built with
  `CLK_HZ = 50_400_000`. The one-PLL fallback (`PLL_CASCADE = 0`, which cannot
  phase-shift the SDRAM clock) routed before the performance pass but no longer
  routes with the larger memory system: if cascaded PLLs turn out not to work on
  silicon, the clocking has to be rethought.
* **Reset.** Asserted at configuration and while the PLLs are unlocked, held for
  2^16 cycles after the last request (debouncing button S1), released
  synchronously.
* **SDRAM.** The in-package 8 MB part (2M x 32, 11 row / 8 column bits) is reached
  through top-level ports with fixed names (`O_sdram_*`, `IO_sdram_dq`) that
  nextpnr places from its chip database; all 55 land on the documented pads. One
  bidirectional buffer per data bit; command/address and read-data registers sit in
  the I/O blocks; the clock is forwarded through an output DDR register. The
  controller assumes the chip samples each command one cycle after it is sent; the
  SDRAM clock leads the system clock by 2.48 ns, which keeps that mapping and
  gives hold margin. With typical SDR SDRAM numbers the margins are about +1 ns on
  command hold and on read-data hold and more than 10 ns on every setup check. An
  in-phase clock would violate command hold; the common 180-degree clock would
  capture read data between two beats.
* **Video.** DVI over the HDMI connector: an asynchronous FIFO from the system to
  the pixel clock, three TMDS encoders (DVI 1.0), four 10:1 serialisers and
  true-LVDS output buffers. The encoder matches an independent model of the DVI
  algorithm bit for bit over 6 M symbols; the whole pixel path is simulated with
  two clocks at ten phase offsets over two frames without a single wrong pixel,
  and a 0.5 % clock error is detected (sticky LED) before any corrupted pixel.
* **Serial** on pin 69 (to the on-board USB-UART). **LEDs:** heartbeat, halted,
  PLLs locked, video FIFO slip. **Keyboard** inputs tied off (would come from a
  PS/2 or USB-HID receiver). Trace, counters and run-control outputs are left open
  and removed by synthesis.
* **Pins:** `rtl/fpga/tangnano20k.cst` marks every pin as confirmed from the local
  chip database (the pin exists, its I/O block and special function) or as
  unverified board wiring.
* A bitstream is produced (`build/synth/tangnano20k.fs`); it has never been loaded
  into a device.

## 6. What is missing for a real board

* **Program loading.** In simulation the board model preloads SDRAM. On the FPGA
  the SDRAM is empty after power-up and there is no loader; all 46 block RAMs are
  used, so a boot ROM would have to live in logic or LUT RAM, or a loader in the
  board top would copy an image from the on-board SPI flash before releasing reset.
* **WAD storage.** The memory map places the WAD at 0x8080_0000-0x81FF_FFFF, beyond
  the 8 MB part (on the board those addresses alias the program). A storage device
  (SPI flash window, SD card) and a matching file driver are needed.
* **Keyboard input** (PS/2 or USB-HID receiver).
* **Unverified on hardware or from local sources:** board wiring of the 27 MHz
  oscillator, reset button (and its polarity), UART pin, LEDs and HDMI lane order;
  that a PLL fed from the other PLL works on silicon; the SDRAM clock phase; the
  multiplier's delay; signal integrity of the TMDS outputs.
