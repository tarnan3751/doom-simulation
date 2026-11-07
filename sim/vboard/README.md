# vboard - the virtual board

A C++/SDL2 program that wraps a model of the SoC and gives it a screen, a keyboard, a serial terminal and live
performance counters. Four interchangeable engines: `iss` (instruction-set simulator), `fast` (soc_top FAST=1 under
Verilator: ideal memory), `accurate` (soc_top FAST=0: real SDRAM pins against the C++ chip model, real scan-out
captured from the pixel pins, serial decoded from the `uart_tx` pin) and `model` (`sim/model`: the accurate machine's
exact timing in software, several times faster than real time on Doom), plus lockstep cosimulation of either RTL
engine against the ISS (`--cosim`) and of the accurate engine against the model (`--cosim=model`).

Build: `make -C sim/vboard` (-> `build/vboard/vboard`; `RTL_DIR=`, `OUT=` select other RTL / another product directory).
Test: `make -C sim/vboard test` (a few minutes), `make -C sim/vboard test-long` (several minutes: 400 Doom frames under
both cosims, full CoreMark). Needs Verilator 5.054, SDL2, python3, and for the tests the programs under `build/sw`, Doom, the WAD,
the reference dump and `build/tools/framecmp`.

## Usage

`vboard [options] program.elf [-- arg ...]`; `vboard --help` lists the options. Arguments after `--` reach the program
through the boot-info command line (`argv[0]` is `prog`). stdout carries exactly the bytes the CPU sent to the UART
(flushed after every engine slice); everything else (statistics, diagnostics, "screenshot written") goes to stderr.
`--screenshot-frame N` takes the screenshot at exit if frame N is never reached; `--key-script` also works headless.

Exit status: low byte of `SYS_EXIT`; `--isa-test` 0 pass / 1 fail; **119** cosim mismatch; **120** window closed / quit
hotkey; **121** hardware checker error (SDRAM protocol/timing violation, serial framing/timing error, video timing error,
scan-out mismatch); **122** `--max-frames`; **123** `--max-seconds`; **124** `--max-cycles`; **125** fault (bad
fetch/access, message on stderr); **126** setup error (file, ELF, WAD, engine not built, SDL); **2** usage. A program that
itself exits with 120-126 is indistinguishable by status alone (stderr / `--stats-json` `stop` tell).

Hotkeys, never forwarded to the machine: F12 writes `vboard-shot-NNN.ppm` (the composed window) to the current directory;
backquote pauses / resumes (and releases held keys); Cmd+Q, Cmd+W or closing the window quit (status 120); no chord with
Cmd is forwarded. Other keys are translated to Doom key codes as in `doomgeneric_sdl.c` (arrows, Ctrl fire, Space use,
Shift run, Alt strafe, Enter, Esc, Tab, Backspace, F1-F11, digits, letters, punctuation, keypad, Home/End/PgUp/PgDn/Ins/Del).
Auto-repeat is dropped; two keys with the same Doom code (left+right Shift) give one press and one release; losing focus
releases everything.

## Window and HUD

Left: the 640x480 monitor image (framebuffer pixel-doubled to 640x400 on lines 40..439, black bars; "NO SIGNAL" until the
first frame). Right: a 360 px instrument panel. The whole window is composed in one RGB `Canvas`, so `--screenshot` writes
exactly what the window shows; the monitor part is redrawn and uploaded only when the engine has a new image.

| Field | Meaning |
|---|---|
| status pill | RUNNING / PAUSED / HALTED, EXIT n / PASS or FAIL n (isa-test) / FAULT / STOPPED: reason |
| big number, "average" | simulation speed (ISS: retired instructions per wall second, MIPS; RTL: simulated MHz), recent / since start |
| IPC, I-/D-cache hit, Branch / Jump accuracy | retired instructions / cycles; 1 - misses / accesses; 1 - mispredicts / resolved |
| fps @ sim clock, fps @ wall clock | frames per simulated second ("on hardware: X fps at 50 MHz"); frames per wall second |
| counters | simulated time, wall time (pauses excluded), cycles, instructions, frames presented |
| SDRAM violations, Serial / video errors, Scan-out ok/skip/bad | accurate engine checkers (see below) |

"SINCE RESET" uses totals, "RECENT" the difference to a sample about 2 s old (`Hud::kWindow`; since-reset for the first
50 ms). Unavailable fields show `-`. Rates use run time, so pausing does not distort them.

## Front end and engine interface (`engine.h`)

```cpp
class Engine {
  const EngineInfo& info() const;                    // name, clk_hz, speed unit, native_monitor
  simc::WriteFn mem_writer();                        // preload RAM (sim/common loader: ELF, WAD, boot-info page)
  void watch_tohost(uint32_t addr);                  // --isa-test
  bool start(uint32_t entry, std::string& err);      // release reset
  RunResult run(uint64_t max_cycles);                // bounded work; returns early on Event bits
  const Status& status() const;  Stats stats();      // run state / exit code / fault text; counters
  void push_key(bool pressed, uint8_t doom_code);
  std::function<void(uint8_t)> on_serial;            // fired from inside run()
  std::function<void(const FrameView&)> on_frame;    // at each VID_FLIP: indices + palette + frame number
  const uint32_t* monitor_image();  uint64_t monitor_serial();   // 640x480 0x00RRGGBB; changes when the image may have
  std::string diagnostics();                         // cosim report, checker logs
};
make_engine(name, EngineOptions{cosim, cosim_model, check_scanout, keep_going, ...}, err);
```
Engines: `iss`, `fast`, `accurate`, `model`. Events: `EvFrame`, `EvHalted`, `EvFault`, `EvTohost`, `EvCheck` (hardware checker), `EvCosim`. `Stats` holds `Counter{v, ok}`
fields (never faked when an engine lacks them) plus an `extras` list for the summary/JSON. The base class owns frame
snapshots and monitor synthesis (`present_frame()`); the accurate engine overrides `monitor_image()` with the real scan-out.
The front end (`main.cpp`) slices work by wall time (5 ms windowed, 25 ms headless, budget from the measured rate) and does
events, HUD and presentation between `run()` calls only.

## The Verilator engines (`engine_rtl.cpp`)

`soc_top` is verilated twice from the same sources (`--prefix Vsoc_fast -GFAST=1 -GUART_DIV=4`, `--prefix Vsoc_acc -GFAST=0
-GUART_DIV=434`); `engine_rtl.cpp` is compiled once per model (`VB_MODEL`, `VB_ACC`, `VB_UART_DIV`), each with its own
`VerilatedContext`.
* Arrays (`fb0..3`, `palette`, `mem_ideal`'s `mem`) are found through the scope table (`scopeNameMap()` -> `varFind()`):
  Verilator does not flatten modules with public arrays, and this one method works for all three.
* Per cycle (`cycle<TRACE>()`): accurate: `SdramModel::tick(pins)` before the edge (ARCHITECTURE.md section 6), the edge,
  `sdram_dq_i` from the model, `UartDecoder::tick(uart_tx)`, `VideoMonitor::tick(vid_*)`, a vsync-edge check for the
  scan-out snapshots; fast: the `uart_tx_valid/uart_tx_byte` tap. Both: `vid_flip`, `halted`; `trace_*` only with `--cosim` or
  `--isa-test` (template parameter). Counters and errors are read between slices.
* Reset is held for 5 cycles (the accurate controller then needs about 10 000 cycles of SDRAM power-up). After `halted` the
  engine runs a bounded drain (accurate: 12 UART bit times + 64, cosim: 64) so the last serial byte and trace records are not lost.
* Keyboard: `push_key` queues events; one per cycle is driven onto `key_valid/key_data` (and pushed into the cosim ISS).
* `cnt_flip` steps one cycle after the `vid_flip` pulse; stats use `max(cnt_flip, frames seen)`, exact at a stop.
* Accurate checks stop the run (status 121) at the end of the slice in which they fire: SDRAM violations, UART framing /
  bit-timing errors, video timing deviations, scan-out mismatches. `--keep-going` continues, but the status is still 121.
* **Scan-out self-check**: the framebuffer and palette are snapshotted at each vsync falling edge on the pins; when the monitor
  publishes a frame and the snapshots at both of its boundaries are equal, the frame must equal `synth_monitor(fb, palette)`.
  Frames whose arrays changed are skipped (Doom: about half); a change undone within one frame is invisible.

**One clock edge per cycle, past `eval()`.** All logic is posedge-clocked, so the clk=0 `eval()` would only re-arm the edge
detection: `low_phase()` keeps clk at 1 and clears the "previous clk" trigger memory instead. `eval_step()` runs the time step
of `VerilatedEvalLoop::evalImpl` itself, calling the generated region functions (`<prefix>___024root___eval_ico/act/nba/...`)
directly, without the virtual calls, `VlDeleter::deleteAll()` (no class objects) and `Verilated::endOfEval()` (a mutex round
trip per cycle for a queue only `--threads` mtasks use), and skips the input-combinational region while rst, keys and
`sdram_dq_i` equal that region's own trigger memory. This depends on Verilator 5.054 internals, so `vl_check.py` proves the
needed facts on every generated model (the only edge trigger is posedge clk; the ico region watches exactly clk, rst,
sdram_dq_i, key_valid, key_data and has no logic for clk or for its first iteration; no thread pool) and writes
`vb_model_check.h`. If a fact does not hold, or Verilator is another version, the engine builds with the portable two-`eval()`
loop and a compiler warning (`-DVB_SINGLE_EVAL=0` forces it). Results are bit-identical either way.

## The model engine (`engine_model.cpp`)

Wraps `model::Machine` (sim/model/README.md) and reports what the accurate engine reports from the RTL's `cnt_*`
counters, at 50 MHz. It stops where that engine stops (the cycle a VID_FLIP, halt or tohost store becomes visible),
runs the same drain after `halted` (12 UART bit times + 64, +64 with `--isa-test`) and drives key events the same way
(one per cycle from the start of the next slice), so a run gives the same statistics, serial output and frames as on
`accurate`. The window shows the frame at each VID_FLIP, as with `fast`; there are no hardware checkers.

### Cosim (`cosim.h/.cpp`)
For every `trace_valid` cycle the ISS is stepped once and trap flag, pc, insn, rd, rd value, memory address (non-trap), store
strobes, store data (strobed lanes) and next pc are compared. MMIO loads and counter-CSR reads use the ISS override with the
hardware's value (sim/iss/README.md); key events go into the ISS when the RTL receives them. The RTL's `vid_flip` fires before
the ISS executes the flip store, so RTL frame snapshots are queued and compared (RGB) when the ISS flips. Records the RTL
retires after the exit store are ignored. The first mismatch stops the run (status 119) with cycle, instruction number, both
records with `<-- DIFFERS` marks and the last 32 retired instructions. Test hooks: `--cosim-inject N`, `--scanout-inject`.

`--engine accurate --cosim=model` uses the model instead, with no overrides: for every `trace_valid` the model is run to
that cycle and must retire the same instruction in that cycle with all eleven counters equal; frames are compared with
their cycle, serial bytes with the cycle the decoder has them, key events enter the model at the edge the RTL drives
them, and at the end of every slice the model is run to the RTL's cycle and must agree on counters, halt and exit code
without retiring anything more. Same report and status (with a `cycle` row and the counters); `--cosim-inject N` moves
the expected retire cycle of instruction N by one.

## Speed

Per-cycle work is kept to the edge, the device-model ticks and a few byte-wide pin reads. Build choices, each measured
(host instructions and cycles per simulated cycle on demo3):
* Verilator `-fno-dfg -fno-split` (`VL_OPT`): the data-flow optimiser and always-block splitting make this design's code about
  20% longer. Tried without gain: `--threads 2` (thread-pool handshakes double the per-cycle work), `-mcpu=native`, other
  `-fno-*` stages, RTL rewrites of the decoder struct width and of the scan-out flags (they paid only with splitting on).
* Thin LTO of the models, the engines and the SDRAM model, so the scheduling code and the device models inline into the loop.
* PGO as part of the build: when `PGO_ELF` (default `build/doom/soc/doom.elf`) and the WAD exist, an instrumented copy in
  `$(OUT)/pgo-gen` runs `PGO_CYCLES` of `-timedemo demo3` on both RTL engines; `$(OUT)/pgo.profdata` is redone whenever that
  copy changes (RTL, harness, flags). Without Doom the build goes without it (about 15% slower) until the next build after Doom
  exists. `PGO=0` turns it off, `PGO_DATA=FILE` uses a given profile, `make pgo` forces a new training run.
* The device-model ticks are a few percent of the accurate engine; the gap to fast is the accurate RTL itself.
* The PGO training also runs the model. The ISS object stays out of LTO (with it the iss engine's loop executes about
  6% more host instructions).

## Known limitations

* Real keyboard/mouse, pause, quit hotkeys, focus handling and a Doom session with a person at the keyboard were tested only
  with synthetic SDL events (key script) and unattended real-window runs. No audio, no mouse.
* The 8x8 HUD font is small on high-DPI screens (`--scale 2`).
* The scan-out check cannot see a framebuffer/palette write that is undone within one monitor frame.

## Files

| File | Purpose |
|---|---|
| `engine.h` | `Engine` interface, `Stats`, events, `synth_monitor`, factories |
| `engine_iss.cpp` | ISS engine, monitor synthesis, `make_engine` |
| `engine_rtl.cpp` | the Verilator engines (compiled once per model), scan-out check |
| `engine_model.cpp` | the cycle-exact model as an engine |
| `vl_check.py` | build-time proof of what the cycle loop's shortcuts rely on (`vb_model_check.h`) |
| `cosim.h/.cpp` | lockstep comparison against the ISS or the model |
| `main.cpp` | command line, SDL window/events, run loop, stop conditions, summary/JSON |
| `hud.h/.cpp`, `gfx.h/.cpp`, `font8x8.h`, `keymap.h/.cpp` | metrics and instrument panel; canvas and text; SDL key -> Doom code |
| `Makefile`, `tests/tests.mk` | build (with PGO), `test`, `test-long` |
| `tests/test_cli.sh` | end-to-end tests on the ISS engine (headless, isa-test, stops, boot-info, video, window, keyboard, throughput) |
| `tests/test_rtl.sh` | tests on the RTL engines (riscv-tests, programs, frame dumps, scan-out check, keyboard, cosim report, Doom) and on the model (the same under `--cosim=model`, statistics equal to accurate) |
| `tests/test_host.cpp`, `tests/lib.sh` | unit tests and DFRM / screenshot decoding checks; shared shell helpers |
| `tests/{hello,args,spin,video,videoloop,kbd,bench,fault,tohost,corners}.c`, `crt.S`, `link.ld`, `t.h` | target programs (`corners`: timing corners for `--cosim=model`) |
