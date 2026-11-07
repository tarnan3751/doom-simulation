#!/bin/sh
# Test runner for tests/mem (called by the Makefile):  run.sh quick|full <build dir>
# One line per step plus a summary; exits non-zero if any step failed. Logs: <build dir>/logs/<step>.log
set -u
MODE=$1
OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)
LOGS=$OUT/logs
mkdir -p "$LOGS"
rm -f "$LOGS"/*.log
NSTEP=0
NFAIL=0
T0=$(date +%s)

# step <name> <command...>: run, keep the log, show its most relevant line
step() {
    name=$1; shift
    NSTEP=$((NSTEP + 1))
    t0=$(date +%s)
    if "$@" > "$LOGS/$name.log" 2>&1 < /dev/null; then res=PASS; else res=FAIL; NFAIL=$((NFAIL + 1)); fi
    line=$(grep -E "TOTAL|inject|sdram_model_test: [0-9]+ checks|lint:|penalty run|^BRAM" "$LOGS/$name.log" | tail -1)
    printf "%s  %-22s %4ss  %s\n" "$res" "$name" "$(( $(date +%s) - t0 ))" "$line"
    if [ "$res" = FAIL ]; then
        grep -E "ERROR|FAIL|violation|Error" "$LOGS/$name.log" | head -5 | sed 's/^/        /'
    fi
}

echo "== memsys tests ($MODE) =="
step lint        make -s -C "$HERE" lint
step sdram_model "$OUT/sdram_model_test"
# configuration, quick seeds/ops, full seeds/ops
while read -r cfg qs qo fs fo; do
    if [ "$MODE" = quick ]; then step "$cfg" "$OUT/$cfg/Vmemsys" --seeds "$qs" --ops "$qo"
    else                         step "$cfg" "$OUT/$cfg/Vmemsys" --seeds "$fs" --ops "$fo"; fi
done <<EOF
sdram_def   1-4 100000 1-48 500000
sdram_tiny  1-2 50000  1-32 300000
sdram_row11 1-1 50000  1-16 300000
sdram_100m  1-1 50000  1-16 300000
ideal_def   1-2 100000 1-48 500000
ideal_tiny  1-2 50000  1-32 300000
EOF
# pin faults the SDRAM model must report
if [ "$MODE" = quick ]; then set -- 3 15000 refresh act; else set -- 5 40000 refresh act contention powerup; fi
seed=$1 ops=$2; shift 2
for k in "$@"; do step "inject_$k" "$OUT/sdram_def/Vmemsys" --seed "$seed" --ops "$ops" --inject "$k"; done
if [ "$MODE" != quick ]; then
    step penalty_sdram "$OUT/sdram_def/Vmemsys" --penalty
    step penalty_ideal "$OUT/ideal_def/Vmemsys" --penalty
fi
step synth       make -s -C "$HERE" synth

echo "== $NSTEP steps, $NFAIL failed, $(( $(date +%s) - T0 ))s (logs: $LOGS) =="
[ "$NFAIL" -eq 0 ]
