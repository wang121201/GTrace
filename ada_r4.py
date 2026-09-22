#!/usr/bin/env python3
"""Opt-in frozen r4 serial read comparison using GTSim's shared L1 implementation.

Run where the request files reside. This entry point records request hashes and
derived counters, never copies allocation-relative requests to another host.
It does not accept writes, model GPU timing, or change ada_profile.py defaults.
"""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent
PROFILE = ROOT / 'configs/rtx4000-ada-r4-serial'
PINS = {
    'candidate_parameters.json': '5f4c729a924a2898afc829379d878b8aefe6496265c92e03412cb6acfbeb59c1',
    'cache_policy_replay_r4.cpp': '9319661e7abe613cc9bdb81ce488d1b3448aab6e87ad085e394fe92e8db042aa',
}


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def source_pins():
    paths = [ROOT / 'ada_r4.py', ROOT / 'build-config.json']
    paths += list((ROOT / 'source').rglob('*')) + list(PROFILE.rglob('*'))
    return {str(p.relative_to(ROOT)): sha(p) for p in sorted(paths) if p.is_file()}


def dataset_pins(cases_path):
    # Request bytes remain in their original files. Receipt contains only IDs,
    # byte counts and SHA-256; paths or addresses are not printed.
    result = {}
    for line in cases_path.read_text().splitlines():
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 5:
            raise ValueError('invalid five-field case manifest')
        case_id, nominal, cg, expected, name = fields
        if case_id in result:
            raise ValueError('duplicate case ID')
        if not case_id.isdecimal() or int(nominal) not in (98304, 65536, 28672) or cg not in ('0', '1'):
            raise ValueError('unsupported case metadata')
        path = Path(name)
        if not path.is_absolute():
            raise ValueError('request paths must be absolute')
        size = path.stat().st_size
        if int(expected) <= 0 or size != 4 * int(expected):
            raise ValueError('request file length mismatch')
        result[case_id] = {'bytes': size, 'sha256': sha(path)}
    if not result:
        raise ValueError('empty case manifest')
    return result


def expected_profiles():
    params = json.loads((PROFILE / 'source/candidate_parameters.json').read_text())
    result = []
    for r4 in (False, True):
        for p in params['profiles']:
            sets = 16 if r4 else 4
            size = p['effective_l1_bytes'] if r4 else p['nominal_l1_bytes']
            result.append(dict(model_id=params['config_id'] if r4 else 'LRU_u128_s4_h0_c1000',
                observed_shared_bytes=p['observed_shared_kib'] * 1024,
                nominal_l1_bytes=p['nominal_l1_bytes'], capacity_bytes=size,
                sets=sets, ways=size // (128 * sets), policy='CLOCK' if r4 else 'LRU',
                allocation_unit=128, sector_bytes=32, hash=2 if r4 else 0,
                scale=1062 if r4 else 1000))
    return result


def verify_description(description):
    observed = description['profiles']
    expected = expected_profiles()
    key = lambda p: (p['model_id'], p['observed_shared_bytes'])
    if len(observed) != len(expected):
        raise ValueError('wrong number of compiled profile variants')
    for actual, wanted in zip(sorted(observed, key=key), sorted(expected, key=key)):
        if any(actual.get(k) != v for k, v in wanted.items()):
            raise ValueError('compiled profile differs from frozen parameters')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='Five-field, headerless cases manifest; requests remain on this host')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    parser.add_argument('--compiler', default='g++', help='C++20 compiler; recorded in receipt')
    args = parser.parse_args()
    out, src = args.output.resolve(), args.input.resolve()
    if out.exists():
        parser.error('output already exists; choose a new directory')
    compiler = shutil.which(args.compiler)
    if not compiler or not src.is_file():
        parser.error('compiler or input missing')
    if {name: sha(PROFILE / 'source' / name) for name in PINS} != PINS:
        parser.error('frozen r4 source identity mismatch')
    profile = json.loads((PROFILE / 'profile.json').read_text())
    if profile['source_sha256'] != PINS or profile['default_profile_unchanged'] != 'r2-adaptive':
        parser.error('profile identity mismatch')
    before, inputs = source_pins(), dataset_pins(src)
    out.mkdir(parents=True)
    receipt = dict(schema='GTSIM_ADA_R4_SERIAL_RECEIPT_V1', status='RUNNING',
        mode='synchronous serial read filter', source_sha256=before,
        input_manifest_sha256=sha(src), input_request_sha256=inputs,
        case_count=len(inputs), comparison_metric='L2 ingress read sectors',
        default_profile_unchanged='r2-adaptive', GPU_executed=False,
        hardware_accuracy_evaluated=False, CPU_units='minutes user+system')
    try:
        binary = out / 'ada_r4_serial_replay'
        build = json.loads((ROOT / 'build-config.json').read_text())
        includes = ['-I'+str(ROOT/p) for p in build['include_directories']]
        command = [compiler, '-std=c++20', '-O2', '-Wall', '-Wextra'] + includes + [
                   str(ROOT / 'source/ada_r4_serial_replay.cpp'), '-o', str(binary)]
        start = time.monotonic()
        with (out / 'compile.stdout').open('wb') as stdout, (out / 'compile.stderr').open('wb') as stderr:
            built = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=180)
        receipt['compile'] = dict(argv=command, returncode=built.returncode, wall_minutes=(time.monotonic()-start)/60)
        if built.returncode:
            raise ValueError('compile failed')
        receipt['binary_sha256'] = sha(binary)
        description = json.loads(subprocess.check_output([str(binary), '--describe'], timeout=30))
        verify_description(description)
        (out / 'compiled-profiles.json').write_text(json.dumps(description, indent=2)+'\n')
        selftest = json.loads(subprocess.check_output([str(binary), '--self-test'], timeout=30))
        if not str(selftest.get('status', '')).startswith('PASS'):
            raise ValueError('serial driver self-test failed')
        receipt['self_test'] = selftest
        start = time.monotonic()
        r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        with (out / 'predictions.jsonl').open('wb') as stdout, (out / 'run.stderr').open('wb') as stderr:
            process = subprocess.run([str(binary), str(src)], stdout=stdout, stderr=stderr, timeout=600)
        r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        receipt.update(returncode=process.returncode, wall_minutes=(time.monotonic()-start)/60,
                       cpu_minutes=(r1.ru_utime+r1.ru_stime-r0.ru_utime-r0.ru_stime)/60)
        if process.returncode:
            raise ValueError('serial replay failed; partial output is not accepted')
        rows = [json.loads(line) for line in (out / 'predictions.jsonl').read_text().splitlines()]
        expected = {(p['model_id'], int(i)) for p in expected_profiles() for i in inputs}
        actual = {(p['model_id'], int(p['case_id'])) for p in rows}
        if actual != expected or len(rows) != 2*len(inputs) or any(p.get('status') != 'PASS' for p in rows):
            raise ValueError('incomplete or duplicate output cases')
        receipt['source_unchanged'] = before == source_pins()
        receipt['input_unchanged'] = inputs == dataset_pins(src) and receipt['input_manifest_sha256'] == sha(src)
        if not receipt['source_unchanged'] or not receipt['input_unchanged']:
            raise ValueError('source or inputs changed during replay')
        receipt['prediction_sha256'] = sha(out / 'predictions.jsonl')
        receipt['status'] = 'PASS_PROCESS_ONLY'
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        # Do not include errors from a request parser: it might echo raw data.
        receipt['status'] = 'FAILED'
        receipt['error_type'] = type(error).__name__
        receipt['error'] = str(error) if isinstance(error, ValueError) else 'see on-host logs'
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps({k: receipt[k] for k in ('status', 'case_count', 'wall_minutes', 'cpu_minutes', 'error') if k in receipt}))
    return 0 if receipt['status'] == 'PASS_PROCESS_ONLY' else 1


if __name__ == '__main__':
    raise SystemExit(main())
