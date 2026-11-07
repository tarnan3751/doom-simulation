# sw/doom - Doom (doomgeneric) for the SoC, and its native reference

One patched source tree, two platforms. The native build (`doom-ref`) produces the reference frames;
the SoC build (`doom.elf`) must reproduce them pixel for pixel. The SoC build adds performance patches
of its own on a copy of the tree, which the native reference never sees.

## Build

```
make -C sw/doom soc                 # build/doom/soc/doom.elf (needs sw/libc and sw/bsp: make -C sw libs)
make -C sw/doom native ref          # build/doom/native/doom-ref and build/doom/ref/demo{1,2,3}.dfrm
make -C sw/doom test                # FixedDiv of the SoC build vs upstream's expression, bit for bit
make -C sw/doom native-variants check-determinism sanitize sheets   # determinism tooling (below)
build/doom/native/doom-ref --dump-frames ref.dfrm -iwad third_party/wad/doom1.wad -timedemo demo1
build/tools/framecmp ref.dfrm other.dfrm            # exit 0 only if identical; -o PREFIX writes PPMs
build/tools/dfrm2ppm f.dfrm --every 200 --sheet 6 --down 2 -o sheet.ppm   # or --frame N, --info
```

1. `build/doom/src`: upstream `third_party/doomgeneric/doomgeneric` (`.c/.h` without the
   `doomgeneric_<port>.c` files) + `patches/*.patch` in name order; `third_party` is never edited.
2. `build/doom/src-soc`: that tree + `patches-soc/*.patch`. The SoC build compiles it through
   `sw/common.mk` (the Makefile's `SOC_LINK` pass) with `-O2 -mtune=size` (`RV_OPT`) and
   `doomgeneric_soc.c`; the native build compiles `build/doom/src` with the host compiler and
   `doomgeneric_ref.c`.
3. Both compile with the same Doom-affecting flags: `-DCMAP256` (8-bit indices + separate palette),
   320x200, `-fsigned-char` (arm64 `char` is signed, RISC-V's is not, and ticcmds/demo bytes are signed
   data), `-fwrapv` (the fixed-point code relies on wrap-around; gcc and clang agree on it when told),
   `-fno-strict-aliasing`, `-ffp-contract=off`, `-w` (third-party code). Sound is off by construction
   and config files are compiled out upstream (`#if ORIGCODE`).

## Shared patches (`patches/`: both builds, correctness only)

| patch | what |
|---|---|
| 0001-no-host-services | I_Error without zenity/`system()`; no `mkdir`; a failed savegame write is not fatal |
| 0002-timedemo-clean-exit | a finished `-timedemo` prints "timed N gametics in M realtics (F fps)" (integer arithmetic) and exits 0; errors still exit 255 |
| 0003-wipe-singletics | the melt wipe advances one tic per presented frame under `singletics` and never reads the clock |
| 0004-zone-defined-contents | zone blocks are zeroed and get 256 zero bytes of tail pad (see Determinism) |
| 0005-cmap256-palette-index | `I_GetPaletteIndex` uses the live palette |
| 0006-screenbuffer-size | `DG_ScreenBuffer` sized for 8-bit pixels |
| 0007-drop-unused-host-headers | no `<fcntl.h>`, `<sys/types.h>` |

## SoC patches (`patches-soc/`: performance, every frame unchanged)

| patch | what and why |
|---|---|
| 0001-present-videobuffer | `I_FinishUpdate` only calls `DG_DrawFrame`, which copies `I_VideoBuffer` to the framebuffer itself: one frame copy through the D-cache instead of two that evict each other. `DG_ScreenBuffer` is not allocated. |
| 0002-draw-loops | `R_DrawColumn`/`R_DrawSpan`, half of all cycles: the globals are read once (a byte store may alias them, so GCC reloaded them per pixel), odd pixels first then four per iteration, `frac` kept `<< 9` so the texel index `(frac >> 16) & 127` is one shift (exact modulo 2^32), destination `viewbase + y*320 + x` instead of two table reads. |
| 0003-fixed-point | `FixedDiv` without libgcc's `__divdi3`: after upstream's overflow test the quotient has at most 30 bits, so an integer divide plus one Knuth base-2^16 digit (two 32-bit divides) give it exactly; an empty asm keeps GCC from turning `a - q*b` back into a 33-cycle `remu`; `a == INT_MIN` keeps the 64-bit expression. `FixedMul` is inline. `tests/fixed_test.c` checks it against upstream for edge values, all their pairs, the overflow boundary and random inputs. |
| 0004-noreturn-ierror | `I_Error` (which always exits) is declared noreturn: no stack frames for the RANGECHECK paths. |
| 0005-planes-skip-unchanged | `R_DrawPlanes` calls `R_MakeSpans` only where a column's top/bottom differ from the previous one (otherwise it does nothing). |
| 0006-segloop-locals, 0007-masked-column-locals | `R_RenderSegLoop` and `R_DrawMaskedColumn` keep their state in locals: the column drawers change none of it, but GCC reloaded and stored every global around every call. |

### Cache placement (`soc-layout.ld`, `--section-start=.sdata=0x80073000`)

The caches are direct-mapped (I 8 KB, D 16 KB, 32-byte lines), so where code and data sit decides
whether they evict each other. Before, the renderer's hot globals (`.sbss`) shared D-cache sets 365-383
with the stack frames of the renderer and caused almost half of all D-cache misses; and every code change
moved every global, swinging the cycle count by a few percent. Now `.sdata` starts at a fixed address
that maps to set 384, just above the stack's hot frames; the ordering file puts the hot scalars first in
`.sdata`/`.sbss` and the small per-column/per-span arrays first in `.bss`, so about 14 KB of hot static
data never conflicts with itself or the stack. In `.text` each rendering phase is contiguous: walls and
the BSP walk in one block under 8 KB, planes and sprites mapped onto the BSP walk (idle by then) rather
than onto `R_DrawColumn`/`R_GetColumn`/`W_CacheLumpNum`, which every phase uses, then the tic logic.
The link fails (overlapping sections) if `.data` ever grows into 0x8007_3000.

What remains is structural for a direct-mapped write-allocate D-cache: walls are drawn as columns, so
the screen lines of the current 32-column group fill a third of the sets and every texture or table line
a column touches may evict one of them. Tried and dropped: LTO (fewer instructions, deeper stack frames,
no gain), -O3 (much more code for under 1 %), other `-mtune` values, more locals in `R_RenderSegLoop`
(GCC spills them), an exact reciprocal instead of the `dc_iscale` divide (not faster than 33 cycles).
Not attempted: skipping unchanged status-bar numbers (vanilla redraws them every frame and menus can
draw over the bar), caching `W_CacheLumpNum` in `R_GetColumn` (needs zone-tag checks to stay exact).

## Platform layers

* `doomgeneric_soc.c`: the palette goes to `VID_PALETTE` (0x00RRGGBB) when it changed (always on the
  first frame: the palette RAM is not reset); `I_VideoBuffer` is copied to `VID_FB` with 16 000 word
  stores; `VID_FLIP` last, so the board's snapshot and the scan-out only see complete frames. Time: the
  low 32 bits of `rdcycle`, one 32-bit divide per call, remainder carried (exact for polling intervals
  under 85 s at 50 MHz); the clock starts at 1 ms because `i_timer.c` treats 0 as unset. Keys: one
  `KBD_DATA` read per call. `main` appends `-iwad <boot-info wad_name>` when there is none.
* `doomgeneric_ref.c`: headless, as fast as possible, virtual clock (advances only by `DG_SleepMs` and
  one tic per presented frame), no input. `--dump-frames FILE` writes palette + `DG_ScreenBuffer` at
  every `DG_DrawFrame` (the same point where the SoC writes `VID_FLIP`) as DFRM; `--max-frames N`.
  Exit 2: cannot write the dump; 3: `--max-frames` reached.

## Determinism

Everything that could consult the clock, randomness or undefined memory was audited: the timedemo path
reads the clock only for its report line (`singletics`: one `BuildNewTic` per call, the wipe is patch
0003), `P_Random`/`M_Random` are table driven and called in a fixed order, menu/HUD timers count tics,
there is no `rand`/`time`. The one real problem was a read past the end of a column: the drawers index
`dc_source[(frac >> 16) & 127]`, and the first pixel of a short sprite column can use index 127, beyond
the cached lump, i.e. in the next zone block's header (host pointers, a different layout on RV32) - one
pixel of demo2 frame 2363 changed from run to run. Patch 0004 makes those bytes zeros everywhere.

`check-determinism` (two O2 runs and an O0 run per demo) gives byte-identical dumps; so did a UBSan
build (no `-fwrapv`; 43 benign two's-complement and declared-size-array sites, none affects pixels),
`MallocScribble`, `-ftrivial-auto-var-init=pattern`, zone sizes `-mb 2..16` and perturbed zone layouts.
ASan could not run in the development sandbox. Zone: `DEFAULT_RAM 6` MiB; all demos play identically
down to `-mb 2`.

Remaining risks: out-of-bounds or uninitialised reads that these three demos do not reach; gcc/clang
agreement on the 43 UBSan sites is documented two's-complement behaviour, not proven. The timedemo's
"realtics"/fps line counts the platform's clock and is not comparable between the two builds.
Interactive play is unchanged by the patches but not claimed deterministic.
