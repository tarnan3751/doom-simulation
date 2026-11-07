# lib.sh - sourced by test_cli.sh and test_rtl.sh: check counters, JSON fields, the expected key events
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
check_eq() { if [ "$2" = "$3" ]; then ok; else bad "$1: got '$2', expected '$3'"; fi; }   # name got expected
json() { sed -n "s/^  \"$2\": \(.*\),\{0,1\}$/\1/p" "$1" | sed 's/,$//; s/^"//; s/"$//'; }      # file key -> value
# what tests/kbd.c prints for the key scripts of both test files (pressed/released, Doom code)
KEYS_EXPECTED='+ac\n+a3\n-ac\n-a3\n+61\n-61\n+a2\n-a2\n+b6\n-b6\n+bf\n-bf\n+0d\n-0d\n+71\n-71\n'
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
