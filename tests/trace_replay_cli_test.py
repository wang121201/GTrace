#!/usr/bin/env python3
"""Check replay.py source binding and failure publication using a real direct run.

The real trace is replayed once. Negative cases use a clearly marked synthetic
one-record derivative: the first real record is retained byte-for-byte and its
footer, trace receipt and per-call DRAM census are recomputed. This derivative
is a parser/CLI fixture, never a replacement workload result or qualification.
Only the fresh --output tree is written; original inputs remain unchanged.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HEADER, RECORD, FOOTER = 96, 144, 128
UNKNOWN = (1 << 64)-1


def require(condition, reason):
    if not condition:
        raise AssertionError(reason)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def load(path):
    return json.loads(path.read_text())


def mutate_json(path, change):
    value = load(path)
    change(value)
    write_json(path, value)


def payload_hash(words):
    value = 14695981039346656037
    for item in (words[0], words[8], words[9], words[13]):
        for byte in struct.pack('<Q', item):
            value = ((value ^ byte)*1099511628211) & UNKNOWN
    return value


def make_small_source(source, target):
    """Keep one authentic record, rebuilding only fixture census and envelope."""
    target.mkdir()
    result, receipt = load(source/'result.json'), load(source/'run-receipt.json')
    trace = result['trace']
    require(receipt['status'] == 'PASS' and receipt['mode'] == 'direct'
            and result['mode'] == 'direct', 'test requires a successful direct source')
    require(sha(source/'dram.tgn') == trace['file_sha256'], 'original trace SHA mismatch')
    require((source/'dram.tgn').stat().st_size == trace['file_bytes'], 'original trace size mismatch')
    with (source/'dram.tgn').open('rb') as stream:
        header, record = stream.read(HEADER), stream.read(RECORD)
    require(len(header) == HEADER and header[:8] == b'TGCSIM01', 'original trace header')
    require(struct.unpack_from('<QQQ', header, 8) == (RECORD, 1, 0), 'original trace must be functional direct')
    require(len(record) == RECORD, 'source must contain at least one full record')
    words = struct.unpack('<18Q', record)
    require(words[:2] == (0, 0) and words[2:6] == (UNKNOWN,)*4,
            'first real direct record ID/timestamp contract')
    require(words[13] in (0, 1) and words[9] == (128 if words[13] == 0 else 32),
            'first real record is not read128/write32')
    require(words[14] < len(result['pipeline']), 'first real record call index')
    read_requests, write_requests = int(words[13] == 0), int(words[13] == 1)
    read_bytes, write_bytes = read_requests*128, write_requests*32
    record_sha = hashlib.sha256(record).hexdigest()
    fnv = payload_hash(words)
    footer = (struct.pack('<Q', UNKNOWN)+b'TGCSEND1'
              +struct.pack('<6Q', 1, read_requests, write_requests, read_bytes, write_bytes, fnv)
              +record_sha.encode('ascii'))
    require(len(footer) == FOOTER, 'one-record footer size')
    data = header+record+footer
    (target/'dram.tgn').write_bytes(data)
    trace.update(path=str(target/'dram.tgn'), records=1, read_requests=read_requests,
                 write_requests=write_requests, read_bytes=read_bytes, write_bytes=write_bytes,
                 file_bytes=len(data), record_sha256=record_sha,
                 file_sha256=hashlib.sha256(data).hexdigest(), request_payload_fnv1a64=fnv)
    for row in result['pipeline']:
        row['DRAM_read_bytes'] = row['DRAM_write_bytes'] = 0
    result['pipeline'][words[14]]['DRAM_read_bytes'] = read_bytes
    result['pipeline'][words[14]]['DRAM_write_bytes'] = write_bytes
    result['cache']['DRAM_read_bytes'] = read_bytes
    result['cache']['DRAM_write_bytes'] = write_bytes
    fixture = dict(synthetic_test_fixture=True, NOT_A_WORKLOAD_RESULT=True,
                   derivation='First original trace record unchanged; rebuilt one-record footer and required DRAM census.',
                   original_source_run=str(source), original_trace_sha256=sha(source/'dram.tgn'),
                   retained_record_sha256=record_sha, retained_original_call_index=words[14])
    # Preserve the real sealed control and selected labels; zero-record calls
    # remain selected, as allowed by the CLI. Do not pretend other aggregate
    # source fields describe this deliberately reduced test trace.
    result['test_fixture'] = fixture
    receipt['test_fixture'] = fixture
    write_json(target/'result.json', result)
    write_json(target/'run-receipt.json', receipt)
    write_json(target/'fixture-provenance.json', fixture)
    return fixture


def clone_source(base, target):
    target.mkdir()
    for name in ('result.json', 'run-receipt.json'):
        (target/name).write_bytes((base/name).read_bytes())
    os.link(base/'dram.tgn', target/'dram.tgn')


def replace_trace(source, data, update_expected_sha=False):
    """Break the fixture hard link before writing; never mutate shared bytes."""
    (source/'dram.tgn').unlink()
    (source/'dram.tgn').write_bytes(data)
    if update_expected_sha:
        def change(result):
            result['trace']['file_bytes'] = len(data)
            result['trace']['file_sha256'] = hashlib.sha256(data).hexdigest()
        mutate_json(source/'result.json', change)


def launch_proxy(path, binary, action):
    """Tamper only a generated test snapshot between wrapper and real engine."""
    script = f'''#!{sys.executable}
import json
import os
from pathlib import Path
import sys
p = Path(sys.argv[1])
spec = json.loads(p.read_text())
action = {action!r}
if action == 'cfg_expected_sha':
    spec['native_config']['sha256'] = '0'*64
    p.write_text(json.dumps(spec, indent=2)+'\\n')
elif action == 'cfg_snapshot_bytes':
    cfg = Path(spec['native_config']['path'])
    cfg.write_bytes(cfg.read_bytes()+b'\\n# controlled CLI negative fixture\\n')
else:
    raise RuntimeError('unknown test mutation')
os.execv({str(binary)!r}, [{str(binary)!r}, str(p)])
'''
    path.write_text(script)
    path.chmod(0o700)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--source-run', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout-seconds', type=float, default=300,
                        help='Per replay child deadline; positive real source runs once')
    args = parser.parse_args()
    if args.timeout_seconds <= 0:
        parser.error('timeout must be positive')
    binary, source, original, out = (path.resolve() for path in
                                   (args.binary, args.source_run, args.input, args.output))
    out.mkdir(parents=True, exist_ok=False)
    state = dict(schema='TILEGEN_TRACE_REPLAY_CLI_TEST_V1', status='RUNNING',
                 CPU_only=True, GPU_executed=False, compilation_executed=False,
                 original_source_run=str(source), original_input=str(original), binary=str(binary),
                 cases=[], synthetic_fixture_scope='CLI validation only; never a workload result',
                 test_source_sha256=sha(Path(__file__)))
    started = time.monotonic()
    pins = {}

    def save():
        write_json(out/'summary.json', state)

    def invoke(name, source_run, input_path=original, expected_error=None,
               selected_binary=binary, extra=(), mutation=None):
        case = out/name
        case.mkdir(exist_ok=True)
        destination = case/'run'
        command = [sys.executable, str(ROOT/'replay.py'), '--binary', str(selected_binary),
                   '--source-run', str(source_run), '--input', str(input_path),
                   '--output', str(destination), '--timeout-seconds', str(args.timeout_seconds), *extra]
        begin = time.monotonic()
        with (case/'wrapper.stdout').open('wb') as stdout, (case/'wrapper.stderr').open('wb') as stderr:
            process = subprocess.run(command, stdout=stdout, stderr=stderr,
                                     timeout=args.timeout_seconds+30, check=False)
        row = dict(name=name, command=command, exit_code=process.returncode,
                   elapsed_seconds=time.monotonic()-begin,
                   expected='PASS' if expected_error is None else 'REJECT',
                   controlled_mutation=mutation, status='FAILED')
        state['cases'].append(row)
        receipt = load(destination/'run-receipt.json')
        row['run_status'] = receipt.get('status')
        row['result_published'] = (destination/'result.json').exists()
        partial = destination/'result.json.partial'
        row['partial_bytes'] = partial.stat().st_size if partial.exists() else None
        engine_error = (destination/'stderr.log').read_text() if (destination/'stderr.log').exists() else ''
        diagnostic = receipt.get('error', '')+'\n'+engine_error
        row['diagnostic'] = diagnostic.strip()
        if expected_error is None:
            require(process.returncode == 0 and receipt.get('status') == 'PASS'
                    and row['result_published'], name+': valid input did not publish a completed result')
            result = load(destination/'result.json')
            require(result['status'] == 'PASS_CLOSED_MEMORY_ONLY_REPLAY'
                    and result['byte_ledger_closed'] and result['request_ledger_closed'],
                    name+': positive completion ledger invalid')
            expected = load(source_run/'result.json')['trace']
            require(result['accepted'] == result['completed'] == expected['records']
                    and result['read_bytes'] == expected['read_bytes']
                    and result['write_bytes'] == expected['write_bytes']
                    and result['trace']['file_sha256'] == expected['file_sha256'],
                    name+': positive replay changed source trace census')
            row['requests'] = result['requests']
            row['result_sha256'] = sha(destination/'result.json')
        else:
            require(process.returncode != 0 and receipt.get('status') == 'FAILED',
                    name+': expected deterministic rejection, not PASS/timeout')
            require(not row['result_published'], name+': failure published result.json')
            require(expected_error in diagnostic,
                    name+': rejected at wrong guard; expected '+repr(expected_error))
        row['status'] = 'PASS';save()
        return destination

    def negative(name, mutate, expected_error, input_path=original, extra=(), note=None):
        case = out/name;case.mkdir()
        fixture = case/'source';clone_source(out/'one-record-source', fixture)
        mutate(fixture)
        return invoke(name, fixture, input_path, expected_error, extra=extra, mutation=note)

    try:
        require(binary.is_file(), 'replay binary missing')
        with original.open('rb') as stream:
            control = json.loads(stream.readline((1 << 20)+1))
        cfg = Path(control['decoded_control']['memory_model']['native_hbfsim_config_file'])
        require(cfg.is_absolute(), 'source config path is not absolute')
        files = [binary, original, cfg, source/'result.json', source/'run-receipt.json',
                 source/'dram.tgn', ROOT/'replay.py']
        pins = {str(path): sha(path) for path in files}
        state['original_file_sha256'] = pins;save()

        invoke('positive-real-source', source)
        state['one_record_fixture'] = make_small_source(source, out/'one-record-source')
        # Qualify the reduced envelope before interpreting any negative case.
        invoke('positive-one-record-fixture', out/'one-record-source')

        mismatch = out/'wrong-original.input';mismatch.write_bytes(b'not the original sealed input\n')
        negative('source-input-mismatch', lambda _: None, 'original input differs', mismatch,
                 note='Different local input fixture; original input and receipt hash unchanged')
        negative('missing-trace', lambda path: (path/'dram.tgn').unlink(), 'trace missing or size differs')
        negative('missing-trace-hash', lambda path: mutate_json(path/'result.json',
                 lambda result: result['trace'].pop('file_sha256')), 'file_sha256')
        negative('wrong-trace-hash', lambda path: mutate_json(path/'result.json',
                 lambda result: result['trace'].update(file_sha256='0'*64)), 'trace whole-file SHA mismatch',
                 note='Intentionally changed expected SHA only; trace bytes remain original fixture bytes')

        def bad_footer(path):
            data = bytearray((path/'dram.tgn').read_bytes());data[-1] = ord('x')
            replace_trace(path, bytes(data), update_expected_sha=True)
        negative('invalid-footer-with-matching-file-sha', bad_footer, 'trace record SHA mismatch',
                 note='Corrupt footer record digest; deliberately recompute expected whole-file SHA to test internal footer validation')
        negative('truncated-footer-with-matching-file-sha',
                 lambda path: replace_trace(path, (path/'dram.tgn').read_bytes()[:-1], True),
                 'truncated trace', note='Shorten fixture footer; update only expected file bytes/SHA so C++ parses and rejects it')
        negative('context-mismatch', lambda path: mutate_json(path/'result.json',
                 lambda result: result.update(transport_control_sha256='0'*64)), 'direct transport context differs')
        negative('call-label-mismatch', lambda path: mutate_json(path/'result.json',
                 lambda result: result['pipeline'][0].update(source_launch_key='controlled-wrong-call-label')),
                 'pipeline call label/order differs')
        negative('source-aggregate-mismatch', lambda path: mutate_json(path/'result.json',
                 lambda result: result['cache'].update(DRAM_read_bytes=result['cache']['DRAM_read_bytes']+128)),
                 'source cache/trace byte census differs')
        selected_call = state['one_record_fixture']['retained_original_call_index']
        negative('source-per-call-mismatch', lambda path: mutate_json(path/'result.json',
                 lambda result: result['pipeline'][selected_call].update(
                     DRAM_read_bytes=result['pipeline'][selected_call]['DRAM_read_bytes']+128)),
                 'replay per-call traffic differs')

        def native_source(path):
            mutate_json(path/'result.json', lambda result: result.update(mode='cosim'))
            mutate_json(path/'run-receipt.json', lambda receipt: receipt.update(mode='cosim'))
        negative('native-cosim-source-rejected', native_source, 'source must be a successful direct trace run')
        negative('native-cosim-trace-receipt-rejected', lambda path: mutate_json(path/'result.json',
                 lambda result: result['trace'].update(mode='NATIVE_COSIM_ADMITTED_REQUESTS')),
                 'source trace receipt must be closed direct trace')
        negative('cycle-budget-failure', lambda _: None, 'cycle budget', extra=('--max-cycles', '1'))

        for name, action in (('cfg-sha-mismatch', 'cfg_expected_sha'),
                             ('cfg-snapshot-changed', 'cfg_snapshot_bytes')):
            case = out/name;case.mkdir()
            proxy = case/'launch-mutator.py';launch_proxy(proxy, binary, action)
            invoke(name, out/'one-record-source', expected_error='replay metadata SHA mismatch',
                   selected_binary=proxy, mutation='Test-only launcher modifies generated cfg snapshot/manifest before exec of the real binary: '+action)
        state['status'] = 'PASS_TRACE_REPLAY_CLI_BINDING_AND_FAILURE_PUBLICATION'
    except Exception as error:
        state.update(status='FAILED', error=type(error).__name__+': '+str(error))
    finally:
        try:
            unchanged = all(Path(path).is_file() and sha(Path(path)) == digest for path, digest in pins.items())
        except OSError:
            unchanged = False
        state['original_files_unchanged'] = unchanged
        if not unchanged:
            state.update(status='FAILED', source_integrity_error='An original source/input/config/binary/wrapper changed during test')
        state['elapsed_seconds'] = time.monotonic()-started
        state['passed_cases'] = sum(row['status'] == 'PASS' for row in state['cases'])
        save()
    print(json.dumps(state, indent=2))
    return 0 if state['status'].startswith('PASS_') else 1


if __name__ == '__main__':
    raise SystemExit(main())
