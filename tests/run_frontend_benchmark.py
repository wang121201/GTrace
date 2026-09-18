#!/usr/bin/env python3
"""Serial paired native cosim runs; never changes scheduling precision."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
from compare_frontends import compare

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--repeats', type=int, default=3)
    args = p.parse_args()
    if args.repeats < 1:
        p.error('positive repeat count required')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    state = dict(status='RUNNING', mode='cosim', GPU_executed=False,
                 benchmark_execution='serial; alternating baseline/candidate order',
                 repeats=args.repeats, rows=[], comparisons=[])

    def save():
        (out/'summary.json').write_text(json.dumps(state, indent=2)+'\n')

    try:
        for repeat in range(args.repeats):
            order = ('baseline', 'candidate') if repeat % 2 == 0 else ('candidate', 'baseline')
            for variant in order:
                target = out/f'{variant}-{repeat+1}'
                state['current'] = target.name
                save()
                command = [sys.executable, str(ROOT/'run.py'), '--input', str(args.input.resolve()),
                           '--binary', str(getattr(args, variant).resolve()), '--output', str(target),
                           '--mode', 'cosim']
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
                receipt = json.loads((target/'run-receipt.json').read_text())
                result = json.loads((target/'result.json').read_text())
                state['rows'].append(dict(variant=variant, repeat=repeat+1, directory=str(target),
                                         receipt=receipt, engine_seconds=result['host']['engine_seconds'],
                                         frontend=[r.get('host_frontend_seconds') for r in result['pipeline']]))
                save()
                print(json.dumps(dict(completed=target.name, CPU_minutes=receipt['CPU_minutes'])), flush=True)
            state['comparisons'].append(compare(out/f'baseline-{repeat+1}', out/f'candidate-{repeat+1}'))
            save()
        state['medians'] = {}
        for variant in ('baseline', 'candidate'):
            rows = [r for r in state['rows'] if r['variant'] == variant]
            state['medians'][variant] = dict(
                CPU_minutes=statistics.median(r['receipt']['CPU_minutes'] for r in rows),
                elapsed_minutes=statistics.median(r['receipt']['elapsed_minutes'] for r in rows),
                engine_seconds=statistics.median(r['engine_seconds'] for r in rows))
        a, b = (state['medians'][v] for v in ('baseline', 'candidate'))
        state['CPU_speedup'] = a['CPU_minutes']/b['CPU_minutes']
        state['engine_speedup'] = a['engine_seconds']/b['engine_seconds']
        state['status'] = 'PASS_PAIRED_EXACT_NATIVE_COSIM'
    except Exception as error:
        state.update(status='FAIL', error=str(error))
        raise
    finally:
        save()
    print(json.dumps({k:state[k] for k in ('status', 'medians', 'CPU_speedup', 'engine_speedup')}, indent=2))


if __name__ == '__main__':
    main()
