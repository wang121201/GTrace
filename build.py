#!/usr/bin/env python3
"""Build the self-contained CPU native engine; never launches a simulation."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_pins():
    return {str(p.relative_to(ROOT)): sha(p) for p in sorted((ROOT/'source').rglob('*')) if p.is_file()}


def build(args):
    config_path = ROOT / ('replay-build-config.json' if args.replay else 'build-config.json')
    config = json.loads(config_path.read_text())
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    compiler = args.compiler or os.environ.get('CXX', config['compiler'])
    flags = ['-std=' + config['standard'], '-O' + args.optimization, '-Wall', '-Wextra', '-pthread']
    flags += ['-D' + x for x in config['definitions']]
    flags += ['-I' + str(ROOT / x) for x in config['include_directories']]
    if args.native:
        flags.append('-mcpu=native' if platform.machine() in ('arm64', 'aarch64') else '-march=native')
    if args.thin_lto:
        flags.append('-flto=thin')
    units = [ROOT / x for x in config['translation_units']]
    receipt = dict(schema='TILEGEN_NATIVE_BUILD_RECEIPT_V1', status='BUILDING',
                   CPU_only=True, simulation_executed=False, GPU_executed=False,
                   platform=platform.platform(), compiler=compiler, flags=flags,
                   jobs=args.jobs, build_config_sha256=sha(config_path), build_config=str(config_path),
                   steps=[])
    began = time.monotonic()
    receipt['source_pins'] = source_pins()

    def invoke(name, command):
        started = time.monotonic()
        with (out / (name + '.stdout')).open('wb') as stdout, (out / (name + '.stderr')).open('wb') as stderr:
            result = subprocess.run(command, cwd=ROOT, stdout=stdout, stderr=stderr)
        return dict(name=name, command=command, returncode=result.returncode,
                    wall_seconds=time.monotonic()-started)

    def compile_unit(item):
        i, unit = item
        target = out / f'{i:02d}.o'
        if args.deps_only:
            command = [compiler, *flags, '-MM', '-MT', f'unit_{i}', str(unit)]
        else:
            command = [compiler, *flags, '-MMD', '-MF', str(out / f'{i:02d}.d'), '-c', str(unit), '-o', str(target)]
        return invoke(f'{i:02d}', command)

    try:
        receipt['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            receipt['steps'] = list(pool.map(compile_unit, enumerate(units)))
        if any(x['returncode'] for x in receipt['steps']):
            raise RuntimeError('compilation/dependency scan failed; inspect numbered stderr logs')
        if args.deps_only:
            dependencies = set()
            for i in range(len(units)):
                raw = (out / f'{i:02d}.stdout').read_text().replace('\\\n', ' ')
                dependencies.update(Path(x).resolve() for x in shlex.split(raw.split(':', 1)[1]))
            for path in dependencies:
                if not path.is_relative_to(ROOT / 'source'):
                    raise RuntimeError('build dependency escapes imported source: ' + str(path))
            receipt['local_dependency_files'] = len(dependencies)
            receipt['status'] = 'PASS_SELF_CONTAINED_DEPENDENCY_SCAN_NO_BUILD'
        else:
            executable = out / config['executable']
            command = [compiler, *flags, *(str(out / f'{i:02d}.o') for i in range(len(units))),
                       *('-l'+x for x in config['libraries']), '-o', str(executable)]
            receipt['steps'].append(invoke('link', command))
            if receipt['steps'][-1]['returncode']:
                raise RuntimeError('link failed; inspect link.stderr')
            receipt.update(status='PASS_BUILD_ONLY_NO_SIMULATION',
                           binary=dict(path=str(executable), bytes=executable.stat().st_size, sha256=sha(executable)))
        receipt['sources_unchanged'] = source_pins() == receipt['source_pins']
        if not receipt['sources_unchanged']:
            raise RuntimeError('source changed during build; rebuild before using this binary')
    except BaseException as error:
        receipt.update(status='FAIL_BUILD', error=type(error).__name__ + ': ' + str(error))
    finally:
        receipt['wall_seconds'] = time.monotonic()-began
        (out / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k:receipt[k] for k in ('status','wall_seconds','error','binary','local_dependency_files') if k in receipt}))
    return 0 if receipt['status'].startswith('PASS_') else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='Fresh build directory')
    parser.add_argument('--replay', action='store_true', help='Build standalone post-cache HBFSIM replay without GPU simulator objects')
    parser.add_argument('--compiler', help='C++20 compiler executable, defaults to CXX or clang++')
    parser.add_argument('--jobs', type=int, choices=(1,2), default=2)
    parser.add_argument('--optimization', choices=('0','1','2','3'), default='3')
    parser.add_argument('--native', action='store_true', help='Use architecture-appropriate native CPU flag')
    parser.add_argument('--thin-lto', action='store_true', help='Enable clang ThinLTO when its linker supports it')
    parser.add_argument('--deps-only', action='store_true', help='Verify all non-system includes stay within source/')
    return build(parser.parse_args())


if __name__ == '__main__':
    raise SystemExit(main())
