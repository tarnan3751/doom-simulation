#!/usr/bin/env python3
"""Mutation testing for tests/core and tests/mem.

    mutate.py MUTANTS RTL_DIR WORK_DIR CONFIGS [-j N] [NAME ...] -- MAKE_COMMAND ...

For every mutant in MUTANTS and every configuration: copy the *.sv files of RTL_DIR,
replace the mutant's text (it must occur exactly once), then run MAKE_COMMAND with
RTLDIR=<copy> OUT=<its dir> CFG=<configuration>, first for target `mutant-build` (must
succeed), then for `mutant` (the area's short regression). A mutant is caught when
`mutant` fails. It must be caught in exactly the configurations it names: one that names
none must pass (predictor-only bugs, the no-op control). CONFIGS maps the letters used in
MUTANTS to configuration names, e.g. s=sdram_def,f=ideal_tiny. Exit status 0 iff every
outcome is as expected.

MUTANTS: lines starting with '#' are comments. A mutant is a line `<name> <file> <letters|->`
followed by the text to replace (lines starting with '-') and its replacement (lines
starting with '+').
"""
import concurrent.futures
import os
import shutil
import subprocess
import sys


def parse(path):
    muts = []
    for line in open(path).read().splitlines():
        if line[:1] in ('-', '+') and muts:
            muts[-1][line[0]].append(line[1:])
        elif line.strip() and not line.startswith('#'):
            name, f, kill = line.split()
            muts.append({'name': name, 'file': f, 'kill': kill.strip('-'), '-': [], '+': []})
    return muts


def main():
    args, cmd = sys.argv[1:sys.argv.index('--')], sys.argv[sys.argv.index('--') + 1:]
    jobs = int(args.pop(args.index('-j') + 1)) if '-j' in args else 1
    if '-j' in args:
        args.remove('-j')
    mfile, rtl, work, cfgs, *only = args
    cfgs = dict(c.split('=') for c in cfgs.split(','))
    muts = [m for m in parse(mfile) if not only or m['name'] in only]
    env = {k: v for k, v in os.environ.items() if k not in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL')}
    os.makedirs(work, exist_ok=True)

    def run(m, c):
        d = os.path.join(work, f"{m['name']}_{c}")
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d + '/rtl')
        for x in os.listdir(rtl):
            if x.endswith('.sv'):
                shutil.copy(os.path.join(rtl, x), d + '/rtl')
        p, old = os.path.join(d, 'rtl', m['file']), '\n'.join(m['-'])
        s = open(p).read()
        if s.count(old) != 1:
            return None, f"pattern not found exactly once in {m['file']}"
        open(p, 'w').write(s.replace(old, '\n'.join(m['+'])))
        make = cmd + [f'RTLDIR={d}/rtl', f'OUT={d}', f'CFG={cfgs[c]}']
        r = subprocess.run(make + ['mutant-build'], capture_output=True, text=True, env=env)
        if r.returncode:
            return None, 'build failed: ' + (r.stdout + r.stderr)[-200:].replace('\n', ' ')
        r = subprocess.run(make + ['mutant'], capture_output=True, text=True, env=env)
        first = [l for l in (r.stderr + r.stdout).splitlines()
                 if l.startswith('FAIL ') or 'ERROR' in l or 'violation(s) [' in l][:1]
        return r.returncode != 0, first[0][:150] if first else ''

    bad = caught_n = 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(run, m, c): (m, c) for m in muts for c in cfgs}
        for f in concurrent.futures.as_completed(futs):
            m, c = futs[f]
            caught, info = f.result()
            want = c in m['kill']
            ok = caught is not None and caught == want
            bad += not ok
            caught_n += bool(caught)
            if ok:
                shutil.rmtree(os.path.join(work, f"{m['name']}_{c}"), ignore_errors=True)
            verdict = 'ERROR' if caught is None else 'caught' if caught else 'not caught'
            print(f"{'ok ' if ok else 'BAD'}  {m['name']:34s} [{c}] {verdict:10s} "
                  f"(expected {'caught' if want else 'not caught'})  {info}", flush=True)
    n = len(futs)
    print(f"{'PASS' if not bad else 'FAIL'}  mutation: {len(muts)} mutants x {len(cfgs)} configuration(s): "
          f"{caught_n} caught, {n - caught_n} not caught, {bad} not as expected")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
