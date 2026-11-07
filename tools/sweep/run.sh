#!/bin/sh
# Cache-size sweep, end to end: build variants, run demo3 on each, check, write tables and charts.
#   tools/sweep/run.sh [JOBS]        (JOBS = parallel runs/builds, default 6)
# Needs build/doom/soc/doom.elf, build/doom/ref/demo3.dfrm and build/tools/framecmp; build/vboard/vboard
# is not needed: every point, the default included, is built into build/sweep/<name>/. Finished runs are not repeated
# (delete build/sweep/runs/<name> to force). Products: build/sweep/, docs/results/cache-sweep.md + SVG charts.
set -eu
cd "$(dirname "$0")/../.."
JOBS=${1:-6}
CFG=tools/sweep/configs.txt
mkdir -p build/sweep/runs

# 1. build the variants (default point included, from the same sources)
grep -v '^#' $CFG | while read -r n i d; do
  [ -x build/sweep/$n/vboard ] && continue
  echo "$n $i $d"
done | xargs -P "$JOBS" -L1 sh -c '
  n=$0; i=$1; d=$2
  make -s -C sim/vboard PGO=0 OUT="$PWD/build/sweep/$n" VL_GEXTRA="-GICACHE_BYTES=$((i*1024)) -GDCACHE_BYTES=$((d*1024))" \
      > "build/sweep/build-$n.log" 2>&1 && echo "built $n" || { echo "BUILD FAILED $n (see build/sweep/build-$n.log)"; exit 255; }'

# 2. run (largest caches first is not needed; order as listed)
grep -v '^#' $CFG | while read -r n i d; do
  [ -f build/sweep/runs/$n/cmp_exit ] && continue
  echo "$n $i $d"
done | xargs -P "$JOBS" -L1 tools/sweep/run_one.sh

# 3. tables, charts, write-up tables
python3 tools/sweep/report.py
