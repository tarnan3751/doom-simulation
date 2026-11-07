#!/bin/sh
# run-iss.sh - run the sw/ programs on the ISS and check their exit status
# and key output lines.  usage: run-iss.sh ISS BUILD_SW WAD
ISS=$1 B=$2 WAD=$3
pass=0 fail=0
LOG=$B/run-iss
mkdir -p "$LOG"

run() {   # name expected-status grep-pattern -- iss-args...
    name=$1 want=$2 pat=$3
    shift 4
    "$ISS" "$@" > "$LOG/$name.log" 2>&1
    st=$?
    if [ "$st" -eq "$want" ] && grep -q -- "$pat" "$LOG/$name.log"; then
        echo "  PASS  $name (exit $st)"
        pass=$((pass + 1))
    else
        echo "  FAIL  $name (exit $st, want $want; pattern \"$pat\"), log: $LOG/$name.log"
        tail -5 "$LOG/$name.log" | sed 's/^/        /'
        fail=$((fail + 1))
    fi
}

WADARG=""
[ -f "$WAD" ] && WADARG="--wad $WAD"
run hello            0   "hello: done"                   -- $WADARG "$B/hello/hello.elf" -- one two
run hello-trap-load  228 "TRAP: load address misaligned" -- "$B/hello/hello.elf" -- trap-load
run hello-trap-ill   226 "TRAP: illegal instruction"     -- "$B/hello/hello.elf" -- trap-illegal
run hello-abort      134 "abort() called"                -- "$B/hello/hello.elf" -- abort
run hello-stack      223 "STACK OVERFLOW"                -- "$B/hello/hello.elf" -- stack-overflow
run testpattern      0   "VID_FLIP counter advanced by 8 (ok)" -- --dump-frames "$LOG/testpattern.dfrm" "$B/testpattern/testpattern.elf" -- 8
run libctest         0   "0 failures -> PASSED"          -- $WADARG "$B/libctest/libctest.elf"
run coremark-quick   0   "crcfinal"                      -- "$B/coremark/coremark-quick.elf"
run coremark         0   "Correct operation validated"   -- "$B/coremark/coremark.elf"
echo "run-iss: $pass passed, $fail failed (logs in $LOG)"
[ "$fail" -eq 0 ]
