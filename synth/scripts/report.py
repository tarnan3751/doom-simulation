#!/usr/bin/env python3
"""Build <out_dir>/report.md from the products of synth/Makefile.

    report.py <out_dir> <config>...

Reads per configuration config.txt, stat.txt, libmap.log, synth.json and
<view>_s<seed>/{rc,report.json,pnr.log,dsp_routes.json}, plus probes/probes.json
and tb/tb.log. Everything printed is measured; the design-terms path labels come
from PATH_CLASSES, which only matches start/end points whose meaning was checked
by hand (unknown ones are shown raw).
"""
import collections
import datetime
import glob
import json
import os
import re
import statistics
import subprocess
import sys

LUT4_TOTAL, DFF_TOTAL = 20736, 15552
BSRAM_TYPES = ('DPB', 'DPX9B', 'SDPB', 'SDPX9B', 'SP', 'SPX9', 'pROM', 'pROMX9')
FF_TYPES = tuple(f'DFF{n}{k}{e}' for n in ('', 'N') for k in ('', 'S', 'R', 'P', 'C') for e in ('', 'E'))
DSP_TYPES = ('MULT36X36', 'MULT18X18', 'MULT9X9', 'ALU54D', 'MULTALU18X18', 'MULTALU36X18', 'MULTADDALU18X18')

CFG_TEXT = {
    'main':    'repository RTL as it is, board top with DVI (bitstream)',
    'nodvi':   'repository RTL, no video output (DVI=0: scan-out, framebuffer, palette removed)',
    'core':    'rv32_core alone + 8 KB TCM (measurement wrapper)',
    'pll1':    'main with the one-PLL clock fallback (PLL_CASCADE=0)',
    'prior':   'RTL before its block-RAM mapping choices (palette and BHT in block RAM, no no_rw_check)',
    'syncbht': 'main with the BHT read synchronously at fetch issue instead of in IF',
}

EX = r'u_core\.(m_result|w_result|e_\w+|u_regfile\.wdata)'
# (start regex, end regex, label). Start/end are "cell.port" strings from the report.
PATH_CLASSES = [
    (r'u_tags\.mem\.\S*\.DO', r'u_video\.fb\d\.\S*\.(WRE|CE|AD|DI)',
     'I-cache tag RAM -> tag compare (hit) -> i_rsp_ready -> core `advance` -> dmem_req_valid '
     '-> soc_top data-port decode -> MMIO accept -> framebuffer write enable/address'),
    (r'u_tags\.mem\.\S*\.DO', r'u_video\.palette',
     'I-cache tag RAM -> hit -> i_rsp_ready -> core `advance` -> dmem_req_valid -> MMIO -> palette write'),
    (r'u_tags\.mem\.\S*\.DO', r'u_core\.',
     'cache tag RAM -> hit -> rsp_ready -> core `advance` -> pipeline-register enables'),
    (r'u_tags\.mem\.\S*\.DO', r'u_(icache|dcache|tags|mem)',
     'cache tag RAM -> hit -> rsp_ready -> core `advance`/req_valid -> cache RAM enable/address'),
    (EX, r'u_bpred\.bht_mem\S*\.(WRE|WAD|DI)',
     'EX: forwarding mux -> branch / exception resolution -> BHT update (LUT-RAM write enable)'),
    (r'u_core\.d_insn', r'u_core\.e_tgt_match',
     'ID: instruction word -> immediate -> pc + imm (carry chain) -> compare with the predicted target -> e_tgt_match'),
    (r'u_bpred\.btb_mem\S*\.DO', r'u_core\.bp_cnt',
     'IF: BTB RAM output -> BTB hit -> bp_taken -> no-redirect next PC (npc_seq) -> BHT LUT-RAM read -> BHT output register'),
    (r'u_core\.f_pc', r'u_icache\.|u_tags\.|u_bpred\.btb',
     'IF: f_pc -> BHT LUT-RAM read (in IF) -> prediction (bp_taken) -> npc mux -> I-cache / BTB address'),
    (EX, r'u_core\.bp_cnt|u_bpred\.(bht|btb)',
     'EX: forwarding mux -> branch compare / jalr target -> mispredict/redirect -> next-fetch-address mux '
     '(npc) -> branch-predictor (BHT/BTB) read -> its output register'),
    (EX, r'u_bpred\.ras',
     'EX: forwarding mux -> branch resolution -> redirect -> npc -> return-address-stack / prediction registers'),
    (EX, r'u_core\.f_pc',
     'EX: forwarding mux -> branch resolution -> redirect -> npc mux -> IF PC register (f_pc)'),
    (EX, r'u_csr\.',
     'EX: forwarding mux -> exception / branch logic -> CSR (trap) write enables'),
    (EX, r'u_icache\.|u_tags\.|u_icache\b',
     'EX: forwarding mux -> branch resolution -> redirect -> npc -> I-cache data/tag RAM address or enable'),
    (EX, r'u_dcache\.',
     'EX: forwarding mux -> address adder (agu) -> D-cache data RAM address'),
    (EX, r'u_video\.fb',
     'EX: forwarding mux -> address adder (agu) -> MMIO decode -> framebuffer write address/enable'),
    (r'u_bpred\.btb_mem\S*\.DO', r'u_core\.',
     'IF: BTB RAM output -> tag compare (BTB hit) -> prediction -> RAS / next-fetch logic'),
    (r'u_core\.', r'u_core\.',
     'inside rv32_core (register to register)'),
    (r'(u_mem\.|u_icache|u_dcache)', r'u_core\.',
     'memsys (cache response / ready) -> rv32_core'),
    (r'tcm\.', r'u_core\.', 'TCM read data -> rv32_core'),
    (r'u_core\.', r'tcm\.', 'rv32_core request -> TCM address/enable'),
    (r'u_enc\.', r'u_enc\.|u_dvi\.', 'DVI pixel domain: FIFO read data -> TMDS encoder'),
]


def cls(start, end):
    for s, e, label in PATH_CLASSES:
        if re.search(s, start) and re.search(e, end):
            return label
    return '(unclassified)'


def rtl_name(cell_port):
    """Turn a placed cell name into the RTL object it implements (best effort)."""
    x = re.sub(r'(_DFF\w*?_Q(_\d+)?|_passthrough_lut\$|\$conn\$D)(\.\w+)?$', '', cell_port)
    return x


def top_cells(net):
    """Cells of the top module of a Yosys JSON netlist."""
    return next(v for v in net['modules'].values() if v.get('attributes', {}).get('top'))['cells']


def load(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def read(path, default=''):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


# ---------------------------------------------------------------------- Yosys products
def stat_counts(text):
    """Cell counts of the top module from Yosys `stat` output."""
    c = {}
    sect = re.split(r'\n=== ', text)
    body = sect[-1] if len(sect) > 1 else text
    for m in re.finditer(r'^\s+(\d+)\s+([A-Za-z0-9_$]+)\s*$', body, re.M):
        c[m.group(2)] = int(m.group(1))
    return c


def summarise_counts(c):
    lut = sum(c.get(f'LUT{i}', 0) for i in range(1, 5))
    return {
        'LUT1-4': lut,
        'ALU': c.get('ALU', 0),
        'MUX2': sum(c.get(k, 0) for k in ('MUX2_LUT5', 'MUX2_LUT6', 'MUX2_LUT7', 'MUX2_LUT8')),
        'FF': sum(c.get(k, 0) for k in FF_TYPES),
        'RAM16SDP4': c.get('RAM16SDP4', 0),
        'BSRAM': sum(c.get(k, 0) for k in BSRAM_TYPES),
        'BSRAM_by_type': {k: c[k] for k in BSRAM_TYPES if c.get(k)},
        'DSP': sum(c.get(k, 0) for k in DSP_TYPES),
    }


def libmap(text):
    """Per memory: chosen mapping, and the emulation the cheapest candidate of that kind needs."""
    mems, chosen, cur, cand = {}, {}, None, None
    for ln in text.splitlines():
        m = re.match(r'Memory (\S+) mapping candidates \(post-geometry\):', ln)
        if m:
            cur = m.group(1); mems[cur] = []; cand = None; continue
        m = re.match(r'mapping memory (\S+) via (\S+)', ln)
        if m:
            chosen[m.group(1)] = m.group(2); cur = None; continue
        if cur is None:
            continue
        if ln.startswith('- '):
            cand = {'type': ln[2:].rstrip(':').replace('logic fallback', 'logic'), 'lines': []}
            mems[cur].append(cand)
        elif cand is not None:
            cand['lines'].append(ln.strip())
    out = {}
    for name, cands in mems.items():
        ch = chosen.get(name, 'logic (flops)')
        def cost(c):
            for l in c['lines']:
                if l.startswith('- cost:'):
                    return float(l.split()[-1])
            return 1e18
        sel = [c for c in cands if c['type'] == ch]
        best = min(sel, key=cost) if sel else None
        emu = sorted(set(l[2:] for l in best['lines'] if l.startswith('- emulate'))) if best else []
        out[name.split('.', 1)[1] if '.' in name else name] = {'via': ch, 'emulation': emu}
    return out


def netlist_mem_blocks(net):
    """Hard memory cells per inferred memory (by cell-name prefix) in a Yosys JSON netlist."""
    blocks = collections.defaultdict(collections.Counter)
    for cn, c in top_cells(net).items():
        t = c['type']
        if t in BSRAM_TYPES or t == 'RAM16SDP4':
            mem = re.sub(r'(\.\d+)+$', '', cn)
            blocks[mem][t] += 1
    return blocks


GROUPS = [
    ('u_soc.u_core.u_bpred', 'core: branch predictor (BTB, BHT, RAS)'),
    ('u_soc.u_core.u_regfile', 'core: register file'),
    ('u_soc.u_core.u_muldiv', 'core: mul/div unit'),
    ('u_soc.u_core.u_csr', 'core: CSRs and counters'),
    ('u_soc.u_core', 'core: pipeline, decode, ALU, forwarding'),
    ('u_soc.u_mem.u_icache', 'memsys: I-cache'),
    ('u_soc.u_mem.u_dcache', 'memsys: D-cache'),
    ('u_soc.u_mem.u_tags', 'memsys: shared tag RAM'),
    ('u_soc.u_mem.u_arb', 'memsys: line arbiter'),
    ('u_soc.u_mem.g_sdram', 'memsys: SDRAM controller'),
    ('u_soc.u_mem', 'memsys: other'),
    ('u_soc.u_mmio.u_video', 'mmio: video (framebuffer, palette, scan-out)'),
    ('u_soc.u_mmio.u_uart', 'mmio: UART'),
    ('u_soc.u_mmio.u_kbd', 'mmio: keyboard FIFO'),
    ('u_soc.u_mmio.u_sys', 'mmio: system control'),
    ('u_soc.u_mmio', 'mmio: decode, response'),
    ('u_soc', 'soc_top glue'),
    ('g_dvi.u_dvi.u_enc.u_cdc', 'board: DVI clock-crossing FIFO'),
    ('g_dvi.u_dvi', 'board: DVI TMDS encoders + serialisers'),
    ('u_clk', 'board: PLLs, CLKDIV'),
    ('u_rst_', 'board: reset generators'),
    ('tcm', 'wrapper: TCM'),
]
# the core-only configuration has u_core at the top
GROUPS += [(p.replace('u_soc.', '', 1), lab) for p, lab in GROUPS if p.startswith('u_soc.u_core')]


def breakdown(net):
    rows = collections.OrderedDict((label, collections.Counter()) for _, label in GROUPS)
    rows['board: top level (I/O buffers, LEDs, ...)'] = collections.Counter()
    for cn, c in top_cells(net).items():
        hd = c.get('attributes', {}).get('hdlname')
        path = hd.replace(' ', '.') if hd else cn
        label = 'board: top level (I/O buffers, LEDs, ...)'
        for pre, lab in GROUPS:
            if path.startswith(pre + '.') or path.startswith(pre + '[') or (pre.endswith('_') and path.startswith(pre)):
                label = lab
                break
        t = c['type']
        kind = ('LUT1-4' if t.startswith('LUT') else 'MUX2' if t.startswith('MUX2_') else
                'FF' if t in FF_TYPES else 'BSRAM' if t in BSRAM_TYPES else 'DSP' if t in DSP_TYPES else
                t if t in ('ALU', 'RAM16SDP4') else None)
        if kind:
            rows[label][kind] += 1
    return {k: v for k, v in rows.items() if sum(v.values())}


# ---------------------------------------------------------------------- nextpnr products
def runs_of(out, cfg):
    res = []
    for d in sorted(glob.glob(os.path.join(out, cfg, '*_s*'))):
        m = re.match(r'(pnr|swap)_s(\d+)$', os.path.basename(d))
        if not m or not os.path.exists(os.path.join(d, 'done')):
            continue
        rc = read(os.path.join(d, 'rc'), '?').strip()
        rep = load(os.path.join(d, 'report.json'))
        log = read(os.path.join(d, 'pnr.log'))
        run = {'view': m.group(1), 'seed': int(m.group(2)), 'ok': rc == '0' and rep is not None}
        if not run['ok']:
            errs = [l for l in log.splitlines() if l.startswith('ERROR')]
            run['error'] = errs[-1] if errs else f'nextpnr exit code {rc}'
            res.append(run)
            continue
        run['fmax'] = {k: v['achieved'] for k, v in rep.get('fmax', {}).items()}
        run['util'] = rep.get('utilization', {})
        run['paths'] = []
        for cp in rep.get('critical_paths', []):
            p = cp['path']
            logic = sum(x['delay'] for x in p if x['type'] != 'routing')
            route = sum(x['delay'] for x in p if x['type'] == 'routing')
            levels = sum(1 for x in p if x['type'] == 'logic')
            start = p[0]['from']['cell'] + '.' + p[0]['from']['port']
            end = p[-1]['to']['cell'] + '.' + p[-1]['to']['port']
            run['paths'].append({'from': cp['from'], 'to': cp['to'], 'delay': logic + route,
                                 'logic': logic, 'route': route, 'levels': levels,
                                 'start': start, 'end': end, 'raw': p})
        run['dsp'] = load(os.path.join(d, 'dsp_routes.json'), [])
        res.append(run)
    return res


def sysclk(run):
    """Name of the system clock in a run: clk_sys, or clk_sdram when the two nets are one
    (one-PLL fallback: clk_sdram = clk_sys and nextpnr keeps the alias name), or the only clock."""
    f = run.get('fmax', {})
    for k in ('clk_sys', 'clk_sdram'):
        if k in f:
            return k
    rest = [k for k in f if k != 'clk_pix']
    return rest[0] if rest else 'clk_sys'


def sys_path(run, clk='clk_sys'):
    for p in run.get('paths', []):
        if p['from'] == f'posedge {clk}' and p['to'] == f'posedge {clk}':
            return p
    return None


def per_seed_worst(runs):
    """{seed: worse clk_sys Fmax of the views routed for that seed} (a seed counts only
    when every view of it routed)."""
    by = {}
    for r in runs:
        by.setdefault(r['seed'], []).append(r)
    out = {}
    for seed, rr in by.items():
        if all(r['ok'] and sysclk(r) in r['fmax'] for r in rr):
            out[seed] = min(r['fmax'][sysclk(r)] for r in rr)
    return out


def fmt_spread(vals):
    if not vals:
        return 'n/a'
    vals = sorted(vals)
    return f'{vals[-1]:.1f} / {statistics.median(vals):.1f} / {vals[0]:.1f}'


def hidden_mark(run, path, net):
    """' (hidden in pnr view)' if a swap-view path ends on a DP BSRAM port-A address pin,
    i.e. on what is a port-B address (not analysed) in the real netlist."""
    if run['view'] != 'swap' or net is None:
        return ''
    cell, port = path['end'].rsplit('.', 1)
    c = top_cells(net).get(cell)
    if c and c['type'] in ('DPB', 'DPX9B') and port.startswith('ADA'):
        return ' **(ends on a port-B read address of the real netlist: invisible in the pnr view)**'
    return ''


def path_sources(path, net):
    """RTL source lines (file:line) of the cells along a path, in order, de-duplicated."""
    if net is None:
        return []
    cells = top_cells(net)
    out = []
    for x in path['raw']:
        if x['type'] == 'routing':
            continue
        c = cells.get(x['from']['cell'])
        if not c:
            continue
        for s in c.get('attributes', {}).get('src', '').split('|'):
            m = re.search(r'rtl/((?:core|mem|periph|soc|fpga)/\w+\.sv):(\d+)', s)
            if m:
                tag = f'{m.group(1)}:{m.group(2)}'
                if tag not in out:
                    out.append(tag)
    return out


# ---------------------------------------------------------------------- report
def main():
    out = sys.argv[1]
    cfgs = sys.argv[2:]
    L = []
    w = L.append
    COUNT_COLS = ('LUT1-4', 'ALU', 'MUX2', 'FF', 'RAM16SDP4', 'BSRAM', 'DSP')

    def counts_row(name, c):
        w(f'| {name} | ' + ' | '.join(str(c[k]) for k in COUNT_COLS) + ' |')

    def head(cols):
        w(f'| {cols} |')
        w('|' + '---|' * (cols.count(' | ') + 1))
    now = datetime.datetime.now().strftime('%Y-%m-%d %H:%M')
    bindir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '.tools', 'oss-cad-suite', 'bin')

    def version(tool, flag):
        try:
            r = subprocess.run([os.path.join(bindir, tool), flag], capture_output=True, text=True)
            return (r.stdout + r.stderr).strip().splitlines()[0]
        except (OSError, IndexError):
            return f'{tool} (version unknown)'
    yv, nv = version('yosys', '-V'), version('nextpnr-himbaechel', '--version')

    data = {}
    for cfg in cfgs:
        d = os.path.join(out, cfg)
        conf = dict(l.split('=', 1) for l in read(os.path.join(d, 'config.txt')).splitlines() if '=' in l)
        net = load(os.path.join(d, 'synth.json'))
        data[cfg] = {
            'conf': conf,
            'stat': summarise_counts(stat_counts(read(os.path.join(d, 'stat.txt')))),
            'mem': libmap(read(os.path.join(d, 'libmap.log'))),
            'blocks': netlist_mem_blocks(net) if net else {},
            'breakdown': breakdown(net) if net else {},
            'runs': runs_of(out, cfg),
            'net': net,
        }

    w('# Tang Nano 20K implementation report (generated)\n')
    w(f'Generated {now} by `synth/scripts/report.py` from `build/synth` (`make -C synth`).  ')
    w(f'Tools: {yv}; {nv}.  ')
    w('Device: GW2AR-LV18QN88C8/I7 (20 736 LUT4, 15 552 FF, 46 BSRAM, 48 DSP18, 2 rPLL). '
      'Target: clk_sys 50.4 MHz (19.84 ns), clk_pix 25.2 MHz. **Never tested on hardware.**\n')
    w('How far to trust the Fmax numbers: section 7. Interpretation: synth/README.md.\n')

    # ---- 1 summary
    w('## 1. Summary per configuration\n')
    w('Fmax columns: best / median / worst over seeds, MHz. "pnr" = the netlist as built '
      '(the bitstream); "swap" = same netlist with every true-dual-port BSRAM\'s ports A/B exchanged, '
      'which times the read-address paths the pnr view cannot see (section 7). The quoted Fmax of a seed '
      'is the worse of its two views (bold column); a configuration meets 50.4 MHz if that holds for '
      'every seed.\n')
    head('config | what | BSRAM | LUT4 (nextpnr) | FF | RAM16SDP4 | DSP | P&R | **clk_sys, worse view per seed** | clk_sys pnr | clk_sys swap | clk_pix worst')
    for cfg in cfgs:
        D = data[cfg]
        st = D['stat']
        runs = D['runs']
        okr = [r for r in runs if r['ok']]
        u = okr[0]['util'] if okr else {}
        lut = u.get('LUT4', {}).get('used')
        ff = u.get('DFF', {}).get('used')
        if lut is not None:
            lut = f"{lut} ({100.0 * lut / LUT4_TOTAL:.0f} %)"
        if ff is not None:
            ff = f"{ff} ({100.0 * ff / DFF_TOTAL:.0f} %)"
        pn = [r['fmax'][sysclk(r)] for r in okr if r['view'] == 'pnr' and sysclk(r) in r['fmax']]
        sw = [r['fmax'][sysclk(r)] for r in okr if r['view'] == 'swap' and sysclk(r) in r['fmax']]
        px = [r['fmax'].get('clk_pix') for r in okr if r['fmax'].get('clk_pix')]
        fails = [r for r in runs if not r['ok']]
        pr = f'{len(okr)}/{len(runs)} ok' if runs else 'not run'
        if fails and not okr:
            pr = 'FAILS: ' + fails[0]['error'].replace('|', '/')[:90]
        bram = f"{st['BSRAM']}/46" + (' **does not fit**' if st['BSRAM'] > 46 else '')
        comb = list(per_seed_worst(runs).values()) if 'swap' in D['conf'].get('views', '') else []
        w(f"| {cfg} | {CFG_TEXT.get(cfg, '')} | {bram} | "
          f"{lut if lut is not None else '(' + str(st['LUT1-4'] + st['ALU']) + ' yosys)'} | "
          f"{ff if ff is not None else st['FF']} | {st['RAM16SDP4']} | {st['DSP']} | {pr} | "
          f"**{fmt_spread(comb)}** | {fmt_spread(pn)} | {fmt_spread(sw)} | "
          f"{f'{min(px):.1f}' if px else 'n/a'} |")
    w('')
    for cfg in cfgs:
        p = data[cfg]['conf'].get('patches', 'none')
        g = data[cfg]['conf'].get('params', '')
        if (p and p != 'none') or g:
            w(f'* `{cfg}`: ' + (f'patches `{p}` (scripts/variant.py, applied to a private copy of the RTL)'
                               if p and p != 'none' else 'RTL unchanged') +
              (f', parameters `{g}`' if g else ''))
    w('')
    w('Yosys cell counts per configuration (synth_gowin, flattened netlist). nextpnr\'s LUT4 above '
      'is LUT positions in use (LUT cells, the LUT half of ALU cells, pass-through LUTs it inserts) and '
      'its DFF excludes the flops it moved into IOBs:\n')
    head('config | LUT1-4 | ALU | MUX2_LUT5-8 | FF | RAM16SDP4 | BSRAM | DSP')
    for cfg in cfgs:
        st = data[cfg]['stat']
        counts_row(cfg, st)
    w('')

    # ---- 2 utilisation of the recommended configuration
    main_cfg = 'main' if 'main' in data else cfgs[0]
    okr = [r for r in data[main_cfg]['runs'] if r['ok'] and r['view'] == 'pnr']
    if okr:
        r0 = okr[0]
        w(f'## 2. Device utilisation, `{main_cfg}` (nextpnr, seed {r0["seed"]})\n')
        head('resource | used | available | %')
        for k, v in r0['util'].items():
            if v.get('used'):
                pct = f"{100.0 * v['used'] / v['available']:.1f}" if v.get('available') else '-'
                w(f"| {k} | {v['used']} | {v['available']} | {pct} |")
        w('')
        w('LUT4 = LUT positions in use (LUT cells, the LUT half of ALU cells, pass-through LUTs). '
          'RAM16SDP4 (LUT RAM) has its own count of sites; each site is part of a logic tile, so the real '
          'logic headroom is somewhat smaller than the LUT4 percentage suggests. DFF excludes the flops in '
          'IOBs (IOLOGICI/IOLOGICO).\n')

    # ---- 3 per module
    w('## 3. Resources per module (Yosys cells, flattened netlist)\n')
    w('Attribution by the instance-path prefix of each cell name. Yosys names a flop after its '
      'output net and a mapped LUT after a neighbouring net, so cells at module boundaries (and '
      'registers that drive a module output) can land in the parent or a neighbour: the split is '
      '**approximate** for LUT/ALU/MUX2/FF and exact only for RAM16SDP4, BSRAM and DSP cells. Exact '
      'numbers for the core alone: configuration `core`.\n')
    for cfg in [c for c in (main_cfg, 'core') if c in data]:
        bd = data[cfg]['breakdown']
        if not bd:
            continue
        w(f'### `{cfg}`\n')
        head('module | LUT1-4 | ALU | MUX2_LUT5-8 | FF | RAM16SDP4 | BSRAM | DSP')
        tot = collections.Counter()
        for k, v in bd.items():
            tot.update(v)
            counts_row(k, v)
        counts_row('**total**', tot)
        w('')

    # ---- 4 memories
    w('## 4. Memory inference\n')
    for cfg in [c for c in (main_cfg, 'prior') if c in data]:
        D = data[cfg]
        w(f'### `{cfg}`\n')
        head('memory | mapped to | cells | read-during-write / other emulation added by Yosys')
        for name, m in sorted(D['mem'].items()):
            blk = D['blocks'].get(name, {})
            cells = ', '.join(f'{n} {t}' for t, n in sorted(blk.items())) or ('flops' if 'logic' in m['via'] else '-')
            emu = [e for e in m['emulation'] if e != 'emulate data register']
            w(f"| {name} | {m['via'].replace('$__GOWIN_', '').rstrip('_')} | {cells} | {', '.join(emu) or '-'} |")
        st = D['stat']
        w(f"\nBlock RAM total: **{st['BSRAM']} of 46** ({', '.join(f'{n} {t}' for t, n in st['BSRAM_by_type'].items())}).\n")

    # ---- 5 timing per run
    w('## 5. Timing per run (clk_sys)\n')
    head('config | view | seed | clk_sys MHz | clk_pix MHz | critical path ns (logic + route, levels) | start -> end | path class')
    for cfg in cfgs:
        for r in data[cfg]['runs']:
            if not r['ok']:
                w(f"| {cfg} | {r['view']} | {r['seed']} | failed | | | {r['error'][:100]} | |")
                continue
            clk = sysclk(r)
            p = sys_path(r, clk)
            fs = r['fmax'].get(clk)
            fp = r['fmax'].get('clk_pix')
            if p:
                w(f"| {cfg} | {r['view']} | {r['seed']} | {fs:.1f} | {f'{fp:.1f}' if fp else '-'} | "
                  f"{p['delay']:.2f} ({p['logic']:.2f} + {p['route']:.2f}, {p['levels']}) | "
                  f"`{rtl_name(p['start'])}` -> `{rtl_name(p['end'])}` | "
                  f"{cls(p['start'], p['end'])}{hidden_mark(r, p, data[cfg]['net'])} |")
    w('')

    w('Per seed, the quoted Fmax is the worse of the two views:\n')
    head('config | seed | pnr MHz | swap MHz | **quoted (worse)**')
    for cfg in cfgs:
        runs = data[cfg]['runs']
        if 'swap' not in data[cfg]['conf'].get('views', ''):
            continue
        worst = per_seed_worst(runs)
        for seed in sorted(worst):
            v = {r['view']: r['fmax'][sysclk(r)] for r in runs if r['seed'] == seed and r['ok']}
            w(f"| {cfg} | {seed} | {v.get('pnr', 0):.1f} | {v.get('swap', 0):.1f} | **{worst[seed]:.1f}** |")
    w('')

    # ---- 6 worst paths in detail
    w('## 6. Worst clk_sys path per configuration and view\n')
    for cfg in cfgs:
        D = data[cfg]
        for view in ('pnr', 'swap'):
            rr = [r for r in D['runs'] if r['ok'] and r['view'] == view and sys_path(r, sysclk(r))]
            if not rr:
                continue
            worst = max(rr, key=lambda r: sys_path(r, sysclk(r))['delay'])
            p = sys_path(worst, sysclk(worst))
            w(f"### `{cfg}`, {view} view, seed {worst['seed']}: {p['delay']:.2f} ns ({1000.0 / p['delay']:.1f} MHz)\n")
            w(f"* from `{p['start']}`  \n  to `{p['end']}`")
            w(f"* class: {cls(p['start'], p['end'])}{hidden_mark(worst, p, D['net'])}")
            w(f"* {p['levels']} logic elements, {p['logic']:.2f} ns logic + {p['route']:.2f} ns routing")
            srcs = path_sources(p, D['net'])
            if srcs:
                w(f"* RTL lines on the path (cells that kept a source attribute): {', '.join(srcs)}")
            w('')

    # ---- 7 trust
    w('## 7. What this nextpnr build does not analyse\n')
    pr = load(os.path.join(out, 'probes', 'probes.json'))
    if pr:
        ref = pr[0]['crit_ns']
        w(f'Probe designs (`make probes`, synth/probes/timing_probe.sv): a ~{ref:.1f} ns adder cone drives (or is '
          'driven by) exactly one pin group of a hard block. "analysed = NO" means the cone disappeared from '
          'the timing analysis (a trivial path was reported instead).\n')
        head('probe | cell | cone attaches to | reported critical ns | analysed')
        for x in pr:
            w(f"| {x['mode']} | {x['hard'] or 'fabric'} | {','.join(x['pins']) or 'flop D'} | {x['crit_ns']:.2f} | "
              f"{'yes' if x['analysed'] else '**NO**'} |")
        w('')
    # DSP route budget
    for cfg in cfgs:
        rr = [r for r in data[cfg]['runs'] if r['ok'] and r['view'] == 'pnr' and r.get('dsp')]
        if not rr:
            continue
        rows = []
        for r in rr:
            ins = [x for x in r['dsp'] if x['dir'] == 'in' and not any(k in str(x.get('other_type', '')) for k in ('GND', 'VCC'))]
            outs = [x for x in r['dsp'] if x['dir'] == 'out']
            if not ins or not outs:
                continue
            ri = max(x['route_ns'] for x in ins)
            ro = max(x['route_ns'] for x in outs)
            drv = sorted(set(str(x['other_type']) for x in ins))
            snk = sorted(set(str(x['other_type']) for x in outs))
            rows.append((r['seed'], ri, ro, drv, snk))
        if rows:
            w(f'DSP (MULT36X36, rv32_muldiv `mul_a`/`mul_b` -> `mul_p`) in `{cfg}`: no timing arcs, so '
              'nextpnr reports nothing. Measured routing around it (post-route hook):\n')
            head('seed | worst route into DSP ns | worst route out of DSP ns | drivers | sinks | budget left for the multiplier at 19.84 ns')
            for seed, ri, ro, drv, snk in rows:
                budget = 19.841 - 0.23 - ri - ro - 0.04 - 0.37
                w(f"| {seed} | {ri:.2f} | {ro:.2f} | {', '.join(drv)} | {', '.join(snk)} | {budget:.2f} ns |")
            w('\n(budget = period - flop clk-to-q 0.23 - routes - pass-through LUT 0.37 - setup 0.04; the '
              'multiplier\'s own combinational delay must fit in it and is not known to this tool chain.)\n')
            break

    # ---- 8 cross domain
    w('## 8. Clock-domain crossings\n')
    w('nextpnr reports the paths between clk_sys and clk_pix separately and does not count them in '
      'either clock\'s Fmax. They exist only inside tn20k_vid_cdc (Gray-pointer synchronisers, LUT-RAM '
      'read of entries written >= 2 read cycles earlier) and are asynchronous by design.\n')
    head('config | view | seed | from -> to | delay ns | start -> end')
    for cfg in cfgs:
        for r in data[cfg]['runs'][:2]:
            for p in r.get('paths', []):
                if p['from'].split()[-1] != p['to'].split()[-1]:
                    w(f"| {cfg} | {r['view']} | {r['seed']} | {p['from']} -> {p['to']} | {p['delay']:.2f} | "
                      f"`{rtl_name(p['start'])}` -> `{rtl_name(p['end'])}` |")
    w('')

    # ---- 9 IO
    w('## 9. I/O\n')
    for cfg in [c for c in (main_cfg,) if c in data]:
        okr = [r for r in data[cfg]['runs'] if r['ok'] and r['view'] == 'pnr']
        if okr:
            u = okr[0]['util']
            w(f"`{cfg}`: IOB {u.get('IOB', {}).get('used')}, IOLOGICI (input registers / IDDR in the IOB) "
              f"{u.get('IOLOGICI', {}).get('used')}, IOLOGICO (output registers, ODDR, OSER10) "
              f"{u.get('IOLOGICO', {}).get('used')}. The in-package SDRAM pins are placed by name "
              '(O_sdram_*, IO_sdram_dq) from the chip database SiP map. nextpnr has no I/O timing model for '
              'this device (no input/output delay constraints), so the SDRAM interface timing is reasoned in '
              'synth/README.md, not measured.\n')

    # ---- 10 tests
    tb = read(os.path.join(out, 'tb', 'tb.log'))
    if tb:
        w('## 10. Simulation of the FPGA-only RTL (`make tb`)\n')
        w('```')
        w(tb.strip())
        w('```\n')

    bit = os.path.join(out, 'tangnano20k.fs')
    if os.path.exists(bit):
        w(f'Bitstream: `build/synth/tangnano20k.fs` ({os.path.getsize(bit)} bytes), from `{main_cfg}` '
          'by gowin_pack. Never loaded into a device.\n')

    with open(os.path.join(out, 'report.md'), 'w') as f:
        f.write('\n'.join(L) + '\n')


if __name__ == '__main__':
    main()
