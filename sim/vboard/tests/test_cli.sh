#!/bin/sh
# vboard end-to-end tests. usage: test_cli.sh VBOARD TEST_HOST PROGDIR ISADIR OUTDIR
VB=$1; TH=$2; P=$3; ISA=$4; OUT=$5
T=$OUT/work
rm -rf "$T"; mkdir -p "$T"
. "$(dirname "$0")/lib.sh"
# run VBOARD with stdout to $T/out, stderr to $T/err; sets $rc
# This script tests the front end on the ISS engine: add --engine iss unless a test names one.
vb() { case " $* " in *" --engine "*) "$VB" "$@" ;; *) "$VB" --engine iss "$@" ;; esac; }
run() { vb "$@" >"$T/out" 2>"$T/err"; rc=$?; }

echo "== headless: serial, exit status, stdout purity"
run --headless "$P/hello.elf"
printf 'hello from vboard\nsecond line, no newline at end' > "$T/exp"
cmp -s "$T/out" "$T/exp" && ok || bad "hello: stdout differs from exactly what the CPU printed"
check_eq "hello exit status" $rc 7
grep -q "exit status 7" "$T/err" && ok || bad "summary missing on stderr"
run --headless --quiet-stats "$P/hello.elf"
check_eq "quiet-stats leaves stderr empty" "$(wc -c < "$T/err" | tr -d ' ')" 0
check_eq "quiet-stats stdout unchanged" "$(wc -c < "$T/out" | tr -d ' ')" "$(wc -c < "$T/exp" | tr -d ' ')"
run --headless --stats-json "$T/s.json" "$P/hello.elf"
check_eq "json stop" "$(json "$T/s.json" stop)" exit
check_eq "json program_exit_code" "$(json "$T/s.json" program_exit_code)" 7
check_eq "json ic_hit_rate unavailable on iss" "$(json "$T/s.json" ic_hit_rate)" null
check_eq "json cycles == instret on iss" "$(json "$T/s.json" cycles)" "$(json "$T/s.json" instret)"
run --headless --cosim "$P/hello.elf"
check_eq "--cosim needs an RTL engine" $rc 126
grep -q "RTL engine" "$T/err" && ok || bad "cosim on iss: message"
run --engine bogus --headless "$P/hello.elf"
check_eq "unknown engine" $rc 126
run --headless /nonexistent.elf
check_eq "missing ELF" $rc 126
run --bogus "$P/hello.elf"
check_eq "unknown option" $rc 2
run --headless "$P/fault.elf"
check_eq "bad fetch" $rc 125
grep -q "unmapped\|fetch" "$T/err" && ok || bad "fault message"
check_eq "serial before the fault still delivered" "$(cat "$T/out")" "about to fault"

echo "== --isa-test"
n=0
for f in "$ISA"/rv32*.elf; do
    [ -f "$f" ] || continue
    n=$((n+1))
    run --headless --isa-test --quiet-stats "$f"
    [ $rc -eq 0 ] && ok || bad "isa-test $(basename "$f") exit $rc"
done
[ $n -ge 10 ] && ok || bad "found only $n riscv-tests in $ISA"
echo "   ($n riscv-tests run)"
run --headless --isa-test "$P/tohost_pass.elf"; check_eq "tohost=1 passes" $rc 0
run --headless --isa-test "$P/tohost_fail.elf"; check_eq "tohost=5 fails" $rc 1
grep -q "FAILED test 2" "$T/err" && ok || bad "failure message names test 2"
run --headless --isa-test "$P/hello.elf"; check_eq "isa-test without tohost" $rc 126

echo "== stop conditions"
run --headless --max-cycles 1000000 --stats-json "$T/s.json" "$P/spin.elf"
check_eq "max-cycles exit" $rc 124
check_eq "max-cycles exact" "$(json "$T/s.json" cycles)" 1000000
check_eq "spin printed its dot" "$(cat "$T/out")" "."
run --headless --max-seconds 0.3 --quiet-stats "$P/spin.elf"
check_eq "max-seconds exit" $rc 123
run --headless --max-frames 3 --dump-frames "$T/f3.dfrm" --stats-json "$T/s.json" "$P/video.elf"
check_eq "max-frames exit" $rc 122
check_eq "max-frames count" "$(json "$T/s.json" frames)" 3
"$TH" frames "$T/f3.dfrm" 3 > "$T/th.txt" && ok || { bad "max-frames dump"; cat "$T/th.txt"; }

echo "== boot info: arguments and WAD"
run --headless --wad "$OUT/test.wad" "$P/args.elf" -- foo "bar baz" 42
{ printf 'wad_addr=80800000 wad_size=000003e9\nwad_name=[test.wad]\ncmdline=[foo bar baz 42]\nwad head=ABCDEFGH tail='; tail -c 4 "$OUT/test.wad"; echo; } > "$T/exp"
cmp -s "$T/out" "$T/exp" && ok || { bad "args+wad output"; cat "$T/out"; }
run --headless "$P/args.elf"
printf 'wad_addr=00000000 wad_size=00000000\nwad_name=[]\ncmdline=[]\n' > "$T/exp"
cmp -s "$T/out" "$T/exp" && ok || { bad "no-wad boot info"; cat "$T/out"; }
run --headless --wad /nonexistent.wad "$P/args.elf"; check_eq "missing WAD" $rc 126

echo "== video: frame dump and screenshot"
run --headless --dump-frames "$T/v.dfrm" --screenshot "$T/v.ppm" "$P/video.elf"
check_eq "video exit" $rc 0
check_eq "video text" "$(cat "$T/out")" "video done"
"$TH" frames "$T/v.dfrm" 5 > "$T/th.txt" && ok || { bad "dump equals drawn pattern"; cat "$T/th.txt"; }
"$TH" shot "$T/v.ppm" 4 > "$T/th.txt" && ok || { bad "final screenshot geometry"; cat "$T/th.txt"; }
cp "$T/v.ppm" "$OUT/screenshot.ppm"
sips -s format png "$OUT/screenshot.ppm" --out "$OUT/screenshot.png" >/dev/null 2>&1
run --headless --screenshot "$T/v2.ppm" --screenshot-frame 2 --max-frames 3 --quiet-stats "$P/video.elf"
check_eq "screenshot-frame run exit" $rc 122
"$TH" shot "$T/v2.ppm" 1 > "$T/th.txt" && ok || { bad "screenshot at frame 2 shows frame 2"; cat "$T/th.txt"; }
sips -s format png "$T/v2.ppm" --out "$OUT/screenshot-frame2.png" >/dev/null 2>&1
echo "   screenshot: $OUT/screenshot.png"

echo "== windowed (SDL dummy video driver)"
run --max-frames 5 --dump-frames "$T/w.dfrm" --screenshot "$T/w.ppm" --quiet-stats "$P/video.elf"
check_eq "windowed max-frames exit" $rc 122
"$TH" frames "$T/w.dfrm" 5 > "$T/th.txt" && ok || { bad "windowed dump"; cat "$T/th.txt"; }
"$TH" shot "$T/w.ppm" 4 > "$T/th.txt" && ok || { bad "windowed screenshot"; cat "$T/th.txt"; }
run --max-seconds 0.5 --scale 2 --quiet-stats "$P/videoloop.elf"
check_eq "windowed max-seconds exit" $rc 123
run --max-cycles 3000000 --realtime --quiet-stats "$P/spin.elf"
check_eq "realtime max-cycles exit" $rc 124

echo "== keyboard"
KS='1000:+Left,2000:+Left Ctrl,3000:-Left,4000:-Left Ctrl,5000:+A,6000:-A,7000:+Space,8000:-Space,9000:+Left Shift,9500:+Right Shift,10000:-Left Shift,10500:-Right Shift,11000:+F5,12000:-F5,13000:+Return,14000:-Return,14500:+F12,14600:-F12,15000:+Q,16000:-Q'
printf "$KEYS_EXPECTED" > "$T/exp"
mkdir -p "$T/cwd"
for mode in --headless ""; do
    rm -f "$T"/cwd/*
    ( cd "$T/cwd" && vb $mode --max-cycles 5000000 --quiet-stats --key-script "$KS" "$P/kbd.elf" >"$T/out" 2>"$T/err" ); rc=$?
    check_eq "kbd ${mode:-windowed} exit (program ends on release of q)" $rc 0
    cmp -s "$T/out" "$T/exp" && ok || { bad "kbd ${mode:-windowed}: events differ"; cat "$T/out"; }
    [ -f "$T/cwd/vboard-shot-001.ppm" ] && ok || bad "F12 hotkey did not write a screenshot (${mode:-windowed})"
done
run --headless --key-script 'zz' "$P/kbd.elf"; check_eq "bad key script" $rc 126
run --headless --key-script '10:+NoSuchKey' "$P/kbd.elf"; check_eq "unknown key name" $rc 126

echo "== throughput"
BENCH=$P/bench.elf
ISSRUN=$(dirname "$OUT")/iss/iss-run
vb_mips() { vb "$@" --max-seconds 60 --stats-json "$T/b.json" "$BENCH" >/dev/null 2>&1; echo "$(json "$T/b.json" cycles) $(json "$T/b.json" run_seconds)" | awk '{ printf "%.1f", $1/$2/1e6 }'; }
H=$(vb_mips --headless --quiet-stats)
W=$(vb_mips --quiet-stats)
echo "   vboard headless: $H MIPS, windowed (dummy driver): $W MIPS"
if [ -x "$ISSRUN" ]; then
    R=$("$ISSRUN" --stats "$BENCH" 2>&1 >/dev/null | sed -n 's/.* \([0-9.]*\) MIPS.*/\1/p')
    echo "   iss-run: $R MIPS"
    awk -v h="$H" -v r="$R" 'BEGIN { exit !(h >= 0.6 * r) }' && ok || echo "  note: vboard headless throughput below 60% of iss-run (wall-clock ratio of separate runs; depends on machine load, not counted as a failure)"
    awk -v h="$W" -v r="$R" 'BEGIN { exit !(h >= 0.6 * r) }' && ok || echo "  note: vboard windowed throughput below 60% of iss-run (wall-clock ratio of separate runs; depends on machine load, not counted as a failure)"
else
    echo "   (build/iss/iss-run not built; comparison skipped)"
fi

echo "vboard cli tests: $PASS checks passed, $FAIL failed"
[ $FAIL -eq 0 ]
