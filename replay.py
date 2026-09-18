#!/usr/bin/env python3
"""Replay a closed direct trace: memory-only or explicit approximate stage overlap."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

ROOT = Path(__file__).resolve().parent


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def bounded(path, cap):
    with path.open('rb') as stream:
        data = stream.read(cap+1)
    if len(data) > cap:
        raise ValueError('metadata exceeds cap: '+str(path))
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-run', type=Path, required=True, help='Successful direct run containing result.json, run-receipt.json, dram.tgn')
    parser.add_argument('--input', type=Path, required=True, help='Original sealed transport; only control JSON is decoded')
    parser.add_argument('--output', type=Path, required=True, help='Fresh output directory')
    parser.add_argument('--binary', type=Path, default=ROOT/'build/replay-r2/tilegen_replay')
    parser.add_argument('--mode', choices=('memory-only', 'stage-overlap'), default='memory-only')
    parser.add_argument('--prefetch-stages', type=int, choices=(1, 2, 4, 8), default=2,
                        help='Stage mode: maximum memory/compute-incomplete stages; 1 disables cross-stage overlap')
    parser.add_argument('--drain', choices=('global', 'independent'), default='global')
    parser.add_argument('--max-trace-bytes', type=int, default=8 << 30)
    parser.add_argument('--max-cycles', type=int, default=2_000_000_000)
    parser.add_argument('--timeout-seconds', type=float, default=3600)
    args = parser.parse_args()
    if not 224 <= args.max_trace_bytes <= 64 << 30 or not 0 < args.max_cycles <= (1 << 63)-1 or args.timeout_seconds <= 0:
        parser.error('invalid trace/cycle/time budget')
    binary, source, original, out = (p.resolve() for p in (args.binary, args.source_run, args.input, args.output))
    staged = args.mode == 'stage-overlap'
    mode = 'stage-overlap-replay' if staged else 'memory-only-replay'
    out.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    receipt = dict(schema='TILEGEN_STAGE_REPLAY_RUN_V1' if staged else 'TILEGEN_MEMORY_ONLY_REPLAY_RUN_V1', status='RUNNING',
                   mode=mode, CPU_only=True, GPU_sampling=False,
                   compute_scheduled=staged, GPU_stall_scheduled=False,
                   prefetch_stages=args.prefetch_stages if staged else None,
                   hardware_timing_calibrated=False)
    child_began = child_finished = before = after = None
    code = None
    try:
        if not binary.is_file():
            raise ValueError('build replay binary first: python3 build.py --replay --output build/replay-r2 --native --thin-lto')
        def snapshot(name, raw):
            path = out/name
            path.write_bytes(raw)
            return dict(path=str(path), sha256=hashlib.sha256(raw).hexdigest())
        result_bytes = bounded(source/'result.json', 128 << 20)
        run_bytes = bounded(source/'run-receipt.json', 1 << 20)
        result, source_receipt = json.loads(result_bytes), json.loads(run_bytes)
        if not isinstance(result, dict) or not isinstance(source_receipt, dict):
            raise ValueError('source result and receipt must be JSON objects')
        if (source_receipt.get('schema') != 'TILEGEN_TRACE_COSIM_RUN_V1' or source_receipt.get('returncode') != 0
                or result.get('version') != 'tilegen-trace-cosim-20260918-r1'):
            raise ValueError('source run schema/version/completion differs')
        if source_receipt.get('status') != 'PASS' or source_receipt.get('mode') != 'direct' or result.get('mode') != 'direct':
            raise ValueError('source must be a successful direct trace run')
        if staged:
            phases = result.get('phase_profile')
            if (not isinstance(phases, dict) or phases.get('schema') != 'TILEGEN_CTA_STAGE_PROFILE_V1'
                    or phases.get('qualification') != 'EXPLICIT_UNCALIBRATED_STAGE_APPROXIMATION'):
                raise ValueError('stage replay requires a direct run generated with --phase-ctas')
            if (source_receipt.get('phase_profile_exported') is not True
                    or source_receipt.get('result_sha256') != hashlib.sha256(result_bytes).hexdigest()):
                raise ValueError('stage profile/result differs from generating run receipt SHA')
        input_sha = sha(original)
        if input_sha != source_receipt.get('input_sha256'):
            raise ValueError('original input differs from direct run receipt')
        with original.open('rb') as stream:
            control_bytes = stream.readline((1 << 20)+1)
        if len(control_bytes) > 1 << 20 or not control_bytes.endswith(b'\n'):
            raise ValueError('missing/bounded transport control line')
        control = json.loads(control_bytes)
        declared_cfg = Path(control['decoded_control']['memory_model']['native_hbfsim_config_file'])
        if not declared_cfg.is_absolute():
            raise ValueError('source native config path must be absolute')
        trace = source/'dram.tgn'
        if trace.is_symlink() or not trace.is_file() or trace.stat().st_size != result['trace']['file_bytes']:
            raise ValueError('trace missing or size differs; complete integrity is checked by replay engine')
        spec = dict(schema='TILEGEN_TRACE_REPLAY_INPUT_V1', trace_file=str(trace),
            mode=mode, prefetch_stages=args.prefetch_stages,
            source_input_sha256=input_sha, source_input_file=str(original),
            control=snapshot('source-control.json', control_bytes),
            source_result=snapshot('source-result.json', result_bytes),
            source_run_receipt=snapshot('source-run-receipt.json', run_bytes),
            native_config=snapshot('native-memory.cfg', bounded(declared_cfg, 1 << 20)),
            max_trace_bytes=args.max_trace_bytes, max_cycles=args.max_cycles, drain=args.drain)
        spec_path = out/'replay-input.json'
        spec_path.write_text(json.dumps(spec, indent=2)+'\n')
        argv = [str(binary), str(spec_path)]
        receipt.update(argv=argv, binary_sha256=sha(binary), input_manifest_sha256=sha(spec_path),
                       source_trace_sha256=result['trace']['file_sha256'])
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        child_began = time.monotonic()
        with (out/'result.json.partial').open('wb') as stdout, (out/'stderr.log').open('wb') as stderr:
            process = subprocess.run(argv, stdout=stdout, stderr=stderr, timeout=args.timeout_seconds, check=False)
        child_finished = time.monotonic()
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        code = process.returncode
        if code:
            raise ValueError('HBFSIM replay failed; inspect stderr.log')
        replay = json.loads((out/'result.json.partial').read_text())
        expected = result['trace']
        if not isinstance(replay, dict):
            raise ValueError('replay result must be a JSON object')
        expected_schema = 'HBFSIM_POST_CACHE_STAGE_OVERLAP_REPLAY_V1' if staged else 'HBFSIM_POST_CACHE_MEMORY_ONLY_REPLAY_V1'
        expected_status = 'PASS_CLOSED_STAGE_OVERLAP_REPLAY' if staged else 'PASS_CLOSED_MEMORY_ONLY_REPLAY'
        if (replay.get('schema') != expected_schema or replay.get('status') != expected_status
                or replay.get('mode') != mode
                or replay.get('byte_ledger_closed') is not True or replay.get('request_ledger_closed') is not True
                or replay.get('accepted') != expected['records'] or replay.get('completed') != expected['records']
                or replay.get('read_bytes') != expected['read_bytes'] or replay.get('write_bytes') != expected['write_bytes']
                or replay.get('trace', {}).get('file_sha256') != expected['file_sha256']):
            raise ValueError('replay result completion/byte/hash ledger failed')
        if staged and (replay.get('phase_profile') != result['phase_profile']
                       or replay.get('kernel_barriers_enforced') is not True
                       or replay.get('window_stages') != args.prefetch_stages
                       or replay.get('stage_ledger_closed') is not True):
            raise ValueError('stage replay did not preserve the declared phase profile/barriers')
        (out/'result.json.partial').rename(out/'result.json')
        receipt.update(status='PASS', result_sha256=sha(out/'result.json'))
    except subprocess.TimeoutExpired:
        receipt.update(status='TIMEOUT', error='Replay stopped at deadline; no completed result published.')
    except (OSError, ValueError, KeyError, TypeError) as error:
        receipt.update(status='FAILED', error=type(error).__name__+': '+str(error))
    finally:
        end = time.monotonic()
        if child_began is not None:
            after = after or resource.getrusage(resource.RUSAGE_CHILDREN)
            cpu = after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime
            elapsed = (child_finished or end)-child_began
            receipt.update(CPU_seconds=cpu, CPU_minutes=cpu/60,
                           elapsed_seconds=elapsed, elapsed_minutes=elapsed/60)
        receipt.update(returncode=code, wrapper_elapsed_seconds=end-started,
            timing_scope='Child includes trace read/hash/validation, HBFSIM drain and selected stage scheduling; excludes compilation, direct generation and wrapper metadata preparation.',
            bandwidth_scope=('Fixed-cache approximate stage time and native memory service span reported separately; not calibrated inference latency.' if staged else
                             'Memory service of fixed post-cache trace with all-ready FIFO injection; not complete inference latency.'))
        (out/'run-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt, indent=2))
    return 0 if receipt['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
