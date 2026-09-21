#!/usr/bin/env python3
"""Bounded CPU end-to-end checks for the explicitly functional Ada replay.

No GPU, NCU, timing calibration, source edits, or mutation of pinned configs.
The final receipt contains deterministic facts/pins; child execution receipts
retain their measured CPU/wall values separately.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'configs/rtx4000-ada-accelsim-v1'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, obj):
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/ada-e2e-r2/ada_cache_replay')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/ada-profile-e2e-test-r1')
    args = parser.parse_args()
    binary, out = args.binary.resolve(), args.output.resolve()
    if not binary.is_file():
        parser.error('binary does not exist')
    if out.exists():
        parser.error('choose a new output directory')
    out.mkdir(parents=True)
    fixture = ROOT / 'tests/fixtures/ada-sector.jsonl'
    watched = [fixture, ROOT / 'ada_profile.py', ROOT / 'tools/import_ada_tuner.py',
               *sorted(p for p in PROFILE.rglob('*') if p.is_file())]
    before = {str(p.relative_to(ROOT)): sha(p) for p in watched}
    before['binary'] = sha(binary)
    checks, runs = [], []

    def check(name, ok, **facts):
        checks.append({'id': name, 'status': 'PASS' if ok else 'FAIL', **facts})
        if not ok:
            raise AssertionError(name)

    def command(name, argv):
        p = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=60)
        (out / (name + '.stdout')).write_text(p.stdout)
        (out / (name + '.stderr')).write_text(p.stderr)
        runs.append({'id': name, 'returncode': p.returncode})
        return p

    def replay(name, rows=None, reject=False):
        source = fixture
        if rows is not None:
            source = out / (name + '.jsonl')
            source.write_text(''.join(json.dumps(r, sort_keys=True) + '\n' for r in rows))
        target = out / name
        p = command(name, [sys.executable, str(ROOT / 'ada_profile.py'),
                          '--binary', str(binary), '--profile', 'tuner-v1', '--input', str(source),
                          '--output', str(target), '--emit-trace'])
        receipt = json.loads((target / 'receipt.json').read_text())
        check(name + '.exit', (p.returncode != 0) if reject else (p.returncode == 0))
        check(name + '.receipt', receipt['status'] == ('FAILED' if reject else 'PASS_PROCESS_ONLY'))
        check(name + '.pins', receipt['source_unchanged'] and receipt['input_unchanged'] and
              receipt['profile_unchanged'] and receipt['binary_sha256'] == before['binary'])
        if reject:
            # A bad input must not publish a successful report even when a
            # partial trace file exists; callers must honor the exit/receipt.
            text = (target / 'result.json').read_text().strip()
            check(name + '.no_success_result', not text)
            check(name + '.diagnostic', bool((target / 'run.stderr').read_text().strip()))
            return None
        result = json.loads((target / 'result.json').read_text())
        trace = [json.loads(s) for s in (target / 'postcache.requests.jsonl').read_text().splitlines()]
        return result, trace

    status, error = 'FAILED', None
    try:
        # This fixture gives a hand-auditable sector ledger: one dirty sector,
        # 36 unique sector fills, then an L2 hit after an adaptive L1 change.
        r, trace = replay('fixture')
        c = r['cache']
        check('fixture.counts', r['kernel_count'] == 2 and r['source_instructions'] == 42)
        check('fixture.bytes', c['DRAM_read_bytes'] == 1152 and c['DRAM_write_bytes'] == 32,
              DRAM_read_bytes=c['DRAM_read_bytes'], DRAM_write_bytes=c['DRAM_write_bytes'])
        check('fixture.events', len(trace) == 37 and sum(not x['write'] for x in trace) == 36 and
              sum(x['write'] for x in trace) == 1 and all(x['bytes'] == 32 for x in trace))
        check('fixture.trace_order', [x['sequence'] for x in trace] == list(range(37)))
        check('fixture.trace_no_timing', all(x['schema'] == 'GTSIM_ADA_POSTCACHE_SECTOR_V1' and
              x['issue_cycle'] is None and x['address'] % 32 == 0 for x in trace))
        check('fixture.partitions', {x['memory_subpartition'] for x in trace if not x['write']} == set(range(20)) and
              all(x['channel'] == x['memory_subpartition'] // 2 for x in trace))
        check('fixture.partition_conservation', sum(p['read_bytes'] for p in r['partitions']) == 1152 and
              sum(p['write_bytes'] for p in r['partitions']) == 32)
        first, second = r['phases']
        check('fixture.adaptive', (first['L1_bytes_per_sm'], first['L1_ways']) == (131072, 256) and
              (second['L1_bytes_per_sm'], second['L1_ways']) == (28672, 56) and
              first['L1_sets'] == second['L1_sets'] == 4)
        check('fixture.retains_L2', second['traffic']['source_memory_instructions'] == 1 and
              second['traffic']['DRAM_read_bytes'] == second['traffic']['DRAM_write_bytes'] == 0 and
              second['cache_end']['L2_hits'] == first['cache_end']['L2_hits'] + 1)
        check('fixture.lazy_partial_completion', c['source_write_bytes'] == 32 and c['L1_read_hits'] == 2 and
              c['L2_lazy_store_sector_allocations'] == 1 and c['L2_sector_merge_reads'] == 0)
        check('fixture.dirty_conservation', c['dirty_sector_ledger_closed'] and c['writeback_byte_ledger_closed'] and
              c['dirty_sector_creations'] == c['evicted_dirty_sectors'] + c['resident_dirty_sectors'])
        check('fixture.scope', r['status'] == 'COMPLETED_FUNCTIONAL_ONLY' and not r['final_flush'] and
              not r['compute_executed'] and not r['HBFSIM_executed'] and not r['NCU_accuracy_tested'] and
              not r['timing_equivalence_to_AccelSim'] and r['bandwidth_GBps'] is None and
              r['simulated_latency_ns'] is None)

        rows = [json.loads(s) for s in fixture.read_text().splitlines()]
        kernel, store = rows[0], rows[1]
        # The main fixture eventually evicts its dirty line. A write-only
        # prefix proves EOF does not drain an otherwise resident dirty sector.
        r, trace = replay('no_final_flush', [kernel, store])
        c = r['cache']
        check('no_final_flush.resident_dirty', not trace and c['DRAM_read_bytes'] == c['DRAM_write_bytes'] == 0 and
              c['resident_dirty_sectors'] == c['dirty_sector_creations'] == 1 and not r['final_flush'])
        partial_read = copy.deepcopy(store)
        partial_read['op'] = 'read'
        r, trace = replay('partial_merge', [kernel, store, partial_read])
        c = r['cache']
        check('partial_merge.real_fill', len(trace) == 1 and not trace[0]['write'] and trace[0]['bytes'] == 32 and
              c['DRAM_read_bytes'] == 32 and c['DRAM_write_bytes'] == 0 and c['L2_sector_merge_reads'] == 1)
        check('partial_merge.retains_dirty', c['resident_dirty_sectors'] == 1 and c['dirty_sector_ledger_closed'] and
              c['writeback_byte_ledger_closed'] and not r['final_flush'])

        invalid = []
        for field in ('threads_per_cta', 'registers_per_thread', 'shared_bytes_per_cta', 'grid_ctas'):
            bad = copy.deepcopy(kernel); del bad[field]
            invalid.append(('missing_' + field, [bad, store]))
        for name, field, value in [('zero_threads', 'threads_per_cta', 0), ('zero_grid', 'grid_ctas', 0),
                                   ('unknown_registers', 'registers_per_thread', None)]:
            bad = copy.deepcopy(kernel); bad[field] = value
            invalid.append((name, [bad, store]))
        for name, field, value in [('zero_bytes', 'bytes', 0), ('negative_bytes', 'bytes', -1),
                                   ('negative_address', 'address', -1), ('float_address', 'address', 0.5),
                                   ('bool_address', 'address', True), ('lane32', 'lane', 32)]:
            bad = copy.deepcopy(store); bad['ranges'][0][field] = value
            invalid.append((name, [kernel, bad]))
        bad = copy.deepcopy(store); bad['ranges'][0].update(address=2**64 - 1, bytes=2)
        invalid.append(('address_overflow', [kernel, bad]))
        bad = copy.deepcopy(store); bad['ranges'] = []
        invalid.append(('empty_ranges', [kernel, bad]))
        for name, field, value in [('out_of_grid', 'cta', 96), ('out_of_warp', 'warp', 2),
                                   ('out_of_SM', 'sm', 48), ('bad_allocation', 'allocation_id', 2**31),
                                   ('unknown_operation', 'op', 'atomic')]:
            bad = copy.deepcopy(store); bad[field] = value
            invalid.append((name, [kernel, bad]))
        invalid.append(('memory_before_kernel', [store]))
        for name, bad in invalid:
            replay('reject_' + name, bad, reject=True)

        # Importer resolves ROOT from its own path: isolated real file copies
        # exercise the production CLI/hash gates, without monkeypatching them.
        import_root = out / 'importer-copy'
        (import_root / 'tools').mkdir(parents=True)
        shutil.copy2(ROOT / 'tools/import_ada_tuner.py', import_root / 'tools/import_ada_tuner.py')
        cfg = import_root / 'configs/rtx4000-ada-accelsim-v1'
        shutil.copytree(PROFILE, cfg)
        argv = [sys.executable, str(import_root / 'tools/import_ada_tuner.py'), '--check']
        p = command('importer_pristine', argv)
        check('importer.pristine', p.returncode == 0 and p.stdout.strip() == 'PASS_PINNED_ADA_CONFIG')
        mutations = [('tuner_value', 'source/gpgpusim.config', b'-gpgpu_n_mem 10', b'-gpgpu_n_mem 11'),
                     ('trace_value', 'source/trace.config', b'4,2', b'5,2'),
                     ('profile_claim', 'profile.json', b'"hardware_calibrated": false', b'"hardware_calibrated": true')]
        for name, relative, old, new in mutations:
            path = cfg / relative; original = path.read_bytes()
            check('importer.' + name + '.mutation_present', old in original)
            path.write_bytes(original.replace(old, new, 1))
            p = command('importer_' + name, argv)
            check('importer.' + name + '.rejected', p.returncode != 0 and bool(p.stderr.strip()))
            path.write_bytes(original)
        path = cfg / 'source/gpgpusim.config'; original = path.read_bytes()
        path.write_bytes(original + b'\n# semantically harmless but not the pinned source\n')
        p = command('importer_pin_whitespace', argv)
        check('importer.byte_pin_rejected', p.returncode != 0)
        path.write_bytes(original)
        check('importer.restored_control', command('importer_restored', argv).returncode == 0)

        after = {str(p.relative_to(ROOT)): sha(p) for p in watched}
        after['binary'] = sha(binary)
        check('original_inputs_unchanged', before == after)
        status = 'PASS_ADA_PROFILE_E2E'
    except Exception as exc:
        error = str(exc)
    receipt = {'schema': 'GTSIM_ADA_PROFILE_E2E_TEST_V1', 'status': status,
               'test_sha256': sha(Path(__file__)), 'pins': before, 'checks': checks,
               'check_count': len(checks), 'subprocesses': runs, 'error': error,
               'scope': 'CPU functional replay only; no timing/GPU/NCU qualification',
               'timing_fields': 'Measured CPU/wall are in child run receipts, not this deterministic receipt'}
    dump(out / 'receipt.json', receipt)
    print(json.dumps({'status': status, 'checks': len(checks), 'error': error, 'receipt': str(out / 'receipt.json')}))
    return 0 if status == 'PASS_ADA_PROFILE_E2E' else 1


if __name__ == '__main__':
    raise SystemExit(main())
