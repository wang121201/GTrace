#!/usr/bin/env python3
"""Stage CLI profile binding/publication tests; original source stays read-only."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-run', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source, out = args.source_run.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    original = json.loads((source/'result.json').read_text())
    cases = []

    def run(name, mutate=None, good=False, window=2, repin=True):
        src = out/(name+'-source')
        src.mkdir()
        result = copy.deepcopy(original)
        if mutate:
            mutate(result)
        (src/'result.json').write_text(json.dumps(result)+'\n')
        receipt = json.loads((source/'run-receipt.json').read_text())
        # Synthetic metadata negatives are re-pinned only to reach the engine
        # contract checks. The separate stale-pin case must fail at the wrapper.
        if repin:
            receipt['result_sha256'] = hashlib.sha256((src/'result.json').read_bytes()).hexdigest()
        (src/'run-receipt.json').write_text(json.dumps(receipt)+'\n')
        os.link(source/'dram.tgn', src/'dram.tgn')
        target = out/(name+'-run')
        argv = [sys.executable, str(ROOT/'replay.py'), '--source-run', str(src),
                '--input', str(args.input.resolve()), '--binary', str(args.binary.resolve()),
                '--output', str(target), '--mode', 'stage-overlap', '--prefetch-stages', str(window)]
        child = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        receipt_path = target/'run-receipt.json'
        receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else {}
        published = (target/'result.json').exists()
        ok = ((child.returncode == 0 and receipt.get('status') == 'PASS' and published) if good else
              (child.returncode != 0 and receipt.get('status') == ('FAILED' if window in (1,2,4,8) else None) and not published))
        row = dict(name=name, status='PASS' if ok else 'FAIL', expected_success=good,
                   returncode=child.returncode, formal_result_published=published,
                   receipt_status=receipt.get('status'), error=receipt.get('error'))
        if good and ok:
            replay = json.loads((target/'result.json').read_text())
            ok = (replay['phase_profile'] == original['phase_profile'] and
                  replay['trace']['file_sha256'] == original['trace']['file_sha256'] and
                  replay['read_bytes'] == original['trace']['read_bytes'] and
                  replay['write_bytes'] == original['trace']['write_bytes'])
            row['status'] = 'PASS' if ok else 'FAIL'
        cases.append(row)
        if not ok:
            raise AssertionError(json.dumps(row)+'\n'+child.stderr.decode(errors='replace'))

    run('positive-closed-profile', good=True)
    run('missing-profile', lambda x: x.pop('phase_profile'))
    run('null-profile', lambda x: x.__setitem__('phase_profile', None))
    run('list-profile', lambda x: x.__setitem__('phase_profile', []))
    run('changed-compute-stale-pin', lambda x: x['phase_profile']['stages'][0].__setitem__('compute_cycles', 0), repin=False)
    run('wrong-qualification', lambda x: x['phase_profile'].__setitem__('qualification', 'CALIBRATED'))
    run('missing-last-stage', lambda x: x['phase_profile']['stages'].pop())
    run('bad-cta-range', lambda x: x['phase_profile']['stages'][0].__setitem__('cta_begin', 1))
    run('negative-compute', lambda x: x['phase_profile']['stages'][0].__setitem__('compute_cycles', -1))
    run('record-gap', lambda x: x['phase_profile']['stages'][0].__setitem__('record_begin', 1))
    run('record-shortfall', lambda x: x['phase_profile']['stages'][-1].__setitem__('record_end', x['trace']['records']-1))
    run('non-contiguous-stage-id', lambda x: x['phase_profile']['stages'][0].__setitem__('id', 10))
    run('compute-budget', lambda x: x['phase_profile']['stages'][0].__setitem__('compute_cycles', 2_000_000_001))
    run('invalid-window', window=3)
    summary = dict(status='PASS', CPU_only=True, cases=cases,
                   scope='Positive real-trace replay and synthetic negative metadata derivatives; no source trace bytes modified.')
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
