#!/bin/sh
# vboard tests on the Verilator engines (fast, accurate) under --cosim, and on the model (--cosim=model, --engine model).
# usage: test_rtl.sh VBOARD TEST_HOST PROGDIR ISADIR OUTDIR short|long
VB=$1; TH=$2; P=$3; ISA=$4; OUT=$5; MODE=${6:-short}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SW=$ROOT/build/sw; DOOM=$ROOT/build/doom/soc/doom.elf; WAD=$ROOT/third_party/wad/doom1.wad
REF=$ROOT/build/doom/ref/demo3.dfrm; FRAMECMP=$ROOT/build/tools/framecmp
T=$OUT/work_rtl
rm -rf "$T"; mkdir -p "$T"
. "$(dirname "$0")/lib.sh"
need() { [ -f "$1" ] && ok || bad "missing input $1 (build it: make -C sw all; make -C sw/doom soc)"; }
# serial output with timing-dependent content removed: the build name is normalised, lines that print
# cycle counts, times or rates are dropped (they legitimately differ between ISS, fast and accurate)
norm() { sed -E 's/(FAST|accurate) build/BUILD/' "$1" | grep -v -i -E 'cycles|Total ticks|Total time|Iterations/Sec|CoreMark/MHz| us since|ticks'; }
[ -x "$FRAMECMP" ] || make -s -C "$ROOT/tools" >/dev/null 2>&1
for f in "$SW/hello/hello.elf" "$SW/libctest/libctest.elf" "$SW/coremark/coremark-quick.elf" "$SW/testpattern/testpattern.elf" "$DOOM" "$WAD" "$REF" "$FRAMECMP"; do need "$f"; done

echo "== riscv-tests with --cosim, fast and accurate"
for e in fast accurate; do
    n=0
    for f in "$ISA"/rv32*.elf; do
        [ -f "$f" ] || continue
        n=$((n+1))
        "$VB" --engine $e --headless --cosim --isa-test --quiet-stats "$f" >/dev/null 2>"$T/err"; rc=$?
        [ $rc -eq 0 ] && ok || { bad "$e isa-test $(basename "$f") exit $rc"; head -3 "$T/err"; }
    done
    [ $n -ge 56 ] && ok || bad "$e: only $n riscv-tests found"
    echo "   $e: $n riscv-tests"
done
"$VB" --engine fast --headless --isa-test --quiet-stats "$P/tohost_fail.elf" >/dev/null 2>"$T/err"; check_eq "fast tohost=5 fails" $? 1
grep -q "FAILED test 2" "$T/err" && ok || bad "fast: failure message"
"$VB" --engine accurate --headless --isa-test --quiet-stats "$P/tohost_pass.elf" >/dev/null 2>&1; check_eq "accurate tohost=1 passes" $? 0

echo "== programs under --cosim: exit 0, serial equal to the ISS run"
for p in hello:$SW/hello/hello.elf libctest:$SW/libctest/libctest.elf coremark-quick:$SW/coremark/coremark-quick.elf; do
    name=${p%%:*}; elf=${p#*:}
    "$VB" --engine iss --headless --quiet-stats "$elf" > "$T/$name.iss" 2>/dev/null; check_eq "$name iss exit" $? 0
    norm "$T/$name.iss" > "$T/$name.iss.n"
    for e in fast accurate; do
        "$VB" --engine $e --headless --cosim --quiet-stats --stats-json "$T/$name.$e.json" "$elf" > "$T/$name.$e" 2>"$T/err"; rc=$?
        check_eq "$name $e exit" $rc 0
        [ $rc -eq 0 ] || head -40 "$T/err"
        norm "$T/$name.$e" > "$T/$name.$e.n"
        cmp -s "$T/$name.iss.n" "$T/$name.$e.n" && ok || { bad "$name $e: serial output differs from the ISS"; diff "$T/$name.iss.n" "$T/$name.$e.n" | head; }
        [ "$(wc -l < "$T/$name.iss.n")" -ge 3 ] && ok || bad "$name: reference output suspiciously short"
        [ "$(json "$T/$name.$e.json" cosim_instructions)" -gt 100000 ] && ok || bad "$name $e: cosim compared too few instructions"
    done
    echo "   $name ok ($(json "$T/$name.fast.json" cosim_instructions) instructions compared on fast)"
done
check_eq "accurate: zero SDRAM violations (libctest)" "$(json "$T/libctest.accurate.json" sdram_violations)" 0
check_eq "accurate: zero serial errors (libctest)" "$(json "$T/libctest.accurate.json" serial_errors)" 0

echo "== testpattern: frame dumps identical on iss, fast, accurate"
for e in iss fast accurate; do
    c=; [ $e != iss ] && c=--cosim
    "$VB" --engine $e --headless $c --dump-frames "$T/tp.$e.dfrm" --stats-json "$T/tp.$e.json" --quiet-stats "$SW/testpattern/testpattern.elf" >/dev/null 2>&1
    check_eq "testpattern $e exit" $? 0
done
"$FRAMECMP" -q "$T/tp.iss.dfrm" "$T/tp.fast.dfrm" >/dev/null && ok || bad "testpattern: fast frames differ from iss"
"$FRAMECMP" -q "$T/tp.iss.dfrm" "$T/tp.accurate.dfrm" >/dev/null && ok || bad "testpattern: accurate frames differ from iss"
[ "$(json "$T/tp.accurate.json" frames)" -ge 3 ] && ok || bad "testpattern: too few frames"
check_eq "testpattern accurate scan-out mismatches" "$(json "$T/tp.accurate.json" scan_bad)" 0

echo "== static image: scan-out self-check actually checks frames; screenshots show the pattern"
for e in fast accurate; do
    "$VB" --engine $e --headless --cosim --dump-frames "$T/vh.$e.dfrm" --screenshot "$T/vh.$e.ppm" --stats-json "$T/vh.$e.json" --quiet-stats "$P/video_hold.elf" > "$T/out" 2>"$T/err"
    check_eq "video_hold $e exit" $? 0
    check_eq "video_hold $e serial" "$(cat "$T/out")" "video done"
    "$TH" frames "$T/vh.$e.dfrm" 5 >"$T/th.txt" && ok || { bad "video_hold $e: dump differs from the drawn pattern"; cat "$T/th.txt"; }
    "$TH" shot "$T/vh.$e.ppm" 4 >"$T/th.txt" && ok || { bad "video_hold $e: screenshot geometry"; cat "$T/th.txt"; }
done
chk=$(json "$T/vh.accurate.json" scan_checked)
[ "$chk" -ge 2 ] && ok || bad "accurate scan-out check compared only $chk frames"
check_eq "accurate scan-out mismatches" "$(json "$T/vh.accurate.json" scan_bad)" 0
check_eq "accurate video timing errors" "$(json "$T/vh.accurate.json" video_errors)" 0
echo "   accurate: $chk monitor frames checked, $(json "$T/vh.accurate.json" scan_skipped) skipped"
cp "$T/vh.accurate.ppm" "$OUT/screenshot-accurate-pattern.ppm"
sips -s format png "$OUT/screenshot-accurate-pattern.ppm" --out "$OUT/screenshot-accurate-pattern.png" >/dev/null 2>&1
"$VB" --engine accurate --no-check-scanout --headless --quiet-stats --stats-json "$T/nc.json" "$P/video_hold.elf" >/dev/null 2>&1
check_eq "--no-check-scanout: nothing checked" "$(json "$T/nc.json" scan_checked)" 0

echo "== keyboard through the RTL (key-script -> SDL events -> key_valid/key_data -> KBD_DATA)"
KS='20000:+Left,25000:+Left Ctrl,30000:-Left,35000:-Left Ctrl,40000:+A,45000:-A,50000:+Space,55000:-Space,60000:+Left Shift,62000:+Right Shift,65000:-Left Shift,66000:-Right Shift,70000:+F5,75000:-F5,80000:+Return,85000:-Return,86000:+F12,87000:-F12,90000:+Q,95000:-Q'
printf "$KEYS_EXPECTED" > "$T/kexp"
for e in fast accurate; do
    for mode in --headless ""; do
        mkdir -p "$T/cwd"; rm -f "$T"/cwd/*
        ( cd "$T/cwd" && "$VB" --engine $e $mode --cosim --max-cycles 3000000 --quiet-stats --key-script "$KS" "$P/kbd.elf" >"$T/out" 2>"$T/err" ); rc=$?
        check_eq "kbd $e ${mode:-windowed} exit" $rc 0
        cmp -s "$T/out" "$T/kexp" && ok || { bad "kbd $e ${mode:-windowed}: events differ"; cat "$T/out" | head -20; head -30 "$T/err"; }
    done
done

echo "== cosim failure report (ISS state corrupted on purpose with --cosim-inject)"
for e in fast accurate; do
    "$VB" --engine $e --headless --cosim --cosim-inject 1000 --quiet-stats "$P/hello.elf" >/dev/null 2>"$T/err"; rc=$?
    check_eq "cosim-inject $e exit status" $rc 119
    grep -q "COSIM MISMATCH" "$T/err" && grep -q "DIFFERS" "$T/err" && grep -q "instruction #1000" "$T/err" && ok || { bad "$e: cosim report incomplete"; head -20 "$T/err"; }
    [ "$(grep -c '^   #' "$T/err")" -eq 32 ] && ok || bad "$e: cosim report should list 32 previous instructions"
done

echo "== hardware-checker failure path (scan-out mismatch injected on purpose)"
"$VB" --engine accurate --headless --scanout-inject --quiet-stats "$P/video_hold.elf" >/dev/null 2>"$T/err"; check_eq "scanout-inject exit status" $? 121
grep -q "scan-out mismatch" "$T/err" && ok || { bad "scan-out mismatch not reported"; head "$T/err"; }
"$VB" --engine accurate --headless --scanout-inject --keep-going --quiet-stats --stats-json "$T/kg.json" "$P/video_hold.elf" >"$T/out" 2>"$T/err"; check_eq "--keep-going still reports status 121" $? 121
check_eq "--keep-going ran to the end" "$(cat "$T/out")" "video done"
[ "$(json "$T/kg.json" scan_bad)" -ge 2 ] && ok || bad "--keep-going: expected several mismatching frames"

echo "== Doom: leading frames of -timedemo demo3 under cosim vs the native reference"
NF=60; [ "$MODE" = long ] && NF=400
for e in fast accurate; do
    "$VB" --engine $e --headless --cosim --wad "$WAD" --max-frames $NF --dump-frames "$T/doom.$e.dfrm" --stats-json "$T/doom.$e.json" --quiet-stats "$DOOM" -- -timedemo demo3 >"$T/doom.$e.out" 2>"$T/err"; rc=$?
    check_eq "doom $e exit (max-frames)" $rc 122
    [ $rc -eq 122 ] || head -40 "$T/err"
    "$FRAMECMP" -q --limit $NF "$T/doom.$e.dfrm" "$REF" >"$T/fc.txt" && ok || { bad "doom $e: first $NF frames differ from the reference"; cat "$T/fc.txt"; }
    check_eq "doom $e frames presented" "$(json "$T/doom.$e.json" frames)" $NF
    check_eq "doom $e sdram violations" "$(json "$T/doom.$e.json" sdram_violations)" "$([ $e = fast ] && echo null || echo 0)"
    [ $e = accurate ] && { check_eq "doom accurate scan-out mismatches" "$(json "$T/doom.$e.json" scan_bad)" 0
                           [ "$(json "$T/doom.$e.json" scan_checked)" -ge 5 ] && ok || bad "doom accurate: too few scan-out frames checked"
                           check_eq "doom accurate serial errors" "$(json "$T/doom.$e.json" serial_errors)" 0
                           check_eq "doom accurate video errors" "$(json "$T/doom.$e.json" video_errors)" 0; }
    echo "   $e: $NF frames identical to the reference; $(json "$T/doom.$e.json" cosim_instructions) instructions compared; IPC $(json "$T/doom.$e.json" ipc), $(json "$T/doom.$e.json" cycles_per_frame) cycles/frame"
done

echo "== model: lockstep against the accurate RTL (--cosim=model: retire cycle, counters, frames, serial) and as an engine"
# the counters the model must reproduce, as one line per run
cnt() { for k in cycles instret ic_access ic_miss dc_access dc_miss branches branch_miss jumps jump_miss frames; do printf '%s=%s ' $k "$(json "$1" $k)"; done; }
lockstep() {   # name expected-status args...: one lockstep run, its instruction count added to LS
    name=$1; want=$2; shift 2
    mkdir -p "$T/cwd"   # F12 in a key script writes a screenshot to the current directory
    ( cd "$T/cwd" && "$VB" --engine accurate --cosim=model --headless --quiet-stats --stats-json "$T/ls.json" "$@" > "$T/ls.out" 2>"$T/err" ); rc=$?
    check_eq "lockstep $name exit" $rc $want
    [ $rc -eq $want ] || head -40 "$T/err"
    LS=$((LS + $(json "$T/ls.json" cosim_instructions)))
}
LS=0; n=0
for f in "$ISA"/rv32*.elf; do
    [ -f "$f" ] || continue
    n=$((n+1))
    lockstep "$(basename "$f")" 0 --isa-test "$f"
    "$VB" --engine model --headless --isa-test --quiet-stats "$f" >/dev/null 2>&1; check_eq "model isa-test $(basename "$f")" $? 0
done
echo "   $n riscv-tests, $LS instructions compared"
KS20=$(for k in A B C D E F G H I J K L M N O P Q R S T; do printf '2000000:+%s,2400000:-%s,' $k $k; done)
for p in hello:$SW/hello/hello.elf testpattern:$SW/testpattern/testpattern.elf coremark-quick:$SW/coremark/coremark-quick.elf corners:$P/corners.elf; do
    name=${p%%:*}; elf=${p#*:}; ks=; [ $name = corners ] && ks=$KS20
    LS=0; lockstep $name 0 --key-script "$ks" "$elf"; cp "$T/ls.out" "$T/$name.ls.out"
    "$VB" --engine accurate --headless --quiet-stats --stats-json "$T/$name.acc.json" --key-script "$ks" "$elf" > /dev/null 2>&1
    "$VB" --engine model --headless --quiet-stats --stats-json "$T/$name.mdl.json" --key-script "$ks" "$elf" > "$T/$name.mdl.out" 2>&1; check_eq "model $name exit" $? 0
    check_eq "model $name: the accurate engine's statistics" "$(cnt "$T/$name.mdl.json")" "$(cnt "$T/$name.acc.json")"
    cmp -s "$T/$name.mdl.out" "$T/$name.ls.out" && ok || bad "model $name: serial output differs from the RTL's"
    echo "   $name: $LS instructions compared"
done
grep -q "^A.* keys 10 polls 0[0-9a-f]* sum" "$T/corners.ls.out" && ok || { bad "corners: did not see 16 of 20 keys and a busy UART"; cat -v "$T/corners.ls.out"; }
printf "$KEYS_EXPECTED" > "$T/kexp"
for mode in --headless ""; do
    LS=0; lockstep "kbd ${mode:-windowed}" 0 $mode --max-cycles 3000000 --key-script "$KS" "$P/kbd.elf"
    cmp -s "$T/ls.out" "$T/kexp" && ok || bad "lockstep kbd ${mode:-windowed}: events differ"
    ( cd "$T/cwd" && "$VB" --engine model $mode --max-cycles 3000000 --quiet-stats --key-script "$KS" "$P/kbd.elf" > "$T/out" 2>"$T/err" ); check_eq "model kbd ${mode:-windowed} exit" $? 0
    cmp -s "$T/out" "$T/kexp" && ok || bad "model kbd ${mode:-windowed}: events differ"
done
"$VB" --engine accurate --cosim=model --cosim-inject 1000 --headless --quiet-stats "$P/hello.elf" >/dev/null 2>"$T/err"; check_eq "--cosim=model inject exit" $? 119
grep -q "cycle (+1)" "$T/err" && grep -q "DIFFERS" "$T/err" && [ "$(grep -c '^   #' "$T/err")" -eq 32 ] && ok || { bad "--cosim=model report incomplete"; head -20 "$T/err"; }
"$VB" --engine model --headless --quiet-stats "$P/fault.elf" >"$T/out" 2>"$T/err"; check_eq "model fault exit (where the ISS stops)" $? 125
check_eq "model fault: serial delivered" "$(cat "$T/out")" "about to fault"
"$VB" --engine fast --cosim=model --headless "$P/hello.elf" >/dev/null 2>&1; check_eq "--cosim=model needs the accurate engine" $? 126
"$VB" --engine model --cosim --headless "$P/hello.elf" >/dev/null 2>&1; check_eq "--engine model rejects --cosim" $? 126
LS=0; lockstep "doom $NF frames" 122 --wad "$WAD" --max-frames $NF --dump-frames "$T/doom.ls.dfrm" "$DOOM" -- -timedemo demo3
cp "$T/ls.json" "$T/doom.ls.json"
"$VB" --engine model --headless --wad "$WAD" --max-frames $NF --dump-frames "$T/doom.mdl.dfrm" --stats-json "$T/doom.mdl.json" --quiet-stats "$DOOM" -- -timedemo demo3 >/dev/null 2>&1
check_eq "model doom exit (max-frames)" $? 122
check_eq "model doom: the RTL's statistics" "$(cnt "$T/doom.mdl.json")" "$(cnt "$T/doom.ls.json")"
"$FRAMECMP" -q --limit $NF "$T/doom.mdl.dfrm" "$REF" >"$T/fc.txt" && ok || { bad "model doom: first $NF frames differ from the reference"; cat "$T/fc.txt"; }
echo "   doom: $NF frames, $LS instructions compared; model $(json "$T/doom.mdl.json" speed) MHz"

if [ "$MODE" = long ]; then
    echo "== long: full CoreMark on fast, accurate and model (no cosim)"
    for e in fast accurate model; do
        "$VB" --engine $e --headless --quiet-stats --stats-json "$T/cm.$e.json" "$SW/coremark/coremark.elf" > "$T/cm.$e" 2>/dev/null; check_eq "coremark $e exit" $? 0
        grep -q "Correct operation validated" "$T/cm.$e" && ok || bad "coremark $e: no validation line"
        grep "CoreMark/MHz" "$T/cm.$e"
    done
    check_eq "coremark: the model has the accurate engine's statistics" "$(cnt "$T/cm.model.json")" "$(cnt "$T/cm.accurate.json")"
    cmp -s "$T/cm.model" "$T/cm.accurate" && ok || bad "coremark: model serial output differs from accurate"
fi

echo "vboard rtl tests ($MODE): $PASS checks passed, $FAIL failed"
[ $FAIL -eq 0 ]
