#!/usr/bin/env python3
"""vcd2txt.py — render a VCD waveform as text in the terminal (no GUI needed).

Two views:
  * default: a per-clock-cycle table (sampled at the clock's rising edge) — the most
    useful view for a synchronous design. Buses shown as decimal (+hex).
  * --wave : an ASCII waveform, one character column per recorded time step
             (1-bit signals as _/‾, buses as their value at each change).

Examples:
  python scripts/vcd2txt.py build/counter.vcd
  python scripts/vcd2txt.py build/counter.vcd --clock clk --signals count,en,load,tick
  python scripts/vcd2txt.py build/counter.vcd --wave
"""
import argparse
import re
import sys

# The wave view uses an overline glyph; force UTF-8 so it doesn't crash on a Windows
# console using the legacy cp1252 codepage.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def parse_vcd(text):
    """Return (order, names, widths, times, states).
    order: list of ids in declaration order (deduplicated)
    names/widths: id -> name / bit width
    times: list of timestamps; states[i]: id -> raw value string at times[i]."""
    names, widths, order = {}, {}, []
    lines = text.splitlines()
    i = 0
    # --- definitions ---
    while i < len(lines):
        ln = lines[i].strip()
        if ln.startswith('$var'):
            # $var <type> <width> <id> <name> [range] $end
            m = re.match(r'\$var\s+\S+\s+(\d+)\s+(\S+)\s+(\S+)', ln)
            if m:
                w, sid, nm = int(m.group(1)), m.group(2), m.group(3)
                rng = re.search(r'\]\s*\$end', ln)
                if rng:
                    br = re.search(r'(\[[\d:]+\])\s*\$end', ln)
                    if br:
                        nm += br.group(1)
                if sid not in names:
                    names[sid] = nm; widths[sid] = w; order.append(sid)
        elif ln.startswith('$enddefinitions'):
            i += 1
            break
        i += 1
    # --- value changes ---
    toks = []
    for ln in lines[i:]:
        toks.extend(ln.split())
    times, states = [], []
    cur_t, vals = None, {}
    j = 0
    while j < len(toks):
        t = toks[j]
        if t.startswith('#'):
            if cur_t is not None:
                times.append(cur_t); states.append(dict(vals))
            cur_t = int(t[1:])
        elif t[0] in '01xzXZ':
            vals[t[1:]] = t[0]                 # scalar: value char + id
        elif t[0] in 'bB':
            vals[toks[j + 1]] = t[1:]; j += 1  # vector: bits, then id
        elif t[0] in 'rR':
            vals[toks[j + 1]] = t[1:]; j += 1
        j += 1
    if cur_t is not None:
        times.append(cur_t); states.append(dict(vals))
    return order, names, widths, times, states


def val_str(raw, w):
    if raw is None:
        return '-'
    if any(c in 'xzXZ' for c in raw):
        return 'x'
    if w == 1:
        return raw
    try:
        v = int(raw, 2)
        return f"{v}(0x{v:x})" if w > 4 else str(v)
    except ValueError:
        return raw


def changing_ids(order, states):
    out = []
    for sid in order:
        seen = {st.get(sid) for st in states}
        if len(seen) > 1:
            out.append(sid)
    return out


def table_view(order, names, widths, times, states, clock, want):
    # pick clock id
    clk_id = None
    for sid in order:
        if names[sid].split('[')[0] == clock:
            clk_id = sid; break
    ids = [s for s in order if s != clk_id]
    if want:
        ids = [s for s in ids if names[s].split('[')[0] in want]
    else:
        ids = changing_ids(ids, states)   # drop constants (params) by default
    if not ids:
        print("(no changing signals to show)"); return
    if clk_id is None:
        print(f"(no clock named '{clock}'; showing every timestamp)")
        rows = list(range(len(times)))
    else:
        rows, prev = [], '0'
        for k in range(len(times)):
            c = states[k].get(clk_id, '0')
            if prev == '0' and c == '1':
                rows.append(k)
            prev = c
    hdr = ["cyc", "time"] + [names[s] for s in ids]
    widthsn = [len(h) for h in hdr]
    table = []
    for cyc, k in enumerate(rows):
        r = [str(cyc), str(times[k])] + [val_str(states[k].get(s), widths[s]) for s in ids]
        table.append(r)
        widthsn = [max(widthsn[c], len(r[c])) for c in range(len(r))]
    def fmt(r): return "  ".join(x.rjust(widthsn[c]) for c, x in enumerate(r))
    print(fmt(hdr))
    print("  ".join("-" * w for w in widthsn))
    for r in table:
        print(fmt(r))


def wave_view(order, names, widths, times, states):
    ids = changing_ids(order, states)
    label_w = max((len(names[s]) for s in ids), default=8)
    for sid in ids:
        cells = []
        for k in range(len(times)):
            raw = states[k].get(sid)
            if widths[sid] == 1:
                cells.append('‾' if raw == '1' else ('x' if raw in ('x', 'z') else '_'))
            else:
                cells.append(val_str(raw, widths[sid]))
        if widths[sid] == 1:
            print(f"{names[sid].ljust(label_w)} : {''.join(cells)}")
        else:
            print(f"{names[sid].ljust(label_w)} : " + " ".join(cells))


def main(argv=None):
    ap = argparse.ArgumentParser(description="render a VCD as terminal text")
    ap.add_argument('vcd')
    ap.add_argument('--clock', default='clk', help="clock signal name (default: clk)")
    ap.add_argument('--signals', default='', help="comma-separated signal names to show")
    ap.add_argument('--wave', action='store_true', help="ASCII waveform instead of a table")
    args = ap.parse_args(argv)
    with open(args.vcd) as f:
        order, names, widths, times, states = parse_vcd(f.read())
    if not times:
        print("no value changes in VCD"); return 1
    if args.wave:
        wave_view(order, names, widths, times, states)
    else:
        want = [s for s in args.signals.split(',') if s] if args.signals else None
        table_view(order, names, widths, times, states, args.clock, want)
    return 0


if __name__ == '__main__':
    sys.exit(main())
