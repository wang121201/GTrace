"""Derive the dashboard's per-phase traffic/time/bandwidth table from a result.json.

Reproduces the aggregation the archived report_compare.py / build_dashboard.py use:
phase windows are the row ranges carrying each `phase` label; bytes are the
difference of `physical_cumulative` between the phase's first and last row; time is
phase cycles x (40000/87) ps; the core runs at 2175 MHz.
"""
import json
import sys

CYCLE_PS = 40000.0 / 87.0          # 459.7701149425287 ps -> 2175 MHz
PHASES = ['Measured/Prefill', 'Measured/Decode1', 'Measured/Decode2']
SHORT = {'Measured/Prefill': 'Prefill', 'Measured/Decode1': 'Decode1',
         'Measured/Decode2': 'Decode2'}


def gb(n):
    return n / 1e9


def human(n):
    for unit, div in (('GB', 1e9), ('MB', 1e6), ('KB', 1e3)):
        if abs(n) >= div:
            return f'{n / div:.2f} {unit}'
    return f'{n} B'


def derive(path):
    d = json.load(open(path))
    rows = d['rows']
    out = {}
    for ph in PHASES:
        idx = [i for i, r in enumerate(rows) if r.get('phase') == ph]
        first, last = rows[idx[0]], rows[idx[-1]]
        rb = last['physical_cumulative']['read_bytes'] - first['physical_cumulative']['read_bytes']
        wb = last['physical_cumulative']['write_bytes'] - first['physical_cumulative']['write_bytes']
        cyc = last['operation_end_cycle'] - first['operation_start_cycle']
        ns = cyc * CYCLE_PS / 1000.0
        out[ph] = {'rows': len(idx), 'read_bytes': rb, 'write_bytes': wb,
                   'cycles': cyc, 'modeled_ns': ns,
                   'bandwidth_GB_s': (rb + wb) / (ns / 1e9) / 1e9}
    # full span = first row of Prefill .. last row of Decode2
    first = rows[[i for i, r in enumerate(rows) if r['phase'] == PHASES[0]][0]]
    last = rows[[i for i, r in enumerate(rows) if r['phase'] == PHASES[-1]][-1]]
    rb = last['physical_cumulative']['read_bytes'] - first['physical_cumulative']['read_bytes']
    wb = last['physical_cumulative']['write_bytes'] - first['physical_cumulative']['write_bytes']
    cyc = last['operation_end_cycle'] - first['operation_start_cycle']
    ns = cyc * CYCLE_PS / 1000.0
    out['Full'] = {'rows': len(rows), 'read_bytes': rb, 'write_bytes': wb, 'cycles': cyc,
                   'modeled_ns': ns, 'bandwidth_GB_s': (rb + wb) / (ns / 1e9) / 1e9}
    out['_status'] = d.get('status')
    out['_cycles_total'] = d.get('cycles')
    out['_hbfsim_physical'] = d.get('HBFSIM', {}).get('physical')
    return out


if __name__ == '__main__':
    res = derive(sys.argv[1])
    print(json.dumps(res, indent=2))
