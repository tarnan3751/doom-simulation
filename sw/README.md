# sw/ - target software: board support, C library, programs

Everything that runs on the SoC: `bsp/` (crt0, link script, console, clock, exit, trap report), `libc/`
(a small freestanding C library), the programs `hello/`, `testpattern/`, `coremark/` and `doom/` (its own
README), and `common.mk`, the build fragment they all use. `include/soc.h` is the register map
(docs/ARCHITECTURE.md sections 2 and 7, bit for bit).

## Commands

```
make -C sw all          # libc.a, libbsp.a + crt0.o, hello, testpattern, coremark(+quick), libctest -> build/sw/
make -C sw test         # host unit tests of the libc (host libc as oracle) and Doom's fixed_test
make -C sw run-iss      # every program on build/iss/iss-run: exit status + key output line
make -C sw/coremark CM_ITERATIONS=N   # iteration count of the full CoreMark run (default 2500)
build/iss/iss-run build/sw/hello/hello.elf -- trap-load   # also trap-store trap-null trap-illegal
                        # trap-ebreak trap-ecall trap-jump abort stack-overflow
```

Products: `build/sw/<prog>/<prog>.elf` and `.map` (`make -C sw/<prog> dis` adds a listing).

## Building a program

```make
PROG := myprog                 # -> build/sw/myprog/myprog.elf
SRCS := main.c util.S          # relative to this Makefile, or absolute
include $(dir $(lastword $(MAKEFILE_LIST)))../common.mk
```

Optional: `OPT` (default `-O2`), `PROG_CFLAGS`, `PROG_LDFLAGS`, `PROG_LIBS`, `STACK_SIZE` (default 256 KB),
`OUT`, `OBJDIR`. Objects are named after the source's base name. Programs are compiled as hosted C
(GCC builtins on) with `-march=rv32im_zicsr -mabi=ilp32 -mstrict-align -std=gnu11` - the core traps on
misaligned accesses, so `-mstrict-align` is never optional - and linked `-nostdlib -nostartfiles
-T bsp/link.ld --gc-sections` with crt0.o, libc.a, libbsp.a and libgcc in one `--start-group` (libc calls
the BSP hooks and vice versa). Objects depend on the compiler flags through a stamp.

## Board support (bsp/)

`_start` (crt0.S): gp, sp = 0x807F_F000, trap vector, a 2 KB trap stack in mscratch, `.sbss`/`.bss`
cleared, the whole stack reserve painted with 0x57ACC0DE, `__bsp_init()` (argv, WAD, init arrays), `main`,
`exit`. The link script puts the stack reserve (`__stack_size`) directly below 0x807F_F000 and the heap
between the end of `.bss` and the reserve; the link fails if they do not fit. A program may move its small
data with linker options (Doom does, see doom/README.md); do not give `.sdata` a large `ALIGN()` in the
script instead: GNU ld then counts that alignment against the 2 KB gp range and relaxes nothing to
gp-relative addressing (+12.6 KB of code in Doom).

* argv: `argv[0] = "prog"`, then the boot-info `cmdline` split at spaces, tabs and newlines, no quoting.
* WAD: registered as a read-only memfile under `wad_name` (an empty name becomes `doom1.wad`; a WAD
  outside RAM is ignored with a console note). `fopen` matches it case-insensitively against the last
  path component, so Doom's `./doom1.wad` probing works.
* Console: the BSP polls `tx_ready` before every byte and drains the UART before writing `SYS_EXIT`.
* Exit codes of the BSP itself (distinct in the low byte, which is what a host process status keeps):
  0xE0 + mcause for an unexpected exception, 0xDE trap inside the trap handler, 0xDF stack overflow found
  at exit, 134 abort()/assert.
* Trap report (trap.c, straight to the UART): cause in words, mepc and the instruction, mtval with a hint
  (NULL+offset, below the stack, misaligned, MMIO, wild pointer, pc outside the code), all registers,
  stack use, return-address candidates and a ready-made `riscv64-elf-addr2line` line.
* Stack overflow is detected by paint (no MPU): the bottom 1 KB of the reserve must keep it; checked at
  exit, by `bsp_stack_check()` and in the trap report. A frame with a large unwritten local array can step
  over the guard undetected. NULL is the MMIO window: on the hardware a NULL read returns 0 and a write is
  ignored; only the ISS stops on it.

## The C library (libc/)

It contains what the programs in this repository use, and nothing else: string and memory functions,
the ctype tables, malloc, printf/fprintf/snprintf with exact floating point, sscanf (integers, strings,
scansets), strtol/strtoul/atoi, read-only memory files behind fopen/fread/fseek, exit/atexit/abort/assert.
A missing function shows up as a link error. Every public name is declared in the usual header; `atof`
is declared but not defined, because doomgeneric's `m_config.c` calls it in code the link discards.
`math.h` declares only `fabs`, which GCC expands inline; no program does floating-point input.
The platform hooks are in `libc_port.h` (console write, heap region, exit); `memfile.h` and `malloc.h`
(`malloc_get_stats`, `malloc_check`, used by the tests) are the extensions.

* **mem/str**: memcpy/memmove align the destination and copy 8 words per iteration, merging two aligned
  source words with shifts when source and destination are not co-aligned; nothing ever does a misaligned
  access or reads a word that holds no copied byte. memset, memcmp, memchr and strlen work a word at a
  time. The library is built `-ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns` so GCC
  cannot turn these loops back into calls to themselves.
* **malloc**: one fixed region from the BSP, boundary tags (one header word per allocated chunk, footers
  only in free chunks), 16-byte aligned payloads, O(1) coalescing, segregated free lists, first fit in the
  smallest class, in-place realloc when possible. Out of memory returns NULL + ENOMEM; double free,
  foreign pointers and overwritten headers abort with a message.
* **printf**: one formatter behind every variant; integers use 32-bit division whenever the value fits;
  doubles are expanded exactly (base-10^9 bignum, fpconv.c) and rounded half-to-even at the last digit,
  so %f/%e/%g match glibc and macOS digit for digit. No long double (`L`).
* **FILE**: stdout/stderr go to the console unbuffered (fflush is a no-op). fopen in a read mode works on
  registered memfiles; unknown names fail with ENOENT, write/append/update modes with EROFS, and
  remove/rename always fail: the machine has no storage. 16 streams.

Deliberate behaviour choices: `%a`/`%A` print as `%e`/`%E`; NaN keeps its sign and `+`/space flags like
glibc (`-nan`); `realloc(p, 0)` frees and returns NULL; scanf scans `"0x"` without hex digits as 0 and
returns EOF on an input failure before any assignment (glibc); strto* leave errno alone when nothing was
converted (ISO C, glibc); ctype is "C" locale only, bytes 128-255 have no class.

## Tests

* `make -C sw/libc test` (part of `make -C sw test`): the library sources are compiled natively with
  clang against our headers, every public name renamed to `tl_<name>` (`HOST_RENAME` in the Makefile;
  `nm` checks that nothing escaped), so the test binary links our library next to the host libc and
  compares the two: printf (a shared case table, every flag/width/precision/length combination on edge
  and random values, floats down to `%.1074f`), strtol/strtoul, sscanf (40 000 random inputs x 17
  formats, scansets, %n), mem*/str* at all alignments and lengths, the ctype tables, 300 000 random
  malloc/calloc/realloc/free operations with per-byte checks and heap walks, exhaustion and misuse, the
  memfile FILE layer, exit/atexit/abort/assert. The hooks (`tests/host_harness.c`) capture the console,
  give malloc a misaligned 64 MB region and turn `__libc_exit` into a longjmp.
* `libctest.elf`: the same areas on the target against expected values and reference loops, plus a few
  cycle counts; exit status = failures. The RTL regression (sim/vboard/tests) runs it under `--cosim`.
* `make -C sw run-iss`: hello (self-checks), the trap and abort paths, testpattern, libctest, CoreMark.

## Programs

* **hello**: printf self-check against a known string, cycle/instret counters, argv, boot info, malloc,
  stack use; the fault modes listed under Commands.
* **testpattern** `[frames [ms_per_frame]]`: palette, border, corner squares, colour bars, the strip of
  all 256 indices, byte-lane groups, checkerboards and a moving box, drawn in RAM and copied with word
  stores; exits 1 if the VID_FLIP counter did not advance by exactly the number of frames.
* **coremark**: upstream sources unmodified + `core_portme.[ch]` (data on the stack, time from the 64-bit
  cycle counter); adds CoreMark/MHz, cycles and instructions per iteration and IPC to the report; exits 1
  on a CRC error. Flags: the set SiFive publishes for its in-order cores (`-finline-limit=600` and
  `-funroll-all-loops` do most of the gain over plain -O2; -O3, LTO and `-mtune=size` were slower).
  `coremark-quick` runs 20 iterations (CoreMark then prints its "at least 10 secs" error; CRCs still
  validated, exit 0).

## Known limitations

No file descriptors, no writable files, no stdin, no locale or wide characters, no math library, no
floating-point input, no setjmp/longjmp, no threads. `Total ticks` in CoreMark's report is printed with
`%lu` (32 bits on RV32): it would wrap for runs longer than 85 s at 50 MHz; the computed times use all
64 bits.
