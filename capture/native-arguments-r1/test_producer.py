#!/usr/bin/env python3
"""Compile/run host-only producer tests; actual callbacks, no CUDA library/GPU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
LOCAL_HEADERS = Path('/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/sglang-integration/nvbit_headers')
REMOTE_HEADERS = Path('/home/xmu/nvidiagds/simulators/hyfiss/tracing-tool/nvbit')


def need(ok, why):
    if not ok:
        raise ValueError(why)


def pin(path):
    p = Path(path)
    raw = p.read_bytes()
    return dict(path=str(p.resolve()), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path)
    p.add_argument('--nvbit', type=Path, default=LOCAL_HEADERS if LOCAL_HEADERS.is_dir() else REMOTE_HEADERS)
    p.add_argument('--compiler', default=shutil.which('clang++') or 'c++')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    out = a.output.resolve() if a.output else Path(tempfile.mkdtemp(prefix='native-arguments-producer-'))
    if a.output:
        out.mkdir(parents=True, exist_ok=False)
    names = ['argument_capture.h', 'argument_runtime.inc', 'argument_plan.h', 'argument-plan.json',
             'observer.cu', 'argument_capture_test.cpp', 'argument_callback_test.cpp', 'test_producer.py']
    inputs = [pin(HERE / n) for n in names]
    reference = next(q for q in [HERE / 'manifest.json', HERE.parent / 'native-observer-r2/manifest.json',
                                HERE.parent / 'observer-r2/manifest.json'] if q.is_file())
    header_pins = json.loads(reference.read_text())['nvbit_headers']
    for name in ['generated_cuda_meta.h', 'cuda.h']:
        row = pin(a.nvbit / name)
        need(all(row[k] == header_pins[name][k] for k in ['bytes', 'sha256']), 'real pinned CUDA callback header')
        inputs.append(row)
    source = (HERE / 'observer.cu').read_text()
    begin = source.index('string launch_body(')
    end = source.index('\nbool allocation_api(', begin)
    (out / 'launch_body_under_test.inc').write_text(source[begin:end])
    # Check the real callback connection as well as executing its real body.
    need(source.count('capture_native_arguments(') == 1 and source.count('specific=launch_body(ctx,cbid,params,p.id,api);') == 1,
         'one argument capture call from the original entry callback')
    need('s.argument_ledger.complete(p.id,status&&*status==CUDA_SUCCESS);' in source and
         's.prefix("launch")+",\\"edge\\":\\"return\\""+p.body' in source,
         'return reuses original Pending body and closes ledger')
    need(source.count('nvbit_enable_instrumented(ctx,f,false);') == 1 and
         'nvbit_insert_call(' not in source and 'sgsample::' not in source,
         'no dynamic sampler introduced')
    need('const uint64_t DEFAULT_CAP = 1ull << 30;' in source and
         'bool pass=closed&&argument_ledger.closed()&&errors.empty()' in source,
         'original total cap and argument-aware success gate')
    env = dict(os.environ, CUDA_VISIBLE_DEVICES='', PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='detect_leaks=0')
    steps = []

    def run(name, argv, timeout=180):
        start = time.monotonic()
        stdout, stderr = out / (name + '.stdout'), out / (name + '.stderr')
        with stdout.open('xb') as o, stderr.open('xb') as e:
            r = subprocess.run(argv, stdout=o, stderr=e, env=env, timeout=timeout)
        steps.append(dict(name=name, argv=argv, seconds=time.monotonic()-start, returncode=r.returncode,
                          stdout=pin(stdout), stderr=pin(stderr)))
        need(r.returncode == 0, 'host test failed: ' + name + '; see ' + str(stderr))
        return stdout

    flags = ['-std=c++11', '-O0', '-g0', '-Wno-deprecated-declarations', '-I'+str(HERE), '-I'+str(out), '-I'+str(a.nvbit)]
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    libs = [] if os.uname().sysname == 'Darwin' else ['-lcrypto']
    for test in ['argument_capture_test', 'argument_callback_test']:
        run(test+'-compile', [a.compiler, *flags, str(HERE/(test+'.cpp')), '-o', str(out/test), *libs])
    standalone = json.loads(run('ledger', [str(out/'argument_capture_test')]).read_text())
    need(standalone['status'] == 'PASS_HOST_ARGUMENT_CAPTURE' and standalone['checks'] >= 100, 'ledger checks')
    run('four-api', [str(out/'argument_callback_test'), 'four-api'])
    for mode in ['packed', 'null_function', 'null_config', 'bad_api', 'resource_failure', 'journal_cap']:
        j = json.loads(run(mode, [str(out/'argument_callback_test'), mode]).read_text())
        need(j['status'] == 'EXPECTED_CALLBACK_REJECTION', 'negative callback refused')
    full = run('actual-full-plan', [str(out/'argument_callback_test'), 'full'])
    plan = json.loads((HERE/'argument-plan.json').read_text())['launches']
    arguments = raw_bytes = rows = 0
    summary = None
    with full.open('rb') as f:
        for raw in f:
            j = json.loads(raw)
            if 'status' in j:
                need(summary is None, 'one callback summary')
                summary = j
                continue
            need(summary is None and rows < len(plan), 'row before summary')
            row, body, expected = j['argument'], j['body'], plan[rows]
            payload = raw.split(b',"body":', 1)[0][len(b'{"argument":'):]
            need(body['native_argument_record']['payload_sha256'] == hashlib.sha256(payload).hexdigest(), 'exact payload reference SHA')
            need(body['native_argument_record']['sequence'] == row['sequence'] == rows and
                 body['parameter_values_captured'] is True, 'actual callback captures once in order')
            for key in ['source_launch_key', 'epoch_id', 'epoch_launch_ordinal', 'phase', 'code_sha256',
                        'code_sha256_kind', 'parameter_layout_sha256', 'grid', 'block', 'cuda_api']:
                need(row[key] == expected[key], 'actual generated plan ' + key)
            need([v['size_bytes'] for v in row['arguments']] == expected['argument_sizes'], 'actual size vector')
            for n, value in enumerate(row['arguments']):
                expected_bytes = bytes((rows*7+n*13+k*17) & 255 for k in range(value['size_bytes']))
                need(value['index'] == n and value['parameter_buffer_offset'] is None and
                     bytes.fromhex(value['raw_bytes_hex']) == expected_bytes and
                     value['sha256'] == hashlib.sha256(expected_bytes).hexdigest(), 'independent actual host byte oracle')
                arguments += 1
                raw_bytes += len(expected_bytes)
            rows += 1
    need(summary and summary['status'] == 'PASS_ACTUAL_ARGUMENT_CALLBACK' and summary['launches'] == rows == len(plan), 'full generated plan closure')
    need(summary['arguments'] == arguments and summary['raw_bytes'] == raw_bytes, 'independent CPU payload totals')
    need(inputs == [pin(r['path']) for r in inputs], 'source/header identity unchanged during tests')
    result = dict(status='PASS_CPU_ONLY_ARGUMENT_PRODUCER', GPU_executed=False, dynamic_instrumentation=False,
                  sanitizer='address,undefined' if a.sanitize else None, sanitizer_leak_detection=False if a.sanitize else None,
                  ledger=standalone, actual_callback=summary, inputs=inputs, steps=steps, output=str(out))
    (out/'receipt.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
