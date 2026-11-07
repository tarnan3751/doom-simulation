#!/usr/bin/env python3
"""Make a private, patched copy of the shared RTL for comparison builds.

    variant.py <out_dir> <patch>[,<patch>...]      copy rtl/{core,mem,periph,soc} to
                                                  <out_dir>/rtl and apply the patches
    variant.py --list                             describe the available patches

The default flow builds the repository RTL unchanged; these patches only produce
the comparison configurations of synth/Makefile (the RTL before the block-RAM
mapping choices, and alternative predictor structures). The shared RTL is never
modified. Every patch is an exact text substitution that must match exactly once,
so a patch fails loudly instead of silently not applying when the RTL changes.
"""
import os
import shutil
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

# name: (description, behaviour, [(file, old, new), ...])
PATCHES = {
    'video_bsram': (
        'palette back in block RAM, framebuffer lanes and palette without no_rw_check '
        '(the video memories as they were before the FPGA mapping choices)',
        'simulation unchanged',
        [('periph/video.sv', f'    (* no_rw_check *) logic [7:0]  fb{i} [0:FB_WORDS-1]', f'    logic [7:0]  fb{i} [0:FB_WORDS-1]')
         for i in range(4)] +
        [('periph/video.sv', '    (* no_rw_check *) (* ram_style = "distributed" *)\n    logic [23:0] palette [0:255]',
          '    logic [23:0] palette [0:255]')]),
    'btb_rwcheck': (
        'BTB without no_rw_check (Yosys then adds read-first emulation in front of the lookup)',
        'simulation unchanged',
        [('core/rv32_bpred.sv', '    (* no_rw_check *) btb_entry_t btb_mem [0:BTB_ENTRIES-1];',
          '    btb_entry_t btb_mem [0:BTB_ENTRIES-1];')]),
    'bht_sync': (
        'BHT read synchronously with the fetch request address (lu_pc) at fetch issue, '
        'instead of asynchronously with f_pc in IF',
        'changes prediction timing (the previous behaviour)',
        [('core/rv32_bpred.sv',
          '        if (bht_we) bht_mem[bht_idx(up_pc)] <= bht_new;\n    end\n'
          '    assign bht_q = bht_mem[bht_idx(f_pc)];     // counter of the instruction in IF',
          '        if (bht_we) bht_mem[bht_idx(up_pc)] <= bht_new;\n'
          '        if (lu_en)  bht_q <= bht_mem[bht_idx(lu_pc)];\n    end')]),
    'bht_bsram': (
        'BHT without the distributed-RAM and no_rw_check attributes (block RAM; use with bht_sync)',
        'simulation unchanged',
        [('core/rv32_bpred.sv', '    (* no_rw_check *) (* ram_style = "distributed" *)\n    logic [1:0] bht_mem [0:BHT_ENTRIES-1];',
          '    logic [1:0] bht_mem [0:BHT_ENTRIES-1];')]),
}


def apply(out_dir, names):
    src = os.path.join(ROOT, 'rtl')
    dst = os.path.join(out_dir, 'rtl')
    if os.path.exists(dst):
        shutil.rmtree(dst)
    for sub in ('core', 'mem', 'periph', 'soc'):
        shutil.copytree(os.path.join(src, sub), os.path.join(dst, sub))
    applied = []
    for name in names:
        if name not in PATCHES:
            sys.exit(f'variant.py: unknown patch {name!r} (see --list)')
        for rel, old, new in PATCHES[name][2]:
            path = os.path.join(dst, rel)
            text = open(path).read()
            n = text.count(old)
            if n != 1:
                sys.exit(f'variant.py: patch {name}: anchor found {n} times in {rel} (expected 1)')
            open(path, 'w').write(text.replace(old, new))
        applied.append(name)
    with open(os.path.join(out_dir, 'PATCHES'), 'w') as f:
        f.write('\n'.join(applied) + '\n')


def main():
    if len(sys.argv) == 2 and sys.argv[1] == '--list':
        for name, (desc, beh, _) in PATCHES.items():
            print(f'{name}\n    {desc}\n    behaviour: {beh}')
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    names = [n for n in sys.argv[2].split(',') if n and n != 'none']
    os.makedirs(sys.argv[1], exist_ok=True)
    apply(sys.argv[1], names)


if __name__ == '__main__':
    main()
