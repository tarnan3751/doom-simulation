#!/bin/sh
# tools/sweep/run_one.sh <name> <icache_KB> <dcache_KB>
# Runs full demo3 on the accurate engine of the variant build, compares frames with the reference, deletes the dump.
# Uses build/sweep/<name>/vboard.
set -u
cd "$(dirname "$0")/../.."
NAME=$1; I=$2; D=$3
RUN=build/sweep/runs/$NAME
mkdir -p "$RUN"
BIN=build/sweep/$NAME/vboard
$BIN --engine accurate --headless --wad third_party/wad/doom1.wad \
    --dump-frames "$RUN/frames.dfrm" --stats-json "$RUN/stats.json" \
    build/doom/soc/doom.elf -- -timedemo demo3 > "$RUN/serial.txt" 2> "$RUN/stderr.txt"
echo $? > "$RUN/run_exit"
build/tools/framecmp "$RUN/frames.dfrm" build/doom/ref/demo3.dfrm > "$RUN/cmp.txt" 2>&1
echo $? > "$RUN/cmp_exit"
rm -f "$RUN/frames.dfrm"
echo "done $NAME run=$(cat $RUN/run_exit) cmp=$(cat $RUN/cmp_exit)"
