#!/usr/bin/env python3
"""Verify native trace export is observational for the same frozen workload."""
import argparse
import hashlib
import json
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized(value):
    if isinstance(value, list):
        return [normalized(x) for x in value]
    if isinstance(value, dict):
        return {k: normalized(v) for k, v in value.items() if k != 'host_engine_seconds'}
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('off', type=Path)
    parser.add_argument('on', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    off, on = [json.loads((p/'result.json').read_text()) for p in (args.off, args.on)]
    receipts = [json.loads((p/'run-receipt.json').read_text()) for p in (args.off, args.on)]
    assert all(r['status'] == 'PASS' for r in receipts)
    assert receipts[0]['binary_sha256'] == receipts[1]['binary_sha256']
    assert receipts[0]['input_sha256'] == receipts[1]['input_sha256']
    assert off['mode'] == on['mode'] == 'cosim'
    assert 'trace' not in off and on['trace']['status'] == 'PASS_CLOSED_TRACE_READBACK'
    assert off['transport_control_sha256'] == on['transport_control_sha256']
    assert off['workflow'] == on['workflow']
    assert off['dirty_sector_evaluation'] == on['dirty_sector_evaluation']
    assert off['memory_backend'] == on['memory_backend']
    assert len(off['pipeline']) == len(on['pipeline'])
    for a, b in zip(off['pipeline'], on['pipeline']):
        assert a['source_launch_key'] == b['source_launch_key']
        assert normalized(a['execution']) == normalized(b['execution']), a['source_launch_key']
    trace = on['trace']
    counters = on['workflow']['counter_delta']
    assert trace['records'] == counters['dram_fill_requests']+counters['dram_writeback_requests']
    assert trace['read_bytes'] == on['workflow']['DRAM_read_bytes']
    assert trace['write_bytes'] == on['workflow']['DRAM_write_bytes']
    result = dict(status='PASS_SAME_BINARY_TRACE_ON_OFF_EXACT',
                  compared='complete per-kernel execution excluding host_engine_seconds; workflow, dirty-sector ledger and memory_backend exact',
                  calls=len(on['pipeline']), trace=trace,
                  binary_sha256=receipts[0]['binary_sha256'], input_sha256=receipts[0]['input_sha256'],
                  result_sha256=[sha(p/'result.json') for p in (args.off,args.on)],
                  CPU_minutes=[r['CPU_minutes'] for r in receipts],
                  elapsed_minutes=[r['elapsed_minutes'] for r in receipts])
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
