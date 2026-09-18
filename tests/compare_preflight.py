#!/usr/bin/env python3
"""Compare identical simulated work before/after host-only preflight pruning."""
import argparse
import json
from pathlib import Path
from compare_runs import sha


def simulated(value):
    if isinstance(value, list):
        return [simulated(v) for v in value]
    if isinstance(value, dict):
        return {k: simulated(v) for k,v in value.items()
                if k not in ('host_engine_seconds', 'host_stage_seconds',
                             'host_partition_registration_seconds')}
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('before', type=Path)
    p.add_argument('after', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    directories = (args.before, args.after)
    runs = [json.loads((d/'run-receipt.json').read_text()) for d in directories]
    results = [json.loads((d/'result.json').read_text()) for d in directories]
    assert all(r['status'] == 'PASS' for r in runs)
    assert runs[0]['input_sha256'] == runs[1]['input_sha256']
    a, b = results
    assert a['mode'] == b['mode']
    for key in ('workflow', 'dirty_sector_evaluation', 'memory_backend'):
        assert a[key] == b[key], key
    assert len(a['pipeline']) == len(b['pipeline'])
    for old, new in zip(a['pipeline'], b['pipeline']):
        assert old['source_launch_key'] == new['source_launch_key']
        assert simulated(old['execution']) == simulated(new['execution']), old['source_launch_key']
    report = dict(status='PASS_EXACT_SIMULATION_HOST_PREFLIGHT_ONLY', mode=b['mode'],
                  input_sha256=runs[0]['input_sha256'], calls=len(b['pipeline']),
                  binaries=[r['binary_sha256'] for r in runs],
                  result_sha256=[sha(d/'result.json') for d in directories],
                  CPU_minutes=[r['CPU_minutes'] for r in runs],
                  CPU_speedup=runs[0]['CPU_seconds']/runs[1]['CPU_seconds'],
                  elapsed_minutes=[r['elapsed_minutes'] for r in runs],
                  typed_preflight_families=[len(r['frame_validation']) for r in results],
                  compared='workflow; all per-kernel execution except host_engine_seconds/host_stage_seconds/host_partition_registration_seconds; dirty ledger; memory backend',
                  scope='Selected-source subset. Does not establish full1138 speedup.')
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
