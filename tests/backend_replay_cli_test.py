#!/usr/bin/env python3
"""Bounded target-backend CLI contract checks; fixtures are NOT performance workloads."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
UNKNOWN = (1 << 64)-1


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def fixture(source, target):
    """Two real read addresses on different pages; synthetic IDs/stage work/census."""
    target.mkdir()
    result = json.loads((source/'result.json').read_text())
    receipt = json.loads((source/'run-receipt.json').read_text())
    assert sha(source/'dram.tgn') == result['trace']['file_sha256']
    with (source/'dram.tgn').open('rb') as stream:
        header = stream.read(96)
        first = struct.unpack('<18Q', stream.read(144))
        assert first[0:2] == (0, 0) and first[13:16] == (0, 0, 0)
        second = None
        for _ in range(1, result['trace']['records']):
            words = struct.unpack('<18Q', stream.read(144))
            if words[13:16] == (0, 0, 0) and words[8]//4096 != first[8]//4096:
                second = words
                break
        assert second is not None, 'need two real call-0/CTA-0 reads on distinct service pages'
    selected_ids = [first[0], second[0]]
    second = (1, 1)+second[2:]
    record_bytes = b''.join(struct.pack('<18Q', *words) for words in (first, second))
    record_sha = hashlib.sha256(record_bytes).hexdigest()
    fnv = 14695981039346656037
    for words in (first, second):
        for field in (words[0], words[8], words[9], words[13]):
            for byte in struct.pack('<Q', field):
                fnv = ((fnv ^ byte)*1099511628211) & UNKNOWN
    footer = struct.pack('<Q', UNKNOWN)+b'TGCSEND1'+struct.pack('<6Q', 2, 2, 0, 256, 0, fnv)+record_sha.encode()
    data = header+record_bytes+footer
    (target/'dram.tgn').write_bytes(data)
    result['trace'].update(path=str(target/'dram.tgn'), records=2, read_requests=2, write_requests=0,
                           read_bytes=256, write_bytes=0, file_bytes=len(data), record_sha256=record_sha,
                           file_sha256=hashlib.sha256(data).hexdigest(), request_payload_fnv1a64=fnv)
    stages = []
    for i, call in enumerate(result['pipeline']):
        call['DRAM_read_bytes'], call['DRAM_write_bytes'] = (256 if i == 0 else 0), 0
        stages.append(dict(id=i, call_index=i, record_begin=0 if i == 0 else 2, record_end=2,
                           cta_begin=0, cta_end=call['CTAs'], compute_cycles=10))
    result['cache'].update(DRAM_read_bytes=256, DRAM_write_bytes=0)
    result['phase_profile'].update(stages=stages, record_count=2)
    note = dict(synthetic_test_fixture=True, NOT_A_WORKLOAD_RESULT=True,
                original_source=str(source), original_trace_sha256=sha(source/'dram.tgn'),
                original_record_ids=selected_ids,
                derivation='Two real read addresses; second ID reset to 1; trace envelope/census rebuilt; synthetic 10-cycle stage per selected call.')
    result['test_fixture'] = receipt['test_fixture'] = note
    dump(target/'result.json', result)
    receipt.update(result_sha256=sha(target/'result.json'), phase_profile_exported=True)
    dump(target/'run-receipt.json', receipt)
    dump(target/'fixture.json', note)
    return note


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'source-run', 'input', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    source, binary, original, out = (p.resolve() for p in (args.source_run, args.binary, args.input, args.output))
    out.mkdir(parents=True, exist_ok=False)
    with original.open('rb') as stream:
        control = json.loads(stream.readline())
    gddr = Path(control['decoded_control']['memory_model']['native_hbfsim_config_file'])
    hbm, hbf = ROOT/'configs/hbm-one-stack-stage.cfg', ROOT/'configs/hbf-one-stack-stage.cfg'
    protected = [binary, original, ROOT/'replay.py', Path(__file__).resolve(), gddr, hbm, hbf, source/'result.json', source/'run-receipt.json', source/'dram.tgn']
    pins = {str(p): sha(p) for p in protected}
    started = time.monotonic()
    state = dict(schema='TILEGEN_TARGET_BACKEND_CLI_TEST_V1', status='RUNNING', CPU_only=True,
                 cases=[], original_file_sha256=pins, synthetic_fixture_scope='CLI correctness only; not performance or hardware calibration')
    def save():
        dump(out/'summary.json', state)
    def run(name, backend, cfg=None, good=False, mode='stage-overlap', extra=(), mutate=None, proxy=False, error=None):
        src = out/'two-record-source'
        if mutate:
            src = out/(name+'-source');shutil.copytree(out/'two-record-source', src)
            result = json.loads((src/'result.json').read_text());mutate(result);dump(src/'result.json', result)
            receipt = json.loads((src/'run-receipt.json').read_text())
            receipt['result_sha256'] = sha(src/'result.json');dump(src/'run-receipt.json', receipt)
        selected_binary = binary
        if proxy:
            selected_binary = out/(name+'-proxy.py')
            selected_binary.write_text('#!/usr/bin/env python3\nimport json,os,sys\nfrom pathlib import Path\n'
                'p=Path(sys.argv[1]); d=json.loads(p.read_text()); d["target_config"]["sha256"]="0"*64\n'
                'p.write_text(json.dumps(d)+"\\n")\n'+f'os.execv({str(binary)!r},[{str(binary)!r},str(p)])\n')
            selected_binary.chmod(0o755)
        target = out/(name+'-run')
        command = [sys.executable, str(ROOT/'replay.py'), '--source-run', str(src), '--input', str(original),
                   '--binary', str(selected_binary), '--output', str(target), '--backend', backend, '--mode', mode,
                   '--timeout-seconds', '30']
        if cfg is not None:
            command += ['--backend-config', str(cfg)]
        child = subprocess.run(command+list(extra), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=40)
        receipt = json.loads((target/'run-receipt.json').read_text()) if (target/'run-receipt.json').exists() else {}
        diagnostic = child.stderr.decode(errors='replace')+'\n'+receipt.get('error', '')
        if (target/'stderr.log').exists():
            diagnostic += '\n'+(target/'stderr.log').read_text()
        published = (target/'result.json').exists()
        ok = ((child.returncode == 0 and receipt.get('status') == 'PASS' and published) if good else
              (child.returncode != 0 and not published and receipt.get('status') in (None, 'FAILED')))
        if error is not None:
            ok = ok and error in diagnostic
        if good and ok:
            result = json.loads((target/'result.json').read_text())
            expected = json.loads((src/'result.json').read_text())
            ok = (result['target_backend']['kind'] == backend and result['read_bytes'] == 256 and result['write_bytes'] == 0
                  and result['trace']['file_sha256'] == expected['trace']['file_sha256']
                  and result['phase_report']['ledger_closed'] is True and result['accepted'] == result['completed'] == 2)
            if backend == 'hbf':
                ok = ok and result['hbf_initial_image']['pages'] == 2 and result['physical']['kind'] == 'HBF_LOGICAL_CONTROLLER_NAND'
        row = dict(name=name, status='PASS' if ok else 'FAIL', expected_success=good, exit_code=child.returncode,
                   receipt_status=receipt.get('status'), result_published=published, diagnostic=diagnostic.strip(), command=command+list(extra))
        state['cases'].append(row);save()
        if not ok:
            raise AssertionError(json.dumps(row))
    try:
        state['fixture'] = fixture(source, out/'two-record-source');save()
        for kind, cfg in (('source-gddr6', None), ('gddr6', gddr), ('hbm', hbm), ('hbf', hbf)):
            run('positive-'+kind, kind, cfg, good=True)
        run('hbm-with-gddr6-overlay-rejected', 'hbm', gddr)
        run('gddr6-with-hbm-config-rejected', 'gddr6', hbm)
        run('hbf-without-explicit-hbf-declaration-rejected', 'hbf', gddr)
        for kind in ('gddr6', 'hbm', 'hbf'):
            run('missing-config-'+kind, kind, error='requires --backend-config')
        run('source-config-override', 'source-gddr6', gddr, error='source-gddr6 uses sealed source config')
        run('missing-config-file', 'hbm', out/'absent.cfg', error='No such file')
        damaged = out/'damaged.cfg';damaged.write_text(hbm.read_text()+'\ninvalid-test-only-setting=1\n')
        run('damaged-config', 'hbm', damaged, error='key is not owned by the simulator engine')
        run('target-config-sha', 'hbm', hbm, proxy=True, error='replay metadata SHA mismatch')
        run('hbf-memory-only', 'hbf', hbf, mode='memory-only', error='requires stage-overlap')
        for limit in (0, 513):
            run('hbf-parent-limit-'+str(limit), 'hbf', hbf, extra=('--hbf-max-live', str(limit)), error='invalid HBF budget')
        for limit in (0, 8_388_609):
            run('hbf-seed-limit-'+str(limit), 'hbf', hbf, extra=('--max-hbf-seed-pages', str(limit)), error='invalid HBF budget')
        run('hbf-touched-page-budget', 'hbf', hbf, extra=('--max-hbf-seed-pages', '1'), error='seed page budget exceeded')
        run('hbf-trace-identity', 'hbf', hbf, mutate=lambda r:r['trace'].update(file_sha256='0'*64), error='trace whole-file SHA mismatch')
        run('hbm-context-identity', 'hbm', hbm, mutate=lambda r:r.update(transport_control_sha256='0'*64), error='direct transport context differs')
        state['status'] = 'PASS'
    except Exception as error:
        state.update(status='FAILED', error=type(error).__name__+': '+str(error))
    finally:
        state['original_files_unchanged'] = all(p.is_file() and sha(p) == pins[str(p)] for p in protected)
        if not state['original_files_unchanged']:
            state.update(status='FAILED', source_integrity_error='Protected source/input/config/binary/wrapper changed')
        state['elapsed_seconds'] = time.monotonic()-started
        state['passed_cases'] = sum(c['status'] == 'PASS' for c in state['cases']);save()
    print(json.dumps({k:state[k] for k in ('status','passed_cases','original_files_unchanged','elapsed_seconds')}))
    return 0 if state['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
