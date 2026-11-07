#!/bin/sh
# CLI-level tests of iss-run.  usage: test_cli.sh iss-run progdir isadir outdir
RUN=$1; P=$2; ISA=$3; OUT=$4
fail=0; n=0
ok()   { n=$((n+1)); }
bad()  { n=$((n+1)); fail=$((fail+1)); echo "FAIL: $1"; }
expect_status() { # name want got
    if [ "$3" -eq "$2" ]; then ok; else bad "$1: exit status $3, wanted $2"; fi
}

# 1. riscv-tests
pass=0; total=0
for e in "$ISA"/*.elf; do
    total=$((total+1))
    if "$RUN" --isa-test --max-insns 10000000 "$e" 2>"$OUT/err.txt"; then pass=$((pass+1)); else bad "riscv-test $(basename "$e"): $(cat "$OUT/err.txt")"; fi
done
echo "riscv-tests: $pass/$total passed"
[ "$total" -eq 56 ] && ok || bad "expected 56 riscv-test ELFs, found $total"
[ "$pass" -eq "$total" ] && ok

# 2. a failing riscv-test style program must be reported: run exit42.elf in --isa-test mode -> no tohost symbol
"$RUN" --isa-test "$P/exit42.elf" 2>/dev/null; expect_status "isa-test without tohost" 126 $?

# 3. exit status + UART
out=$("$RUN" "$P/exit42.elf" 2>/dev/null); st=$?
expect_status "exit42" 42 $st
[ "$out" = "ok" ] && ok || bad "exit42 stdout was '$out'"

# 4. directed programs that self-check and exit 0
for t in mmio traps video; do
    "$RUN" "$P/$t.elf" 2>"$OUT/err.txt"; st=$?
    if [ $st -eq 0 ]; then ok; else bad "$t exited $st (check number = exit status): $(cat "$OUT/err.txt")"; fi
done

# 5. boot-info with and without WAD
mkdir -p "$OUT/waddir"; cp "$OUT/test.wad" "$OUT/waddir/test.wad"
"$RUN" --wad "$OUT/waddir/test.wad" "$P/bootinfo.elf" -- foo -bar baz; expect_status "bootinfo+wad" 0 $?
"$RUN" "$P/bootinfo.elf" -- N x; expect_status "bootinfo nowad" 0 $?
"$RUN" --wad /nonexistent.wad "$P/bootinfo.elf" 2>/dev/null; expect_status "missing wad" 126 $?

# 6. faults: distinct status, one-line reason with pc
for c in l s r f n; do
    err=$("$RUN" "$P/fault.elf" -- $c 2>&1 >/dev/null); st=$?
    expect_status "fault $c" 125 $st
    case "$err" in *"pc=0x"*) ok;; *) bad "fault $c: no pc in message '$err'";; esac
    [ "$(printf '%s\n' "$err" | wc -l)" -eq 1 ] && ok || bad "fault $c: message not one line"
done

# 7. --max-insns, --trace, --stats
"$RUN" --max-insns 1000 "$P/bench.elf" -- 100000000 2>"$OUT/err.txt"; st=$?
expect_status "max-insns" 124 $st
grep -q "pc=0x" "$OUT/err.txt" && ok || bad "max-insns message lacks pc"
"$RUN" --max-insns 100 --trace "$OUT/trace.txt" "$P/bench.elf" -- 100000000 2>/dev/null; expect_status "trace run" 124 $?
[ "$(wc -l < "$OUT/trace.txt" | tr -d ' ')" -eq 100 ] && ok || bad "trace should have 100 lines"
"$RUN" --stats "$P/mmio.elf" 2>"$OUT/err.txt"; grep -q "MIPS" "$OUT/err.txt" && ok || bad "--stats prints no MIPS"

# 8. frame dump: video.elf presents 3 frames
"$RUN" --dump-frames "$OUT/frames.dfrm" "$P/video.elf"; expect_status "dump-frames" 0 $?
sz=$(wc -c < "$OUT/frames.dfrm" | tr -d ' ')
[ "$sz" -eq $((16 + 3 * 64768)) ] && ok || bad "frame dump size $sz, wanted $((16 + 3 * 64768))"

# 9. usage errors
"$RUN" 2>/dev/null; expect_status "no args" 2 $?
"$RUN" --bogus x.elf 2>/dev/null; expect_status "bad option" 2 $?
"$RUN" /nonexistent.elf 2>/dev/null; expect_status "missing elf" 126 $?
echo "x" > "$OUT/notelf.bin"; "$RUN" "$OUT/notelf.bin" 2>/dev/null; expect_status "not an elf" 126 $?

echo "cli tests: $((n-fail))/$n passed"
[ $fail -eq 0 ]
