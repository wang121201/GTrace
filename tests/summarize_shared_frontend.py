#!/usr/bin/env python3
"""Publish evidence for shared pre-cache preparation after all checks succeed."""
import argparse
import json
from pathlib import Path
import shutil
import statistics
from compare_frontends import compare
from compare_runs import sha

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(path.read_text())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runs', type=Path, default=ROOT/'build/shared-frontend-validation')
    p.add_argument('--build', type=Path, default=ROOT/'build/shared-frontend-r2')
    args = p.parse_args()
    runs, build = args.runs.resolve(), args.build.resolve()
    b = read(build/'build-receipt.json')
    assert b['status'] == 'PASS_BUILD_ONLY_NO_SIMULATION' and b['sources_unchanged']
    for path, expected in b['source_pins'].items():
        assert sha(ROOT/path) == expected, path
    benchmark = read(runs/'paired-final/summary.json')
    assert benchmark['status'] == 'PASS_PAIRED_EXACT_NATIVE_COSIM'
    for row in benchmark['rows']:
        if row['variant'] == 'candidate':
            assert row['receipt']['binary_sha256'] == b['binary']['sha256']
    # Recompute comparisons using the final comparison script, preserving the
    # raw benchmark record separately. Null trace result means not requested.
    benchmark['comparisons'] = [compare(runs/f'paired-final/baseline-{i}',
                                        runs/f'paired-final/candidate-{i}')
                                for i in range(1, benchmark['repeats']+1)]
    units = read(runs/'units/summary.json')
    assert units['status'] == 'PASS' and units['sanitize'] == 'address,undefined'
    oracle = read(runs/'original-formulas/run.stdout')
    oracle_receipt = read(runs/'original-formulas/receipt.json')
    expected = 'PASS_GEMV_SILU_SHARED_MATERIALIZERS_MATCH_ORIGINAL_FORMULAS'
    assert oracle['status'] == oracle_receipt['status'] == expected
    assert oracle_receipt['original_build_receipt_sha256'] == sha(build/'build-receipt.json')
    comparisons = {}
    for old, new in [('families-cosim-on','families-cosim'),
                     ('families-direct','families-direct'),
                     ('decode-direct','decode-direct'),
                     ('decode-cosim-fast','decode-fast')]:
        comparisons[new] = compare(ROOT/'build/qualification-r2'/old, runs/new)
        assert comparisons[new]['binary_sha256'][1] == b['binary']['sha256']
    representative = read(runs/'paired-final/candidate-1/result.json')
    candidate_rows = [r for r in benchmark['rows'] if r['variant'] == 'candidate']
    intermediate = read(runs/'paired/summary.json')
    assert intermediate['status'] == 'PASS_PAIRED_EXACT_NATIVE_COSIM'
    frontend = {key:statistics.median(sum(f[key] for f in row['frontend'])
                                      for row in candidate_rows)
                for key in ('build','retire','retire_hash','retire_invariant')}
    evidence = dict(status='PASS_SHARED_FRONTEND_EXACT_B1',
        baseline_commit='ad8afa309a1d95e420c09b71877e27858bf4c9f6',
        branch='codex/tilegen-trace-cosim-20260918-r1',
        binary=b['binary'], build_receipt_sha256=sha(build/'build-receipt.json'),
        production_source_files=len(b['source_pins']),
        source_tree_unchanged_since_build=True, batch_size=1,
        writeback_request_bytes=32, read_fill_and_RFO_bytes=128,
        benchmark=benchmark, native_frontend_wall_seconds=frontend,
        materializer_only_intermediate=dict(medians=intermediate['medians'],
            CPU_speedup=intermediate['CPU_speedup'], engine_speedup=intermediate['engine_speedup'],
            receipt=str(runs/'paired/summary.json'), receipt_sha256=sha(runs/'paired/summary.json'),
            scope='Ablation before host CTA-validation changes; not the delivered binary.'),
        workflow=representative['workflow'], comparisons=comparisons,
        original_formulas=oracle,
        original_formulas_receipt_sha256=sha(runs/'original-formulas/receipt.json'),
        unit_tests=units,
        gate_test=read(runs/'units/cta_validation_test.run.stdout'),
        limitations=[
            'No full1138 rerun, no B8, no new NCU or GPU sampling, no XMU deployment.',
            'Benchmark is a cold selected Decode2 layer1 GEMV/SiLU/GEMV subsequence, 512+1+512 CTA; not one full decode token.',
            'Three serial alternating pairs. Engine is host wall time; complete child CPU is user+sys. Compilation/preparation excluded.',
            'Other functional regression checks may run concurrently; their times are not used as speedup benchmarks.',
            'CPU ranges overlap and median elapsed does not improve; no stable end-to-end speedup is established.',
            'GEMV/SiLU share pre-cache materializers. Functional direct and fine cosim still have different cache order.',
            'Original estimated-address, modeled-compute and incomplete implicit-dependency qualifications remain.'])
    output = ROOT/'validation/shared-frontend.json'
    output.write_text(json.dumps(evidence, indent=2)+'\n')
    shutil.copyfile(build/'build-receipt.json', ROOT/'validation/shared-frontend-build.json')
    shutil.copyfile(runs/'original-formulas/receipt.json', ROOT/'validation/shared-frontend-formula-receipt.json')
    print(json.dumps(dict(status=evidence['status'], output=str(output),
                         medians=benchmark['medians'], CPU_speedup=benchmark['CPU_speedup'],
                         engine_speedup=benchmark['engine_speedup']), indent=2))


if __name__ == '__main__':
    main()
