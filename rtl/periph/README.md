# rtl/periph, rtl/soc - MMIO peripherals and the SoC top

All registers are 32-bit and word aligned. Reads have no side effect except `KBD_DATA`. Unmapped addresses
read 0 and ignore writes. Register writes ignore `wstrb` (`sb` to UART_TX works because the byte sits in
lane 0); only the framebuffer (all four lanes) and the palette (lanes 0-2) honour byte strobes.
Source of truth: `docs/ARCHITECTURE.md` section 7.

## Slave behaviour (`mmio.sv`)
No wait states: `rsp_ready` is a flop, 0 during reset, 1 from the first cycle after it. A request accepted
at edge E is performed at E (pop, FIFO push, UART start, ...); `rsp_rdata` is a register loaded only at an
accept edge (read data for a load, 0 for a store), so it is held until the cycle after the next accept and
never depends on `req_*`. Decode: `addr[31:16]==0x1000` selects 4 KB blocks by `addr[15:12]`
(0 UART, 1 SYS, 2 KBD, 3 VID); `addr[31:16]==0x1001` is the framebuffer.

| Address | Name | Acc | Behaviour |
|---|---|---|---|
| 0x1000_0000 | UART_TX | W | bits [7:0] start a frame if idle; ignored if busy; reads 0 |
| 0x1000_0004 | UART_STATUS | R | bit0 tx_ready (0 from the cycle after an accepted write until the stop bit has ended) |
| 0x1000_1000 | SYS_CLK_HZ | R | `CLK_HZ` |
| 0x1000_1004 | SYS_EXIT | W | `halted` <= 1 (sticky), `exit_code` <= wdata (a later write replaces the code); reads 0 |
| 0x1000_1008 | SYS_FLAGS | R | bit0 = `FAST` |
| 0x1000_2000 | KBD_DATA | R | pops: `{1, 22'b0, pressed, code}` or 0 when empty |
| 0x1000_2004 | KBD_STATUS | R | bit0 = event waiting |
| 0x1000_3000 | VID_FLIP | W/R | write: `vid_flip` pulse (cycle after accept), counter+1; read: counter |
| 0x1000_3004 | VID_STATUS | R | bit0 = scan-out outside lines 40..439 (always 1 if FAST); [31:16] vsync counter (0 if FAST) |
| 0x1000_3400 + 4i | VID_PALETTE | W | entry i (0..255), lanes 0-2 = B,G,R; lane 3 ignored |
| 0x1001_0000 + 4w | VID_FB | W | words 0..15999 (64 000 bytes), lane b = pixel 4w+b; stores at/after 0x1001_FA00 ignored |

UART: 8N1, `UART_DIV` clk per bit, start bit begins in the cycle after the accept, line idle high.
`uart_tx_valid`/`uart_tx_byte` are registered: valid for one cycle, the cycle after the accept.
Keyboard: 16-entry FIFO; a push while full is dropped, even when a pop happens at the same edge.

## Video pipeline (FAST=0)
`vid_timing` counters `hcnt` 0..799, `vcnt` 0..524 advance on `ce` (toggles every clk). Sync active low:
hsync low for hcnt 656..751, vsync low for vcnt 490..491; `de` = hcnt<640 && vcnt<480.

| Stage | Contents (all stages free-run on clk) |
|---|---|
| S0 | counters (VID_STATUS reads these: vblank; the frame counter steps when vcnt reaches 490) |
| S1 | col = hcnt/2, row = (vcnt-40)/2, fb-window / de / hs / vs flags |
| S2 | byte address = row*320 + col (0 outside the window) |
| S3 | four framebuffer lane RAMs read (word = addr[15:2]); lane = addr[1:0] |
| S4 | lane mux -> palette index -> palette RAM read |
| S5 | output flops: rgb (0 outside the window, so letterbox lines are black even if palette[0] is not), de, hsync, vsync, `vid_pix_ce` |

The pins lag the counters by 5 clk, all equally, so sync/de/colour stay aligned. `vid_pix_ce` is high in
the first of the two cycles a pixel is on the pins (sample on it); the first pixel after reset gets its
pulse 5 clk after reset release. The VID_STATUS frame counter (16 bit, vsync assertions since reset, not
frames displayed) therefore steps 5 clk before the vsync falling edge on the pins, and bit0 changes 5 clk
before the first pixel of line 40 / 440. With FAST=1 no timing generator or pipeline is built:
`vid_pix_ce`=0, syncs=1, `de`=0, rgb=0, VID_STATUS reads 1.

## soc_top (`rtl/soc/soc_top.sv`)
Core + memsys + mmio glue and the eleven 64-bit event counters. `addr[31]` splits the data port: 1 = RAM
(memsys), 0 = MMIO (an unmapped address with bit 31 clear reads 0 from MMIO). Only the selected slave sees
`req_valid`; `dmem_rsp_rdata` is chosen by `sel_ram_q`, a flop loaded with `addr[31]` at every accept, and
`dmem_rsp_ready = memsys.d_rsp_ready & mmio.rsp_ready`. This relies on the contract rule that an idle slave
holds ready=1, and it keeps the core waiting while either slave is still initialising after reset.
`memsys` is not given `LINE_BYTES` (keeps its default).

## For the board author
* The arrays are `public_flat_rw`; with FAST=1 nothing reads them in RTL, so the board must. In a model whose
  top is `soc_top` they are `top->rootp->soc_top__DOT__u_mmio__DOT__u_video__DOT__{fb0..fb3,palette}`
  (replace the `soc_top` prefix with `mmio` when `mmio` is the top; header `V<prefix>___024root.h`):
  `fb0..fb3` are 16000 x 8 bit, `palette` is 256 x 24 bit (0x00RRGGBB). Pixel (x, y) is index `y*320+x` =
  lane `(y*320+x)&3`, word `(y*320+x)>>2`.
* A snapshot taken in the cycle `vid_flip` is seen includes every store the core issued before the VID_FLIP
  write (stores are performed at their accept edge, in order).
* Models (`sim/models/`, header-only): `UartDecoder::tick(uart_tx)` and
  `VideoMonitor::tick(ce,hs,vs,de,r,g,b)` once per clock, sampling the pins after `eval()` of the falling
  edge (post-edge values). `VideoMonitor` publishes a frame at the next vsync falling edge.
* Yosys `synth_gowin` of `mmio` (FAST=0): 32 DPB block RAMs for the framebuffer (8 per lane); the palette is
  distributed RAM (96 RAM16SDP4) and all five memories carry `no_rw_check`, so Yosys adds no
  read-during-write logic (a CPU store and the scan-out read of the same word in one cycle may give either
  value for one pixel of one frame). Current numbers: `docs/results/fpga.md`.

## Files and tests
`uart_tx.sv` transmitter, `sysctl.sv` SYS_* registers, `kbd.sv` keyboard FIFO, `vid_timing.sv` counters /
sync decode / frame counter, `video.sv` VID_* registers + palette + framebuffer + scan-out, `mmio.sv` decode,
response register and instantiation of the above.

`make -C tests/periph test`: lint (`mmio`, and `soc_top` with the real core and memsys; FAST=0/1), `tb_mmio`
FAST=0/1 (registers, UART timing, keyboard FIFO, framebuffer/palette strobes against a shadow, R3/R5 contract
checked every cycle), `tb_video` (whole 640x480 frames, timing, VID_STATUS), `tb_soc` FAST=0/1 (data-port
split, response ownership, event counters). `tb_soc` replaces the core and memsys by the scripted stand-ins in
`tests/periph/stubs/behav` (test-only, not the real modules). `make -C tests/periph synth` prints the Yosys
cell counts of `mmio`.
