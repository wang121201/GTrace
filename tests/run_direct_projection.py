#!/usr/bin/env python3
"""Run native binding or independent prepared-memory checks on frozen input."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='Completed native build with 01..21.o')
    parser.add_argument('--input', type=Path, required=True, help='Frozen compressed transport covering nine families')
    parser.add_argument('--output', type=Path, required=True, help='Fresh directory for executable and receipts')
    parser.add_argument('--test', choices=('bindings', 'prepared-memory'), default='bindings')
    args = parser.parse_args()
    test_source = ROOT/'tests'/('direct_projection.cpp' if args.test == 'bindings' else 'prepared_memory_test.cpp')
    expected_status = ('PASS_ALL_NINE_NATIVE_BINDINGS_MATCH_ORIGINAL_FINE_BUILDERS' if args.test == 'bindings'
                       else 'PASS_GEMV_SILU_SHARED_MATERIALIZERS_MATCH_ORIGINAL_FORMULAS')
    build, source, out = args.build.resolve(), args.input.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = json.loads((ROOT / 'build-config.json').read_text())
    build_receipt = json.loads((build / 'build-receipt.json').read_text())
    if build_receipt['status'] != 'PASS_BUILD_ONLY_NO_SIMULATION':
        raise ValueError('projection test requires a completed native build')
    if not build_receipt.get('sources_unchanged') or not build_receipt.get('source_pins'):
        raise ValueError('projection test requires a source-pinned build')
    for path, expected in build_receipt['source_pins'].items():
        if sha(ROOT/path) != expected:
            raise ValueError('rebuild native objects after source changes: '+path)
    compiler = build_receipt['compiler']
    flags = build_receipt['flags']
    binary, obj = out / 'direct-projection', out / 'test.o'
    commands = [
        ('compile', [compiler, *flags, '-MMD', '-MF', str(out / 'test.d'), '-c',
                     str(test_source), '-o', str(obj)]),
        ('link', [compiler, *flags, str(obj),
                  *(str(build / f'{i:02d}.o') for i in range(1, len(config['translation_units']))),
                  *('-l' + x for x in config['libraries']), '-o', str(binary)]),
        ('run', [str(binary)]),
    ]
    receipt = dict(schema='DIRECT_NATIVE_BINDING_PROJECTION_TEST_V1', status='RUNNING',
                   GPU_executed=False, HBFSIM_executed=False, jobs=1,
                   input=str(source), input_sha256=sha(source),
                   original_build_receipt=str(build / 'build-receipt.json'),
                   original_build_receipt_sha256=sha(build / 'build-receipt.json'),
                   test=args.test, test_source=str(test_source), test_source_sha256=sha(test_source), steps=[])
    try:
        for name, command in commands:
            before = resource.getrusage(resource.RUSAGE_CHILDREN)
            started = time.monotonic()
            with source.open('rb') as stdin, (out / f'{name}.stdout').open('wb') as stdout, \
                    (out / f'{name}.stderr').open('wb') as stderr:
                completed = subprocess.run(command, cwd=ROOT, stdin=stdin if name == 'run' else subprocess.DEVNULL,
                                           stdout=stdout, stderr=stderr)
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            receipt['steps'].append(dict(name=name, command=command, exit_code=completed.returncode,
                                         wall_seconds=time.monotonic() - started,
                                         user_seconds=after.ru_utime-before.ru_utime,
                                         system_seconds=after.ru_stime-before.ru_stime))
            if completed.returncode:
                raise RuntimeError(name + ' failed; inspect ' + name + '.stderr')
        if sha(source) != receipt['input_sha256']:
            raise RuntimeError('input changed during test')
        for path, expected in build_receipt['source_pins'].items():
            if sha(ROOT/path) != expected:
                raise RuntimeError('source changed during projection test: '+path)
        result = json.loads((out / 'run.stdout').read_text())
        if result['status'] != expected_status:
            raise RuntimeError('exact projection comparison did not pass')
        receipt.update(status=expected_status,
                       binary_sha256=sha(binary), result_sha256=sha(out / 'run.stdout'))
    except Exception as error:
        receipt.update(status='FAIL', error=str(error))
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt))
    return 0 if receipt['status'].startswith('PASS') else 1


if __name__ == '__main__':
    raise SystemExit(main())
