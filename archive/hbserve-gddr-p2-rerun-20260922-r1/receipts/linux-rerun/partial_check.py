"""Partial check: derive per-phase aggregates from the live operations journal.

A phase is only reported once the run has moved past it (so its closing epoch_end
row exists), which lets the completed Prefill phase be checked while Decode is still
running.
"""
import json
import sys

CYCLE_PS = 40000.0 / 87.0          # 459.7701... ps -> 2175 MHz
ORDER = ['Measured/Prefill', 'Measured/Decode1', 'Measured/Decode2']
DOC = {
    'Measured/Prefill': dict(read=15.25e9, write=146.80e6, ms=141.39, bw=108.90),
    'Measured/Decode1': dict(read=15.00e9, write=9.98e3, ms=119.37, bw=125.66),
    'Measured/Decode2': dict(read=14.99e9, write=31.23e3, ms=119.39, bw=125.56),
}


def human(n):
    for unit, div in (('GB', 1e9), ('MB', 1e6), ('KB', 1e3)):
        if abs(n) >= div:
            return f'{n / div:.2f} {unit}'
    return f'{n} B'


def agg(rows):
    first, last = rows[0], rows[-1]
    rb = last['physical_cumulative']['read_bytes'] - first['physical_cumulative']['read_bytes']
    wb = last['physical_cumulative']['write_bytes'] - first['physical_cumulative']['write_bytes']
    cyc = last['operation_end_cycle'] - first['operation_start_cycle']
    ns = cyc * CYCLE_PS / 1000.0
    return dict(n=len(rows), read=rb, write=wb, cycles=cyc, ns=ns,
                bw=(rb + wb) / (ns / 1e9) / 1e9)


groups = {p: [] for p in ORDER}
for line in open(sys.argv[1], encoding='utf-8'):
    line = line.strip()
    if not line:
        continue
    r = json.loads(line)
    p = r.get('phase')
    if p in groups:
        groups[p].append(r)

last_started = max((i for i, p in enumerate(ORDER) if groups[p]), default=-1)
print(f'rows read: ' + ', '.join(f'{p.split("/")[-1]}={len(groups[p])}' for p in ORDER))
print()
for i, p in enumerate(ORDER):
    rows = groups[p]
    if not rows or i >= last_started:
        print(f'{p.split("/")[-1]:8s} still running ({len(rows)} rows so far) - skipped')
        continue
    a = agg(rows)
    d = DOC[p]
    def ok(x, y, tol=1e-6):
        return abs(x - y) <= tol * max(1.0, abs(y))
    checks = [
        ('read', a['read'], d['read'], ok(a['read'], d['read'])),
        ('write', a['write'], d['write'], ok(a['write'], d['write'])),
        ('ms', a['ns'] / 1e6, d['ms'], ok(a['ns'] / 1e6, d['ms'], 1e-3)),
        ('GB/s', a['bw'], d['bw'], ok(a['bw'], d['bw'], 1e-3)),
    ]
    print(f'{p.split("/")[-1]:8s} rows={a["n"]:4d}  '
          + '  '.join(f'{n}={"OK" if o else "MISMATCH"}({human(v) if n in ("read","write") else round(v,2)})'
                      for n, v, _, o in checks))
    print(f'         cycles={a["cycles"]}  time={a["ns"] / 1e6:.2f} ms  bw={a["bw"]:.2f} GB/s'
          f'   doc: {human(d["read"])} / {human(d["write"])} / {d["ms"]} ms / {d["bw"]} GB/s')
