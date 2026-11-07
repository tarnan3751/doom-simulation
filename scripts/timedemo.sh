#!/bin/sh
# Runs one Doom timedemo on one engine of the virtual board, dumps every
# presented frame, and compares the dump pixel for pixel with the native
# reference dump of the same demo.
#
#   scripts/timedemo.sh <iss|fast|accurate|model> <demo1|demo2|demo3> [extra vboard options, e.g. --cosim or --cosim=model]
#
# Products (build/results/): <demo>-<engine>.dfrm   frame dump
#                            <demo>-<engine>.json   run statistics
#                            <demo>-<engine>.serial what the CPU printed
#                            <demo>-<engine>.cmp    frame comparison report
# Exit status: 0 only if the run exited cleanly and every frame is identical.
set -u
cd "$(dirname "$0")/.."
ENGINE=${1:?engine}; DEMO=${2:?demo}; shift 2
TAG="$DEMO-$ENGINE"
case " $* " in *" --cosim=model "*) TAG="$TAG-cosim-model";; *" --cosim "*) TAG="$TAG-cosim";; esac
OUT=build/results
REF=build/doom/ref/$DEMO.dfrm
mkdir -p "$OUT"
[ -f "$REF" ] || { echo "missing reference $REF (make -C sw/doom ref)"; exit 2; }

build/vboard/vboard --engine "$ENGINE" --headless --wad third_party/wad/doom1.wad \
    --dump-frames "$OUT/$TAG.dfrm" --stats-json "$OUT/$TAG.json" "$@" \
    build/doom/soc/doom.elf -- -timedemo "$DEMO" > "$OUT/$TAG.serial" 2> "$OUT/$TAG.log"
RUN=$?
build/tools/framecmp "$OUT/$TAG.dfrm" "$REF" > "$OUT/$TAG.cmp" 2>&1
CMP=$?
echo "== $TAG: run exit $RUN, frame compare exit $CMP"
grep -a "timed " "$OUT/$TAG.serial"
tail -3 "$OUT/$TAG.cmp"
[ "$RUN" -eq 0 ] && [ "$CMP" -eq 0 ]
