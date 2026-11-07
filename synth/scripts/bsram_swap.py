#!/usr/bin/env python3
"""Timing-analysis view of a Gowin netlist with ports A and B of every true-dual-port
block RAM (DPB, DPX9B) swapped.

    bsram_swap.py <in.json> <out.json>

This nextpnr build has no setup arcs on the port-B address pins (ADB[13:0]), and Yosys
puts the READ port of every simple-dual-port memory on B (fetch address -> I-cache and
BHT, load/store address -> D-cache, scan-out address -> framebuffer), so exactly the
critical read-address paths go unchecked (synth/README.md, "How far the timing can be
trusted"). A true-dual-port RAM is symmetric: exchanging every A-side connection and
parameter with its B-side counterpart gives an identical netlist in which the former B
pins are analysed A pins. For timing analysis only; the bitstream comes from the
unmodified netlist.
"""
import json
import sys

PORT_PAIRS = [('ADA', 'ADB'), ('DIA', 'DIB'), ('DOA', 'DOB'), ('CEA', 'CEB'),
              ('CLKA', 'CLKB'), ('OCEA', 'OCEB'), ('RESETA', 'RESETB'),
              ('WREA', 'WREB'), ('BLKSELA', 'BLKSELB')]
PARAM_PAIRS = [('BIT_WIDTH_0', 'BIT_WIDTH_1'), ('BLK_SEL_0', 'BLK_SEL_1'),
               ('READ_MODE0', 'READ_MODE1'), ('WRITE_MODE0', 'WRITE_MODE1')]


def swap(d, a, b):
    va, vb = d.get(a), d.get(b)
    for k, v in ((a, vb), (b, va)):
        if v is None:
            d.pop(k, None)
        else:
            d[k] = v


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    net = json.load(open(sys.argv[1]))
    tops = [m for m, v in net['modules'].items() if v.get('attributes', {}).get('top')]
    if len(tops) != 1:
        sys.exit('bsram_swap.py: expected exactly one top module')
    n = 0
    for cell in net['modules'][tops[0]]['cells'].values():
        if cell['type'] not in ('DPB', 'DPX9B'):
            continue
        for a, b in PORT_PAIRS:
            swap(cell['connections'], a, b)
            swap(cell.setdefault('port_directions', {}), a, b)
        for a, b in PARAM_PAIRS:
            swap(cell['parameters'], a, b)
        n += 1
    json.dump(net, open(sys.argv[2], 'w'))
    print(f'bsram_swap.py: swapped ports A/B of {n} DPB/DPX9B cells')


if __name__ == '__main__':
    main()
