#!/usr/bin/env python3
"""Build and run bounded CPU unit tests; no GPU, sampling, or model workflow."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
TESTS = ('dirty32_test', 'direct_cache_smoke', 'native_trace_test', 'cta_validation_test', 'trace_replay_test', 'stage_replay_test', 'direct_phase_profile_test')
HBF = ('hbm/hbm_device.cpp', 'resource_calendar.cpp', 'gap_calendar.cpp', 'address_heatmap.cpp')


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def source_manifest():
    paths = list((ROOT/'source').rglob('*'))
    paths += [ROOT/'build-config.json', Path(__file__).resolve()]
    paths += [ROOT/'tests'/(name+'.cpp') for name in TESTS]
    return {p.relative_to(ROOT).as_posix(): sha(p) for p in sorted(paths) if p.is_file()}


def command(argv, out, stem, env, timeout):
    started = time.monotonic()
    receipt = dict(argv=argv, returncode=None)
    try:
        with (out/(stem+'.stdout')).open('wb') as stdout, (out/(stem+'.stderr')).open('wb') as stderr:
            receipt['returncode'] = subprocess.run(argv, cwd=ROOT, env=env, stdout=stdout,
                stderr=stderr, timeout=timeout, check=False).returncode
    except (OSError, subprocess.TimeoutExpired) as error:
        receipt['error'] = str(error)
    receipt['elapsed_seconds'] = time.monotonic()-started
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/unit-tests', help='New output directory; never overwritten')
    parser.add_argument('--sanitize', choices=('address,undefined',), help='ASan+UBSan; leak detection explicitly disabled')
    args = parser.parse_args()
    config = json.loads((ROOT/'build-config.json').read_text())
    compiler = shutil.which(config['compiler'])
    if not compiler or config['standard'] != 'c++20' or 'TILEGEN_DIRTY_SECTOR_MODE=2' not in config['definitions']:
        parser.error('configured compiler, C++20 and dirty-sector mode 2 are required')
    out = args.output.resolve()
    if out.exists():
        parser.error('output directory already exists; choose a new --output')
    out.mkdir(parents=True)
    before = source_manifest()
    (out/'source-sha256.json').write_text(json.dumps(before, indent=2)+'\n')
    flags = ['-std=c++20', '-O2', '-Wall', '-Wextra']
    flags += ['-D'+item for item in config['definitions']]
    flags += ['-I'+str(ROOT/p) for p in ['source']+config['include_directories']]
    env = os.environ.copy()
    if args.sanitize:
        flags += ['-g', '-fsanitize='+args.sanitize, '-fno-omit-frame-pointer']
        env['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'

    def test(name):
        sources = [ROOT/'tests'/(name+'.cpp')]
        if name in ('native_trace_test', 'trace_replay_test', 'stage_replay_test'):
            sources += [ROOT/'source/work/hbfsim-latest/upstream/src/physical'/p for p in HBF]
        executable = out/name
        build = command([compiler]+flags+[str(p) for p in sources]+['-o', str(executable)], out, name+'.compile', env, 180)
        row = dict(status='FAILED', compile=build)
        if build['returncode'] != 0:
            return name, row
        row['binary_sha256'] = sha(executable)
        argv = [str(executable)]
        if name == 'native_trace_test':
            argv += [str(out/'trace-fixture')]
        run = command(argv, out, name+'.run', env, 30)
        row['run'] = run
        if run['returncode'] == 0:
            try:
                text = (out/(name+'.run.stdout')).read_text()
                if name == 'dirty32_test':
                    match = re.search(r'^PASS mode=2 checks=([1-9][0-9]*)\s*$', text.splitlines()[-1])
                    if not match:
                        raise ValueError('missing dirty32 success marker')
                    row['checks'] = int(match.group(1))
                else:
                    result = json.loads(text)
                    expected = {'native_trace_test':'PASS',
                                'direct_cache_smoke':'PASS_FUNCTIONAL_DIRECT_CACHE_SMOKE',
                                'cta_validation_test':'PASS_CTA_VALIDATION_EQUIVALENCE',
                                'trace_replay_test':'PASS_TRACE_REPLAY_EXACT_TICK_REFERENCE',
                                'stage_replay_test':'PASS_STAGE_REPLAY_EXACT_TICK_REFERENCE',
                                'direct_phase_profile_test':'PASS_DIRECT_PHASE_PROFILE'}[name]
                    if result.get('status') != expected:
                        raise ValueError('missing unit-test success status')
                    row['checks'] = result.get('checks')
                row['status'] = 'PASS'
            except (ValueError, IndexError) as error:
                row['error'] = str(error)
        return name, row

    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=2) as pool:
        rows = dict(pool.map(test, TESTS))
    unchanged = source_manifest() == before
    summary = dict(schema='TILEGEN_CPU_UNIT_TESTS_V1',
        status='PASS' if unchanged and all(row['status'] == 'PASS' for row in rows.values()) else 'FAILED',
        compiler=compiler, standard='c++20', optimization='O2', dirty_sector_mode=2,
        sanitize=args.sanitize, leak_detection=False if args.sanitize else None,
        sanitizer_scope='ASan/UBSan only; LeakSanitizer not run' if args.sanitize else 'none',
        CPU_only=True, GPU_or_model_workflow_executed=False, parallel_test_limit=2,
        elapsed_seconds=time.monotonic()-started, source_tree_unchanged=unchanged,
        source_manifest='source-sha256.json', source_manifest_sha256=sha(out/'source-sha256.json'),
        source_scope='Entire repo/source tree, all selected test sources, this runner and build-config.json', tests=rows)
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2))
    return 0 if summary['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
