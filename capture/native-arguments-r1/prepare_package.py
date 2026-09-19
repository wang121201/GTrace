#!/usr/bin/env python3
"""Seal tested local sources without rerunning a GPU or the successful tests."""
import argparse
import ast
import json
from pathlib import Path

from support import HERE, TASK, R2_SHA, CAPTURE_SHA, R1_SHA, identity, load_r1, need, sha


def pin(path):
    path = Path(path)
    need(path.is_file() and not path.is_symlink(), 'regular preflight input')
    return dict(path=str(path.resolve()), bytes=path.stat().st_size, sha256=sha(path))


def checked_test(path, required, status):
    result = json.loads(path.read_text())
    need(result['status'] == status, 'successful test receipt required')
    inputs = result['inputs']
    need({str(HERE / name) for name in required} <= {row['path'] for row in inputs},
         'receipt must cover actual test and implementation sources')
    for row in inputs:
        need(pin(row['path']) == row, 'source/header changed after CPU tests')
    return dict(receipt=pin(path), result=result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--producer-receipt', type=Path, required=True)
    parser.add_argument('--consumer-receipt', type=Path, required=True)
    args = parser.parse_args()
    for source in HERE.glob('*.py'):
        ast.parse(source.read_text(), filename=str(source))
    old = HERE.parent / 'native-observer-r2'
    identity(old, R2_SHA)
    load_r1()
    import make_argument_plan
    raw = (HERE / 'argument-plan.json').read_bytes()
    need(len(raw) <= 64 << 20, 'plan size envelope')
    plan = make_argument_plan.validate_plan(json.loads(raw))
    need(make_argument_plan.compile_header(raw) == (HERE / 'argument_plan.h').read_text(),
         'exact generated C++ header and embedded plan SHA')
    evidence = plan['source_evidence']
    for name in ('controller', 'census', 'observer_finish', 'build', 'observer_binary'):
        need(pin(evidence[name]['path']) == evidence[name], 'reference evidence changed')
    need(evidence['r2_manifest_sha256'] == R2_SHA and evidence['r1_manifest_sha256'] == R1_SHA and
         evidence['workload_manifest_sha256'] == CAPTURE_SHA, 'frozen dependency identity')
    producer = checked_test(args.producer_receipt,
        ['argument_capture.h', 'argument_runtime.inc', 'argument_plan.h', 'argument-plan.json',
         'observer.cu', 'argument_capture_test.cpp', 'argument_callback_test.cpp', 'test_producer.py'],
        'PASS_CPU_ONLY_ARGUMENT_PRODUCER')
    consumer = checked_test(args.consumer_receipt,
        ['consumer.py', 'make_argument_plan.py', 'test_arguments.py'],
        'PASS_CPU_ONLY_ARGUMENT_CONSUMER')
    build_names = {'build.py', 'observer.cu', 'argument_capture.h', 'argument_runtime.inc',
                   'argument_plan.h', 'argument-plan.json'}
    manifest = dict(schema='SG_NVBIT_NATIVE_ARGUMENT_MANIFEST_V1', revision='native-arguments-r1',
        status='TESTED_SOURCE_REQUIRES_REMOTE_NVCC_BUILD',
        build_inputs={name: {k: v for k, v in pin(HERE / name).items() if k != 'path'}
                      for name in sorted(build_names)},
        nvbit_headers=json.loads((old / 'manifest.json').read_text())['nvbit_headers'],
        reference_observer=dict(controller_sha256=evidence['controller']['sha256'],
                                census_sha256=evidence['census']['sha256'], package_sha256=R2_SHA),
        argument_plan_sha256=sha(HERE / 'argument-plan.json'), limits=plan['limits'],
        max_metadata_bytes=1 << 30, dynamic_instrumentation=False,
        dynamic_memory_addresses=False, typed_pointer_binding=False, native_model_admitted=False)
    (HERE / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    result = dict(status='PASS_CPU_ONLY_ARGUMENT_PACKAGE', GPU_executed=False, NVCC_executed=False,
                  remote_executed=False, producer=producer, consumer=consumer)
    (HERE / 'cpu-preflight.json').write_text(json.dumps(result, indent=2) + '\n')
    files = []
    for path in sorted(HERE.iterdir()):
        if path.is_file() and path.name != 'upload-manifest.json':
            row = pin(path)
            need(row['bytes'] <= 64 << 20, 'single source size envelope')
            row['path'] = path.name
            files.append(row)
    need(len(files) <= 128 and sum(row['bytes'] for row in files) <= 128 << 20, 'package envelope')
    upload = dict(schema='SGLANG_PD_NATIVE_ARGUMENT_UPLOAD_V1',
                  remote_directory=str(TASK / 'native-arguments-r1'), files=files)
    (HERE / 'upload-manifest.json').write_text(json.dumps(upload, indent=2) + '\n')
    own = identity(HERE)
    print(json.dumps(dict(status=result['status'], files=len(files),
                         manifest_sha256=own['manifest_sha256'], total_bytes=sum(x['bytes'] for x in files))))


if __name__ == '__main__':
    main()
