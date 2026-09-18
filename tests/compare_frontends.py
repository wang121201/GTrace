#!/usr/bin/env python3
"""Compare optimized pre-cache preparation with the frozen native executable."""
import argparse
import copy
import json
from pathlib import Path
from compare_runs import sha
from compare_preflight import simulated


def compare(before, after):
    paths = (before, after)
    receipts = [json.loads((p/'run-receipt.json').read_text()) for p in paths]
    results = [json.loads((p/'result.json').read_text()) for p in paths]
    assert all(r['status'] == 'PASS' for r in receipts)
    assert receipts[0]['input_sha256'] == receipts[1]['input_sha256']
    a, b = results
    assert a['mode'] == b['mode']
    assert a['transport_control_sha256'] == b['transport_control_sha256']
    assert a['writeback_request_bytes'] == b['writeback_request_bytes'] == 32
    assert a['batch_size'] == b['batch_size'] == 1
    assert len(a['pipeline']) == len(b['pipeline'])
    if b['mode'] == 'direct':
        assert a['cache'] == b['cache'], 'direct functional cache'
        for x, y in zip(a['pipeline'], b['pipeline']):
            assert {k:v for k,v in x.items() if k != 'host_seconds'} == {
                k:v for k,v in y.items() if k != 'host_seconds'}, x['source_launch_key']
        compared = 'full functional-cache ledger and per-call results excluding host_seconds'
    else:
        for key in ('workflow', 'dirty_sector_evaluation', 'memory_backend'):
            assert a[key] == b[key], key
        for x, y in zip(a['pipeline'], b['pipeline']):
            assert x['source_launch_key'] == y['source_launch_key']
            old, new = (simulated(copy.deepcopy(r['execution'])) for r in (x, y))
            if b['mode'] == 'cosim-fast' and x['family'] == 'SiLU':
                # Only the implementation-description prose changed. The
                # actual call, addresses, census and all timing stay compared.
                descriptions = [v['tiny']['source'].pop('addresses') for v in (old, new)]
                assert descriptions[0] == descriptions[1] or descriptions == [
                    'original canonical_silu::Model::address plus original contiguous-lane verification',
                    'shared canonical_silu::PreparedMemory; original Model::address endpoint and full-grid extent guards']
            assert old == new, x['source_launch_key']
        compared = ('workflow; all per-node retire hashes, kernel cycles, ordered L2 events, '
                    'traffic, overlap, scheduler counters, dirty ledger, HBFSIM statistics; '
                    'only existing host timer fields excluded from execution')
        if b['mode'] == 'cosim-fast':
            compared += '; SiLU source.addresses implementation-description prose separately validated'
    trace_compared = 'trace' in a and 'trace' in b
    if trace_compared:
        assert a['trace']['status'] == b['trace']['status'] == 'PASS_CLOSED_TRACE_READBACK'
        assert a['trace']['file_sha256'] == b['trace']['file_sha256'], 'trace bytes changed'
        assert a['trace']['file_bytes'] == b['trace']['file_bytes']
    return dict(status='PASS_EXACT_SIMULATION_SHARED_FRONTEND', mode=b['mode'],
                calls=len(b['pipeline']), input_sha256=receipts[0]['input_sha256'],
                binary_sha256=[r['binary_sha256'] for r in receipts],
                result_sha256=[sha(p/'result.json') for p in paths],
                CPU_minutes=[r['CPU_minutes'] for r in receipts],
                elapsed_minutes=[r['elapsed_minutes'] for r in receipts],
                CPU_speedup=receipts[0]['CPU_seconds']/receipts[1]['CPU_seconds'],
                compared=compared, full_trace_bytes_equal=True if trace_compared else None,
                trace_sha256=b.get('trace', {}).get('file_sha256') if trace_compared else None,
                scope='Same selected workload only; not full1138 or hardware equivalence.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('before', type=Path)
    p.add_argument('after', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    report = compare(args.before, args.after)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
