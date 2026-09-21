#!/usr/bin/env python3
"""Exercise the calibrated Ada wrapper using bounded, explicit CPU inputs.

All expected capacities and access outcomes below are independent constants,
not imported from the implementation under test.  No GPU or NCU is invoked.
Each child run retains its actual CLI, input, report, trace and wrapper receipt.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
PROFILES = (
    'tuner-v1', 'r2-adaptive', 'r2-shared64', 'r2-shared100',
    'r3-fifo-shared32', 'r3-fifo-shared64', 'r3-fifo-shared100',
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def kernel(name, observed=None, shared=0):
    row = {'type': 'kernel', 'name': name, 'threads_per_cta': 128,
           'registers_per_thread': 0, 'shared_bytes_per_cta': shared,
           'grid_ctas': 48}
    if observed is not None:
        row['observed_shared_carveout_bytes'] = observed
    return row


def read(address):
    return {'type': 'memory', 'allocation_id': 0, 'cta': 0, 'warp': 0,
            'sm': 0, 'pc': 4096, 'op': 'read',
            'ranges': [{'lane': 0, 'address': address, 'bytes': 4}]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    binary, out = args.binary.resolve(), args.output.resolve()
    if not binary.is_file():
        parser.error('binary must exist')
    if out.exists():
        parser.error('output must be a new directory')
    out.mkdir(parents=True)
    inputs = out / 'inputs'
    inputs.mkdir()
    fixture = ROOT / 'tests/fixtures/ada-sector.jsonl'
    # Retain the exact source fixture instead of referring only to its path.
    (inputs / 'original-sector-fixture.jsonl').write_bytes(fixture.read_bytes())
    watched = [ROOT / 'ada_profile.py', Path(__file__).resolve(), fixture,
               *sorted(p for p in (ROOT / 'source').rglob('*') if p.is_file()),
               *sorted((ROOT / 'tools').glob('*.py')),
               *sorted(p for p in (ROOT / 'configs').rglob('*') if p.is_file())]
    before = {str(p.relative_to(ROOT)): sha(p) for p in watched}
    binary_sha = sha(binary)
    checks, runs = [], []

    def check(name, condition, **facts):
        checks.append({'id': name, 'status': 'PASS' if condition else 'FAIL', **facts})
        if not condition:
            raise AssertionError(name)

    def replay(name, rows, profile=None, reject=False, emit_trace=True):
        source = inputs / (name + '.jsonl')
        source.write_text(''.join(json.dumps(row, sort_keys=True) + '\n' for row in rows))
        target = out / name
        argv = [sys.executable, str(ROOT / 'ada_profile.py'), '--binary', str(binary),
                '--input', str(source), '--output', str(target)]
        if profile is not None:
            argv += ['--profile', profile]
        if emit_trace:
            argv += ['--emit-trace']
        start = time.monotonic()
        p = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=120)
        (out / (name + '.stdout')).write_text(p.stdout)
        (out / (name + '.stderr')).write_text(p.stderr)
        runs.append({'id': name, 'argv': argv, 'returncode': p.returncode,
                     'wall_minutes': (time.monotonic() - start) / 60,
                     'input_sha256': sha(source), 'expected_rejection': reject})
        requested = profile or 'r2-adaptive'
        check(name + '.exit', p.returncode != 0 if reject else p.returncode == 0,
              returncode=p.returncode)
        receipt_path = target / 'receipt.json'
        receipt = json.loads(receipt_path.read_text()) if receipt_path.is_file() else None
        result_path = target / 'result.json'
        result_text = result_path.read_text().strip() if result_path.is_file() else ''
        if reject:
            check(name + '.not_success', receipt is None or receipt['status'] != 'PASS_PROCESS_ONLY')
            check(name + '.no_success_report', not result_text)
            inner_error = target / 'run.stderr'
            check(name + '.diagnostic', bool(p.stderr.strip()) or
                  (inner_error.is_file() and bool(inner_error.read_text().strip())))
            return None
        check(name + '.wrapper_receipt', receipt is not None and
              receipt['schema'] == 'GTSIM_ADA_RUN_RECEIPT_V2' and
              receipt['status'] == 'PASS_PROCESS_ONLY' and receipt['requested_profile'] == requested)
        check(name + '.pins', receipt['binary_sha256'] == binary_sha and
              receipt['input_sha256'] == sha(source) and receipt['source_unchanged'] and
              receipt['input_unchanged'] and receipt['profile_unchanged'])
        check(name + '.raw_cli_profile', receipt['argv'][-1] == requested and len(receipt['argv']) == 4 and
              (receipt['argv'][2] == '-' if not emit_trace else receipt['argv'][2].endswith('postcache.requests.jsonl')))
        r = json.loads(result_text)
        check(name + '.identity', r['configuration'] == requested and
              r['status'] == 'COMPLETED_FUNCTIONAL_ONLY')
        check(name + '.resolved_pins', set(receipt['resolved_profile_sha256']) ==
              {phase['resolved_profile'] for phase in r['phases']} and
              all(value == receipt['available_profile_sha256'][key]
                  for key, value in receipt['resolved_profile_sha256'].items()))
        c = r['cache']
        check(name + '.conservation', c['dirty_sector_ledger_closed'] and
              c['writeback_byte_ledger_closed'] and c['dirty_sector_creations'] ==
              c['evicted_dirty_sectors'] + c['resident_dirty_sectors'] and
              sum(x['read_bytes'] for x in r['partitions']) == c['DRAM_read_bytes'] and
              sum(x['write_bytes'] for x in r['partitions']) == c['DRAM_write_bytes'])
        check(name + '.functional_scope', not r['final_flush'] and not r['compute_executed'] and
              not r['HBFSIM_executed'] and not r['NCU_accuracy_tested'] and
              not r['timing_equivalence_to_AccelSim'] and r['simulated_latency_ns'] is None and
              r['bandwidth_GBps'] is None)
        if emit_trace:
            trace = [json.loads(line) for line in (target / 'postcache.requests.jsonl').read_text().splitlines()]
            check(name + '.trace', all(x['schema'] == 'GTSIM_ADA_POSTCACHE_SECTOR_V1' and
                  x['bytes'] == 32 and x['issue_cycle'] is None for x in trace) and
                  sum(x['bytes'] for x in trace if not x['write']) == c['DRAM_read_bytes'] and
                  sum(x['bytes'] for x in trace if x['write']) == c['DRAM_write_bytes'])
        return r

    def layout(name, phase, resolved, capacity, sets, ways, replacement):
        actual = {key: phase[key] for key in ('resolved_profile', 'L1_bytes_per_sm',
                                             'L1_sets', 'L1_ways', 'L1_replacement')}
        expected = dict(resolved_profile=resolved, L1_bytes_per_sm=capacity,
                        L1_sets=sets, L1_ways=ways, L1_replacement=replacement)
        check(name, actual == expected, expected=expected, actual=actual)

    status, error = 'FAILED', None
    try:
        rows = [json.loads(line) for line in fixture.read_text().splitlines()]
        for name, profile, cap, ways in [('default_r2', None, 98304, 192),
                                        ('explicit_v1', 'tuner-v1', 131072, 256)]:
            r = replay(name, rows, profile)
            selected = profile or 'r2-adaptive'
            check(name + '.fixture_bytes', r['kernel_count'] == 2 and r['source_instructions'] == 42 and
                  r['cache']['DRAM_read_bytes'] == 1152 and r['cache']['DRAM_write_bytes'] == 32)
            layout(name + '.first_layout', r['phases'][0], selected, cap, 4, ways, 'LRU')
            layout(name + '.second_layout', r['phases'][1], selected, 28672, 4, 56, 'LRU')
            check(name + '.phase_L2_retained', r['phases'][1]['traffic']['DRAM_read_bytes'] == 0 and
                  r['phases'][1]['traffic']['DRAM_write_bytes'] == 0)

        # The 193rd congruent line is a real capacity boundary: r2 has 192
        # ways per set while v1 has 256.  Source and L2 compulsory misses match.
        capacity_rows = [kernel('193-congruent-lines')] + [read(i * 512) for _ in range(2) for i in range(193)]
        capacities = {}
        for profile, hits, misses, l2hits in [('tuner-v1', 193, 193, 0), ('r2-adaptive', 0, 386, 193)]:
            name = 'capacity_' + profile
            r = replay(name, capacity_rows, profile)
            c = r['cache']; capacities[profile] = c
            expected = {'source_read_bytes': 1544, 'source_write_bytes': 0,
                        'DRAM_read_bytes': 6176, 'DRAM_write_bytes': 0,
                        'L1_read_hits': hits, 'L1_read_misses': misses, 'L2_hits': l2hits}
            check(name + '.actual_accesses', all(c[k] == v for k, v in expected.items()),
                  expected=expected, actual={k: c[k] for k in expected})
        check('capacity.same_compulsory_DRAM', capacities['tuner-v1']['DRAM_read_bytes'] ==
              capacities['r2-adaptive']['DRAM_read_bytes'])

        fifo_rows = [kernel('FIFO-does-not-refresh-hit', observed=32768)] + [read(i * 8192) for i in range(12)]
        fifo_rows += [read(0), read(12 * 8192), read(0)]
        r = replay('fifo_hit_order', fifo_rows, 'r3-fifo-shared32')
        layout('fifo.layout', r['phases'][0], 'r3-fifo-shared32', 98304, 64, 12, 'FIFO')
        expected = {'L1_read_hits': 1, 'L1_read_misses': 14, 'L2_hits': 1,
                    'DRAM_read_bytes': 416, 'DRAM_write_bytes': 0, 'source_read_bytes': 60}
        check('fifo.evicted_oldest_despite_hit', all(r['cache'][k] == v for k, v in expected.items()),
              expected=expected, actual={k: r['cache'][k] for k in expected})
        check('fifo.explicit_experimental', r['calibration_status'] == 'EXPERIMENTAL_NOT_PROMOTED')

        # Fixed configurations are only admitted with explicit matching
        # observations, and adaptive observation resolves to a pinned variant.
        variants = [
            ('r2-shared64', 65536, 65536, 4, 128, 'LRU'),
            ('r2-shared100', 102400, 28672, 4, 56, 'LRU'),
            ('r3-fifo-shared64', 65536, 65536, 64, 8, 'FIFO'),
            ('r3-fifo-shared100', 102400, 24576, 64, 3, 'FIFO'),
        ]
        for profile, observed, cap, sets, ways, policy in variants:
            r = replay('observed_' + profile, [kernel(profile, observed), read(0)], profile,
                       emit_trace=False)
            layout(profile + '.layout', r['phases'][0], profile, cap, sets, ways, policy)
            check(profile + '.observed_source', r['phases'][0]['carveout_source'] == 'observed_shared_carveout_bytes' and
                  r['phases'][0]['shared_carveout_bytes'] == observed)
        for observed, resolved, cap, ways in [(32768, 'r2-adaptive', 98304, 192),
                                              (65536, 'r2-shared64', 65536, 128),
                                              (102400, 'r2-shared100', 28672, 56)]:
            r = replay('adaptive_observed_' + str(observed), [kernel('observed', observed), read(0)])
            layout('adaptive.resolve.' + str(observed), r['phases'][0], resolved, cap, 4, ways, 'LRU')

        # Check every fixed-profile branch, not just one representative case.
        fixed = [('r2-shared64', 65536), ('r2-shared100', 102400),
                 ('r3-fifo-shared32', 32768), ('r3-fifo-shared64', 65536),
                 ('r3-fifo-shared100', 102400)]
        for profile, observed in fixed:
            replay('reject_missing_' + profile, [kernel('no-observation'), read(0)], profile, reject=True)
            other = 32768 if observed != 32768 else 65536
            replay('reject_mismatch_' + profile, [kernel('wrong-observation', other), read(0)], profile, reject=True)
        replay('reject_unsupported_observation', [kernel('unsupported', 49152), read(0)], reject=True)
        replay('reject_resources_exceed_observation', [kernel('too-much-shared', 32768, 32769), read(0)], reject=True)
        replay('reject_fixed_resources_exceed_observation',
               [kernel('too-much-shared-fixed', 65536, 65537), read(0)], 'r2-shared64', reject=True)
        replay('reject_unsupported_profile', [kernel('unknown-profile'), read(0)], 'r3-fifo-shared28', reject=True)

        # Real CLI copies exercise the production byte-pin gate without
        # monkeypatching it or changing the original configurations.
        isolated = out / 'isolated-importer'
        (isolated / 'tools').mkdir(parents=True)
        for name in ('import_ada_calibrations.py', 'ada_profile_registry.py', 'import_ada_tuner.py'):
            shutil.copy2(ROOT / 'tools' / name, isolated / 'tools' / name)
        for name in ('rtx4000-ada-accelsim-v1', 'rtx4000-ada-calibrated'):
            shutil.copytree(ROOT / 'configs' / name, isolated / 'configs' / name)

        def importer(name, rejected):
            argv = [sys.executable, str(isolated / 'tools/import_ada_calibrations.py'), '--check']
            p = subprocess.run(argv, cwd=isolated, capture_output=True, text=True, timeout=30)
            (out / (name + '.stdout')).write_text(p.stdout)
            (out / (name + '.stderr')).write_text(p.stderr)
            runs.append({'id': name, 'argv': argv, 'returncode': p.returncode,
                         'expected_rejection': rejected})
            check(name, p.returncode != 0 and bool(p.stderr.strip()) if rejected else
                  p.returncode == 0 and p.stdout.strip() == 'PASS_ADA_CALIBRATION_CONFIGS')

        importer('importer_pristine', False)
        source_copy = isolated / 'configs/rtx4000-ada-calibrated/r2-adaptive/source/gpgpusim.config'
        original = source_copy.read_bytes()
        source_copy.write_bytes(original + b'\n# semantically unchanged but no longer pinned\n')
        importer('importer_reject_source_comment', True)
        source_copy.write_bytes(original)
        profile_copy = isolated / 'configs/rtx4000-ada-calibrated/r2-adaptive/profile.json'
        original = profile_copy.read_bytes()
        tampered = json.loads(original)
        check('importer.rop_control', tampered['memory']['rop_core_cycles'] == 239)
        tampered['memory']['rop_core_cycles'] = 238
        # Preserve serialization so the sole intentional data mutation is ROP.
        profile_copy.write_text(json.dumps(tampered, ensure_ascii=False, indent=2) + '\n')
        importer('importer_reject_profile_rop', True)
        profile_copy.write_bytes(original)
        importer('importer_restored', False)

        check('original_sources_unchanged', before == {str(p.relative_to(ROOT)): sha(p) for p in watched})
        check('binary_unchanged', binary_sha == sha(binary))
        status = 'PASS_ADA_CALIBRATED_WRAPPER_E2E'
    except Exception as exc:
        error = type(exc).__name__ + ': ' + str(exc)
    artifacts = {str(p.relative_to(out)): {'sha256': sha(p), 'bytes': p.stat().st_size}
                 for p in sorted(out.rglob('*')) if p.is_file()}
    receipt = {'schema': 'GTSIM_ADA_CALIBRATED_E2E_V1', 'status': status, 'error': error,
               'test_sha256': sha(Path(__file__)), 'binary': str(binary), 'binary_sha256': binary_sha,
               'source_pins': before, 'checks': checks, 'check_count': len(checks),
               'subprocesses': runs, 'artifacts': artifacts,
               'scope': 'Bounded CPU functional cache CLI checks; no GPU, NCU, latency or bandwidth qualification'}
    dump(out / 'receipt.json', receipt)
    print(json.dumps({'status': status, 'checks': len(checks), 'runs': len(runs),
                      'error': error, 'receipt': str(out / 'receipt.json')}))
    return 0 if status.startswith('PASS_') else 1


if __name__ == '__main__':
    raise SystemExit(main())
