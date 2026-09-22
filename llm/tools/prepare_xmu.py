"""Prepare paired functional LLM runs, reusing sealed XMU input in place.

Run on XMU under the shared CPU controller. No source address trace is exported.
This script creates only fresh task-owned files and does not launch the pairs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

F = Path('/home/xmu/nvidiagds/simulators/hyfiss/analysis/write-gap-fix-20260920-r1')
L = F / 'llama-p32d2-20260921/cpu-llama3_8b-r1'
Q = F / 'kernel-complete-input-r1/p32d2-smoke-r1/cpu-replay-stage-r1'
CASES = {
    'qwen': dict(spec=F/'l2-policy-unified-p32d2-20260921/age-sweep-r1/a64000000h288/spec.json',
                 tree=Q/'tree/kernel-complete-input-r1', expected=2194, cpu=8),
    'llama': dict(spec=L/'run-spec.json', tree=L/'tree/kernel-complete-input-r1', expected=2426, cpu=10),
}


def pin(p):
    p = Path(p).resolve()
    b = p.read_bytes()
    return dict(path=str(p), bytes=len(b), sha256=hashlib.sha256(b).hexdigest())


def save(p, value):
    p.write_text(json.dumps(value, indent=2) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--resources-directory', type=Path)
    ap.add_argument('--assumed-shared-bytes', type=int, choices=[32768, 65536, 102400])
    a = ap.parse_args()
    assert bool(a.resources_directory) != bool(a.assumed_shared_bytes), 'explicit observed metadata OR declared sensitivity profile required'
    root = a.root.resolve()
    assert root.name.startswith('gtsim-ada-r4-llm-20260922-') and root.parent == Path('/home/xmu/nvidiagds/codex-runs')
    repo, build = root/'repo', root/'build'
    build.mkdir(exist_ok=False)
    source = repo/'source'
    runner = repo/'llm/executor-r1'
    third = source/'work/gddr6-support/delivery-stage/sources/third_party/gtsim-prepared/third_party'
    binary = build/'source-cache-runner'
    cmd = ['c++','-std=c++20','-O3','-DTINY_SHA_PORTABLE', '-I'+str(runner), '-I'+str(source),
           '-I'+str(source/'work/tilegen-full-r1/core-native-copy-r2/include'), '-I'+str(third),
           str(runner/'main.cpp'), '-o', str(binary)]
    started = time.monotonic()
    compiled = subprocess.run(cmd, capture_output=True, text=True)
    (build/'compile.log').write_text(compiled.stdout+compiled.stderr)
    compiled.check_returncode()
    compile_minutes = (time.monotonic()-started)/60
    assert binary.read_bytes()[:4] == b'\x7fELF'
    code = [pin(p) for p in sorted(repo.rglob('*')) if p.is_file()]
    prepared = []
    for case, c in CASES.items():
        original = json.loads(c['spec'].read_text())
        for row in original['sources']:
            assert pin(row['path']) == row, 'sealed input changed: '+row['path']
        argv = original['argv'][:]
        argv[2] = str(runner/'whole_stream.py')
        argv[argv.index('--runner')+1] = str(binary)
        env = dict(original['environment'], TILEGEN_NATIVE_TREE=str(c['tree']),
                   TILEGEN_NATIVE_SUPPORT_TREE=str(L/'tree/llama-p32d2-20260921'),
                   TILEGEN_L2_DIRTY_AGE_ACCESSES='64000000', TILEGEN_EF_HIT_RATE='288',
                   PYTHONDONTWRITEBYTECODE='1', OMP_NUM_THREADS='1', MKL_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1')
        additional = []
        if a.resources_directory:
            resources = a.resources_directory/(case+'.json')
            additional = [pin(resources)]
            argv.extend(['--launch-resources', str(resources)])
            env['TILEGEN_ADA_REQUIRE_OBSERVED'] = '1'
        else:
            env['TILEGEN_ADA_SHARED_BYTES'] = str(a.assumed_shared_bytes)
        preflight = root/(case+'-preflight')
        argv[argv.index('--output')+1] = str(preflight)
        p = subprocess.run(argv+['--preflight-only'], env=dict(os.environ, **env), capture_output=True, text=True)
        (build/(case+'-preflight.log')).write_text(p.stdout+p.stderr)
        p.check_returncode()
        state = json.loads((preflight/'status.json').read_text())
        assert state['status'] == 'PASS_COMPLETE_NATIVE_INPUT_PREFLIGHT'
        assert state['input_preflight']['kernel_count'] == c['expected']
        assert len(state['input_preflight']['phases']) == 6
        for offset, profile in enumerate(['r2', 'r4']):
            name = case+'-'+profile
            output = root/name
            assert not output.exists()
            args = argv[:]
            args[args.index('--output')+1] = str(output)
            spec = dict(case_id=name+'-B1-BF16-P32D2', tool='native-functional-LLM-L1-paired-control',
                        input_kind='COMPLETE_NATIVE_TILEGRAPH_SOURCE_PROGRAMS_AND_API_HISTORY',
                        cpu=c['cpu']+offset, gpu=None, seconds=18000, rss_limit_bytes=16<<30,
                        argv=args, environment=dict(env, TILEGEN_ADA_L1_PROFILE=profile),
                        sources=original['sources']+code+[pin(binary),pin(c['spec'])]+additional)
            spec_path = root/(name+'-spec.json')
            save(spec_path, spec)
            prepared.append(dict(case=case, profile=profile, spec=pin(spec_path), expected_kernels=c['expected'],
                                 cpu=spec['cpu'], observed_launch_resources=bool(a.resources_directory)))
    save(root/'prepare-result.json', dict(status='PASS_PAIRED_LLM_PREFLIGHT_NOT_EXECUTED',
         compile_wall_minutes=compile_minutes, binary=pin(binary), command=cmd, cases=prepared,
         compute_stall_cosimulation=False, old_inputs_modified=False))
    print(json.dumps(dict(status='PASS_PAIRED_LLM_PREFLIGHT_NOT_EXECUTED', cases=len(prepared), compile_wall_minutes=compile_minutes)))


if __name__ == '__main__':
    main()
