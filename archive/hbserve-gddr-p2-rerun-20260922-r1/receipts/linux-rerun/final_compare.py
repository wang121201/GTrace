"""Final report: anchors, derived dashboard table, and a classified diff of the
Linux rerun against the archived macOS original."""
import json
import re
import sys

R = '/home/xmu/nvidiagds/codex-runs/hbserve-memgen-gtsim-alignment'
NEW = R + '/build-r1/out/result.json'
OLD = R + '/control/ada-cosim-alignment-20260922-r1/gddr-full-p2-rerun-gtsim-alignment'.replace(
    'gddr-full-p2-rerun-gtsim-alignment', 'gddr-full-p2-rerun-r1/run-r1/result.json')
MIRROR = R + '/build-r1/mirror'
CYCLE_PS = 40000.0 / 87.0
PHASES = ['Measured/Prefill', 'Measured/Decode1', 'Measured/Decode2']
DOC = {
    'Measured/Prefill': dict(read=15.25e9, write=146.80e6, ms=141.39, bw=108.90),
    'Measured/Decode1': dict(read=15.00e9, write=9.98e3, ms=119.37, bw=125.66),
    'Measured/Decode2': dict(read=14.99e9, write=31.23e3, ms=119.39, bw=125.56),
    'Full': dict(read=45.24e9, write=146.84e6, ms=380.15, bw=119.40),
}


def human(n):
    for unit, div in (('GB', 1e9), ('MB', 1e6), ('KB', 1e3)):
        if abs(n) >= div:
            return f'{n / div:.2f} {unit}'
    return f'{n} B'


def leaves(o, p=()):
    if isinstance(o, dict):
        for k, v in o.items():
            yield from leaves(v, p + (str(k),))
    elif isinstance(o, list):
        for i, v in enumerate(o):
            yield from leaves(v, p + (str(i),))
    else:
        yield p, o


def norm(v):
    if isinstance(v, str) and (MIRROR in v or '/Users/wgs' in v or v.startswith('/home/xmu')):
        return '<PATH>'
    return v


new, old = json.load(open(NEW)), json.load(open(OLD))


def phase_table(d):
    rows = d['rows']
    out = {}
    for ph in PHASES:
        idx = [i for i, r in enumerate(rows) if r.get('phase') == ph]
        f, l = rows[idx[0]], rows[idx[-1]]
        rb = l['physical_cumulative']['read_bytes'] - f['physical_cumulative']['read_bytes']
        wb = l['physical_cumulative']['write_bytes'] - f['physical_cumulative']['write_bytes']
        cyc = l['operation_end_cycle'] - f['operation_start_cycle']
        ns = cyc * CYCLE_PS / 1000.0
        out[ph] = dict(n=len(idx), read=rb, write=wb, cycles=cyc, ns=ns,
                       bw=(rb + wb) / (ns / 1e9) / 1e9)
    f = rows[[i for i, r in enumerate(rows) if r['phase'] == PHASES[0]][0]]
    l = rows[[i for i, r in enumerate(rows) if r['phase'] == PHASES[-1]][-1]]
    rb = l['physical_cumulative']['read_bytes'] - f['physical_cumulative']['read_bytes']
    wb = l['physical_cumulative']['write_bytes'] - f['physical_cumulative']['write_bytes']
    cyc = l['operation_end_cycle'] - f['operation_start_cycle']
    ns = cyc * CYCLE_PS / 1000.0
    out['Full'] = dict(n=len(rows), read=rb, write=wb, cycles=cyc, ns=ns,
                       bw=(rb + wb) / (ns / 1e9) / 1e9)
    return out


print('=' * 78)
print('1. DOCUMENT ANCHORS')
print('=' * 78)
anchors = [
    ('status', 'PASS_SELECTED_CONTINUOUS_CURRENT_HISTORY'),
    ('counts.native_kernel', 1138), ('counts.memory_api_submission', 29),
    ('counts.epoch_begin', 3), ('counts.epoch_end', 3),
    ('CTAs', 772512), ('nodes', 4712768255), ('cycles', 826832904),
    ('HBFSIM.physical.read_bytes', 45241836416),
    ('HBFSIM.physical.write_bytes', 146843072),
    ('HBFSIM.physical.finish_ns', 380129130.2923976),
]
allok = True
for dotted, want in anchors:
    cur = new
    for k in dotted.split('.'):
        cur = cur[k]
    allok &= (cur == want)
    print(f'  {"OK  " if cur == want else "FAIL"} {dotted:30s} {cur!r}')
print(f'  {"OK  " if len(new["rows"]) == 1173 else "FAIL"} rows                           {len(new["rows"])}')
print('  ANCHORS_ALL_MATCH:', allok)

print()
print('=' * 78)
print('2. DERIVED DASHBOARD TABLE (phase windows over result.json rows)')
print('=' * 78)
tn, to = phase_table(new), phase_table(old)
print(f'  {"phase":9s} {"rows":>5s}  {"read":>10s} {"write":>10s} {"time ms":>9s} {"GB/s":>8s}   | doc read / write / ms / GB/s')
for ph in ['Measured/Prefill', 'Measured/Decode1', 'Measured/Decode2', 'Full']:
    a, b, d = tn[ph], to[ph], DOC[ph]
    same = (a['read'] == b['read'] and a['write'] == b['write']
            and a['cycles'] == b['cycles'] and a['ns'] == b['ns'])
    name = ph.split('/')[-1]
    print(f'  {name:9s} {a["n"]:5d}  {human(a["read"]):>10s} {human(a["write"]):>10s} '
          f'{a["ns"]/1e6:9.2f} {a["bw"]:8.2f}   | {human(d["read"])} / {human(d["write"])} / '
          f'{d["ms"]} / {d["bw"]}   {"BITWISE-EQUAL-TO-ARCHIVE" if same else "** DIFFERS **"}')
print()
print('  archive vs linux, exact:')
for ph in ['Measured/Prefill', 'Measured/Decode1', 'Measured/Decode2', 'Full']:
    a, b = tn[ph], to[ph]
    print(f'    {ph.split("/")[-1]:8s} read {b["read"]} -> {a["read"]} {"OK" if a["read"]==b["read"] else "DIFF"}'
          f' | write {b["write"]} -> {a["write"]} {"OK" if a["write"]==b["write"] else "DIFF"}'
          f' | cycles {b["cycles"]} -> {a["cycles"]} {"OK" if a["cycles"]==b["cycles"] else "DIFF"}')

print()
print('=' * 78)
print('3. FULL DIFF CLASSIFICATION vs ARCHIVED result.json')
print('=' * 78)
nl, ol = dict(leaves(new)), dict(leaves(old))
common = sorted(set(nl) & set(ol))
fresh = sorted(set(nl) - set(ol))
gone = sorted(set(ol) - set(nl))
cats = {}
examples = {}
for p in common:
    a, b = norm(nl[p]), norm(ol[p])
    if a == b:
        continue
    key = '.'.join(p)
    if re.search(r'host_seconds$|operation_host_seconds$', key):
        c = 'host wall-clock timing (machine dependent)'
    elif re.search(r'sizeof_|alignment|offset', key):
        c = 'compiler ABI: struct sizes/offsets (diagnostic only)'
    elif key.startswith('loader.'):
        c = 'loader provenance (paths/shas)'
    else:
        c = 'OTHER'
    cats[c] = cats.get(c, 0) + 1
    examples.setdefault(c, []).append((key, a, b))
print(f'  leaves compared: {len(common)}   equal: {len(common) - sum(cats.values())}')
for c, n in sorted(cats.items(), key=lambda x: -x[1]):
    print(f'  {n:6d}  {c}')
    for key, a, b in examples[c][:4]:
        shown = key if len(key) < 70 else key[:67] + '...'
        print(f'          {shown}\n              linux={a!r}  arch={b!r}')
print(f'  keys only in new: {len(fresh)}  only in arch: {len(gone)}')
for p in fresh[:6]:
    print('    ONLY-NEW', '.'.join(p)[:90])
for p in gone[:6]:
    print('    ONLY-OLD', '.'.join(p)[:90])
print()
model_other = [p for p in common
               if not re.search(r'host_seconds$|sizeof_|alignment|offset', '.'.join(p))
               and '.'.join(p)[:7] != 'loader.' and norm(nl[p]) != norm(ol[p])]
print('  MODEL-RELEVANT DIFFERENCES (excluding timing/ABI/loader):', len(model_other))
for p in model_other[:10]:
    print('    ', '.'.join(p)[:100], norm(nl[p]), norm(ol[p]))
