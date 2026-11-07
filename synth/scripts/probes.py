#!/usr/bin/env python3
"""Timing-coverage probes: which block-RAM / DSP pins does this nextpnr build analyse?

    probes.py <out_dir> <yosys> <nextpnr>

For every MODE of synth/probes/timing_probe.sv: synthesise, find the physical
hard-block pins the slow cone attaches to (from the netlist), place and route,
and decide from the critical-path report whether the cone was analysed. Writes
<out_dir>/probes.json and prints a table (also used by report.py).
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, '..', 'probes', 'timing_probe.sv')
DEVICE = 'GW2AR-LV18QN88C8/I7'
MODES = ['ref', 'dp_ada', 'dp_adb', 'dp_ceb', 'dp_dib', 'dp_wreb', 'dp_dob',
         'sdp_ada', 'sdp_adb', 'sdp_ceb', 'dsp_in', 'dsp_out']
HARD = ('DPB', 'DPX9B', 'SDPB', 'SDPX9B', 'MULT18X18', 'MULT36X36', 'MULT9X9')


def run(cmd, log):
    with open(log, 'w') as f:
        return subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT)


def cone_pins(netlist):
    """Physical hard-block pins whose net is driven by / drives the adder cone (ALU cells)."""
    top = [m for m, v in netlist['modules'].items() if v.get('attributes', {}).get('top')][0]
    cells = netlist['modules'][top]['cells']
    driver, sinks = {}, {}
    for cn, c in cells.items():
        for p, bits in c['connections'].items():
            d = c.get('port_directions', {}).get(p, 'input')
            for b in bits:
                if not isinstance(b, int):
                    continue
                if d == 'output':
                    driver[b] = cn
                else:
                    sinks.setdefault(b, []).append(cn)

    def from_cone(b, depth=0):
        cn = driver.get(b)
        if cn is None or depth > 3:
            return False
        c = cells[cn]
        if c['type'] == 'ALU':
            return True
        if c['type'].startswith('LUT') or c['type'].startswith('MUX2'):
            return any(from_cone(x, depth + 1) for p, bits in c['connections'].items()
                       if c['port_directions'].get(p) == 'input' for x in bits if isinstance(x, int))
        return False

    pins, hard = set(), None
    for cn, c in cells.items():
        if c['type'] not in HARD:
            continue
        hard = c['type']
        for p, bits in c['connections'].items():
            d = c.get('port_directions', {}).get(p, 'input')
            for b in bits:
                if not isinstance(b, int):
                    continue
                if d == 'input' and from_cone(b):
                    pins.add(re.sub(r'\d+$', '', p))
                if d == 'output' and any(cells[s]['type'] == 'ALU' for s in sinks.get(b, [])):
                    pins.add(re.sub(r'\d+$', '', p))
    return hard, sorted(pins)


def main():
    out, yosys, nextpnr = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    cst = os.path.join(out, 'probe.cst')
    with open(cst, 'w') as f:
        f.write('IO_LOC "clk" 4;\nIO_LOC "o" 15;\n')
    results = []
    for mode in MODES:
        d = os.path.join(out, mode)
        os.makedirs(d, exist_ok=True)
        top = os.path.join(d, 'top.sv')
        with open(top, 'w') as f:
            f.write('module probe_top (input wire clk, output wire o);\n'
                    f'    timing_probe #(.MODE("{mode}")) u (.clk(clk), .o(o));\nendmodule\n')
        js = os.path.join(d, 'top.json')
        rc = run([yosys, '-p', f'plugin -i slang; read_slang {PROBE} {top} --top probe_top; '
                  f'synth_gowin -family gw2a -top probe_top -json {js}'], os.path.join(d, 'yosys.log'))
        if rc:
            sys.exit(f'probes.py: yosys failed for {mode}')
        hard, pins = cone_pins(json.load(open(js)))
        rep = os.path.join(d, 'report.json')
        rc = run([nextpnr, '--json', js, '--device', DEVICE, '--vopt', 'family=GW2A-18C',
                  '--vopt', f'cst={cst}', '--freq', '100', '--seed', '1', '--report', rep],
                 os.path.join(d, 'pnr.log'))
        if rc:
            sys.exit(f'probes.py: nextpnr failed for {mode}')
        r = json.load(open(rep))
        cp = r['critical_paths'][0]['path']
        delay = sum(x['delay'] for x in cp)
        ends = (cp[0]['from']['cell'] + '.' + cp[0]['from']['port'], cp[-1]['to']['cell'] + '.' + cp[-1]['to']['port'])
        results.append({'mode': mode, 'hard': hard, 'pins': pins, 'crit_ns': delay,
                        'start': ends[0], 'end': ends[1]})
    ref = results[0]['crit_ns']
    for x in results:
        x['analysed'] = (x['mode'] == 'ref') or (x['crit_ns'] >= 0.75 * ref)
    json.dump(results, open(os.path.join(out, 'probes.json'), 'w'), indent=1)
    print(f'{"mode":8} {"cell":10} {"cone pins":22} {"critical":>8}  analysed  (ref cone {ref:.2f} ns)')
    for x in results:
        print(f'{x["mode"]:8} {str(x["hard"]):10} {",".join(x["pins"]):22} {x["crit_ns"]:7.2f}   '
              f'{"yes" if x["analysed"] else "NO"}')


if __name__ == '__main__':
    main()
