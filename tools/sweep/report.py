#!/usr/bin/env python3
"""Collect build/sweep/runs/*, write build/sweep/results.json, SVG charts and the tables block of docs/results/cache-sweep.md."""
import json, math, os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
RUNS = f"{ROOT}/build/sweep/runs"
DOC = f"{ROOT}/docs/results/cache-sweep.md"
BASELINE_CYCLES = 2458661186  # default point, plain --engine accurate, docs/results/timedemo.md (a --cosim run adds a 64-cycle tail drain)
REF_FRAMES = 2173

cfgs = []
for line in open(f"{ROOT}/tools/sweep/configs.txt"):
    if line.startswith("#") or not line.strip():
        continue
    n, i, d = line.split()
    cfgs.append((n, int(i), int(d)))

def rd(p):
    try:
        return open(p, errors="replace").read()
    except OSError:
        return ""

rows = []
for n, i, d in cfgs:
    r = {"name": n, "icache_kb": i, "dcache_kb": d}
    run = f"{RUNS}/{n}"
    try:
        s = json.load(open(f"{run}/stats.json"))
    except Exception:
        s = None
    cmp_txt = rd(f"{run}/cmp.txt")
    m = re.search(r"compared (\d+) frames: (\d+) identical, (\d+) differing", cmp_txt)
    r["run_exit"] = int(rd(f"{run}/run_exit").strip() or -1)
    r["cmp_exit"] = int(rd(f"{run}/cmp_exit").strip() or -1)
    r["frames_identical"] = bool(m and m.group(1) == m.group(2) == str(REF_FRAMES) and m.group(3) == "0" and "IDENTICAL" in cmp_txt and r["cmp_exit"] == 0)
    if s:
        r.update(cycles=s["cycles"], instret=s["instret"], ipc=s["ipc"], frames=s["frames"],
                 ic_miss_rate=s["ic_miss"] / s["ic_access"], dc_miss_rate=s["dc_miss"] / s["dc_access"],
                 cycles_per_frame=(s["last_frame_cycle"] - s["first_frame_cycle"]) / (s["frames"] - 1), fps=s["fps_span"], sdram_violations=s["sdram_violations"],
                 stop=s["stop"])
    t = re.search(r"timed (\d+) gametics in (\d+) realtics \(([\d.]+) fps\)", rd(f"{run}/serial.txt"))
    r["doom_line"] = t.group(0) if t else None
    r["doom_fps"] = float(t.group(3)) if t else None
    r["ok"] = bool(s) and r["run_exit"] == 0 and r["frames_identical"] and s["sdram_violations"] == 0 and s["frames"] == REF_FRAMES
    ib, db = math.ceil(i / 2), math.ceil(d / 2)
    r["bram"] = 1 + ib + db  # cache data blocks + 1 for the tags of both caches
    rows.append(r)

by = {r["name"]: r for r in rows}
default = by["i8_d16"]
default_ok_cycles = default.get("cycles") == BASELINE_CYCLES
json.dump({"rows": rows, "default_reproduces_baseline": default_ok_cycles}, open(f"{ROOT}/build/sweep/results.json", "w"), indent=1)

def kb(x): return f"{x} KB"
def fmt_row(r):
    if "cycles" not in r:
        return f"| {kb(r['icache_kb'])} | {kb(r['dcache_kb'])} | (no result) |" + " |" * 10
    dfps = (r["fps"] / default["fps"] - 1) * 100 if "fps" in default else 0
    return (f"| {kb(r['icache_kb'])} | {kb(r['dcache_kb'])} | {r['cycles']:,} | {r['instret']:,} | {r['ipc']:.3f} | "
            f"{r['ic_miss_rate']*100:.2f} % | {r['dc_miss_rate']*100:.2f} % | {r['cycles_per_frame']:,.0f} | "
            f"{r['fps']:.2f} ({dfps:+.1f} %) | {r['doom_fps'] if r['doom_fps'] is not None else '?'} | "
            f"{'yes' if r['ok'] else 'NO'} | {r['bram']} |")
HDR = ("| I-cache | D-cache | Cycles | Instructions | IPC | I miss | D miss | Cycles/frame | fps @ 50 MHz (vs default) | Doom \"fps\" | Frames = reference, exit 0, 0 SDRAM violations | Cache block RAMs |\n"
       "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|---:|")
def table(names):
    return HDR + "\n" + "\n".join(fmt_row(by[n]) for n in names)

isweep = ["i1_d16", "i2_d16", "i4_d16", "i8_d16", "i16_d16", "i32_d16"]
dsweep = ["i8_d2", "i8_d4", "i8_d8", "i8_d16", "i8_d32", "i8_d64"]
corner = ["i2_d4", "i8_d16", "i32_d64"]

# ---- charts (hand-drawn SVG) ----
def chart(names, key, xlabel, miss_key, miss_label, title, path):
    pts = [by[n] for n in names if "fps" in by[n]]
    W, H, L, R, T, B = 640, 380, 70, 70, 50, 60
    pw, ph = W - L - R, H - T - B
    xs = [L + pw * (k + 0.5) / len(pts) for k in range(len(pts))]
    fv = [p["fps"] for p in pts]; mv = [p[miss_key] * 100 for p in pts]
    fmax = math.ceil(max(fv) / 5) * 5; fmin = 0
    mmax = max(mv) * 1.1 if max(mv) > 0 else 1
    mstep = 10 ** math.floor(math.log10(mmax)); 
    for c in (1, 2, 2.5, 5, 10):
        if mmax <= c * mstep: mmax = c * mstep; break
    yf = lambda v: T + ph * (1 - (v - fmin) / (fmax - fmin))
    ym = lambda v: T + ph * (1 - v / mmax)
    c1, c2, fg, grid = "#1f6fb4", "#c7561a", "#222", "#ddd"
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" font-family="Helvetica,Arial,sans-serif" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="#fff"/>',
         f'<text x="{W/2}" y="24" text-anchor="middle" font-size="15" font-weight="bold" fill="{fg}">{title}</text>']
    for k in range(6):
        v = fmax * k / 5
        o.append(f'<line x1="{L}" x2="{W-R}" y1="{yf(v):.1f}" y2="{yf(v):.1f}" stroke="{grid}"/>')
        o.append(f'<text x="{L-8}" y="{yf(v)+4:.1f}" text-anchor="end" fill="{c1}">{v:g}</text>')
        mvv = mmax * k / 5
        o.append(f'<text x="{W-R+8}" y="{ym(mvv)+4:.1f}" fill="{c2}">{mvv:.3g}</text>')
    o.append(f'<line x1="{L}" x2="{L}" y1="{T}" y2="{T+ph}" stroke="{fg}"/><line x1="{L}" x2="{W-R}" y1="{T+ph}" y2="{T+ph}" stroke="{fg}"/>')
    for p, x in zip(pts, xs):
        size = p[key]
        lab = f"{size} KB" + (" (default)" if p["name"] == "i8_d16" else "")
        o.append(f'<text x="{x:.1f}" y="{T+ph+18}" text-anchor="middle" fill="{fg}">{lab}</text>')
    o.append(f'<text x="{L+pw/2}" y="{H-12}" text-anchor="middle" fill="{fg}">{xlabel}</text>')
    o.append(f'<text transform="translate(16 {T+ph/2}) rotate(-90)" text-anchor="middle" fill="{c1}">frames per second at 50 MHz</text>')
    o.append(f'<text transform="translate({W-14} {T+ph/2}) rotate(90)" text-anchor="middle" fill="{c2}">{miss_label} miss rate (%)</text>')
    o.append('<polyline fill="none" stroke="%s" stroke-width="2.5" points="%s"/>' % (c1, " ".join(f"{x:.1f},{yf(v):.1f}" for x, v in zip(xs, fv))))
    o.append('<polyline fill="none" stroke="%s" stroke-width="2.5" stroke-dasharray="6 4" points="%s"/>' % (c2, " ".join(f"{x:.1f},{ym(v):.1f}" for x, v in zip(xs, mv))))
    for x, v, m in zip(xs, fv, mv):
        o.append(f'<circle cx="{x:.1f}" cy="{yf(v):.1f}" r="4" fill="{c1}"/><text x="{x:.1f}" y="{yf(v)-9:.1f}" text-anchor="middle" fill="{c1}" font-size="11">{v:.1f}</text>')
        o.append(f'<rect x="{x-3.5:.1f}" y="{ym(m)-3.5:.1f}" width="7" height="7" fill="{c2}"/><text x="{x:.1f}" y="{ym(m)+17:.1f}" text-anchor="middle" fill="{c2}" font-size="11">{m:.2f}</text>')
    o.append(f'<line x1="{L+10}" x2="{L+34}" y1="{T+10}" y2="{T+10}" stroke="{c1}" stroke-width="2.5"/><text x="{L+40}" y="{T+14}" fill="{fg}">fps (left axis)</text>')
    o.append(f'<line x1="{L+150}" x2="{L+174}" y1="{T+10}" y2="{T+10}" stroke="{c2}" stroke-width="2.5" stroke-dasharray="6 4"/><text x="{L+180}" y="{T+14}" fill="{fg}">{miss_label} miss rate (right axis)</text>')
    o.append('</svg>')
    open(path, "w").write("\n".join(o))

os.makedirs(f"{ROOT}/docs/results", exist_ok=True)
if all("fps" in by[n] for n in isweep):
    chart(isweep, "icache_kb", "I-cache size (D-cache fixed at 16 KB)", "ic_miss_rate", "I-cache", "Doom demo3: frame rate vs I-cache size", f"{ROOT}/docs/results/cache-sweep-icache.svg")
if all("fps" in by[n] for n in dsweep):
    chart(dsweep, "dcache_kb", "D-cache size (I-cache fixed at 8 KB)", "dc_miss_rate", "D-cache", "Doom demo3: frame rate vs D-cache size", f"{ROOT}/docs/results/cache-sweep-dcache.svg")

block = f"""<!-- BEGIN GENERATED TABLES (tools/sweep/report.py) -->
### I-cache sweep (D-cache 16 KB)

{table(isweep)}

### D-cache sweep (I-cache 8 KB)

{table(dsweep)}

### Corner points (and the default for reference)

{table(corner)}

Cache block RAMs = I-cache KB/2 + D-cache KB/2 data blocks (2 KB per 18 kbit block, rounded up) + 1 for the tags of both caches (computed, not synthesized). The device has 46 block RAMs in total; outside the caches the design uses 33 (framebuffer 32, branch-target buffer 1), leaving 13 for cache data and tags.

Default point reproduces the baseline cycle count ({BASELINE_CYCLES:,}): **{'yes' if default_ok_cycles else 'NO'}**.
All 13 configurations ok (reference-identical frames, exit 0, 0 SDRAM violations, {REF_FRAMES} frames): **{'yes' if all(r['ok'] for r in rows) else 'NO'}**.
<!-- END GENERATED TABLES -->"""
if os.path.exists(DOC):
    t = rd(DOC)
    if "<!-- BEGIN GENERATED TABLES" in t:
        t = re.sub(r"<!-- BEGIN GENERATED TABLES.*?<!-- END GENERATED TABLES -->", lambda m: block, t, flags=re.S)
        open(DOC, "w").write(t)
else:
    open(DOC, "w").write(block + "\n")
print(block)
sys.exit(0 if default_ok_cycles and all(r["ok"] for r in rows) else 1)
