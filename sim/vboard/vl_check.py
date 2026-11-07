#!/usr/bin/env python3
# vl_check.py ROOT_CPP OUT_H: proves on the verilated code (<prefix>___024root__0.cpp, Verilator 5.054) what the
# shortcuts of engine_rtl.cpp rely on and writes OUT_H with VB_MODEL_CHECKED 1 (shortcuts exact) or 0:
#  * the only edge trigger is posedge clk: one eval() per cycle with clk held at 1 is exact;
#  * the input-combinational (ico) region watches exactly clk, rst, sdram_dq_i, key_valid, key_data, and none of
#    its logic runs for clk or for the region's first-iteration flag alone: it may be skipped while the others
#    are unchanged;
#  * no thread pool (mtasks would need Verilated::endOfEval).
import re, sys

src = open(sys.argv[1]).read()
INPUTS = {'clk', 'rst', 'sdram_dq_i', 'key_valid', 'key_data'}
POSEDGE = '(::QData)((::IData)(((::IData)(vlSelfRef.__VprimaryPorts.clk)&(~(::IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__1)))))'


def region(name):
    m = re.search(r'\nbool \w+___024root___eval_%s\([^)]*\) \{\n(.*?)\n\}\n' % name, src, re.S)
    return re.sub(r'\s+', '', m.group(1)) if m else ''


def check():
    ico, act = region('ico'), region('act')
    if '__Vm_threadPoolp' in src:
        return 'a --threads model'
    if set(re.findall(r'__Vtrigprevexpr___TOP__(\w+?)__\d+\b', src)) != INPUTS:
        return 'triggers on inputs other than ' + ', '.join(sorted(INPUTS))
    if re.findall(r'__VactTriggered\[\d+U\]=([^;]*);', act) != [POSEDGE]:
        return 'edge triggers other than posedge clk'
    vec = re.search(r'__VicoTriggered\[0U\]=([^;]*);', ico)
    bit = {n: int(b or '0', 0) for n, b in re.findall(r'__Vtrigprevexpr___TOP__(\w+?)__0\)+(?:<<(0x[0-9a-f]+|\d+)U)?', vec.group(1) if vec else '')}
    conds = re.findall(r'if\(\(\(\((0x[0-9a-f]+|\d+)ULL&vlSelfRef\.__VicoTriggered\[(\d+)U\]\)\)!=0ULL\)\)', ico)
    if set(bit) != INPUTS or ico.count('__VicoTriggered[') != 3 + len(conds):   # 2 x [1U] (first-iteration flag) + [0U]
        return 'unrecognised ico region code'
    if any(w != '0' or int(m, 0) >> bit['clk'] & 1 for m, w in conds):
        return 'ico logic runs for clk or for the first iteration'
    return None


why = check()
with open(sys.argv[2], 'w') as f:
    f.write('// written by vl_check.py from %s\n#define VB_MODEL_CHECKED %d\n' % (sys.argv[1], why is None))
    if why:
        f.write('#define VB_MODEL_CHECK_WHY "%s"\n' % why)
if why:
    print('vl_check.py: %s: %s; the engine falls back to the portable eval() loop' % (sys.argv[1], why))
