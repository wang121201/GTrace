#!/usr/bin/env python3
"""Run one sealed B1 workload; measure CPU and elapsed time in seconds/minutes."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='Sealed compressed eight-frame transport')
    parser.add_argument('--output', type=Path, required=True, help='New result directory')
    parser.add_argument('--binary', type=Path, default=ROOT/'build/shared-frontend-r2/tilegen_native')
    parser.add_argument('--mode', choices=('direct', 'cosim', 'cosim-fast'), default='cosim')
    parser.add_argument('--phase-ctas', type=int, default=0, help='Direct only: export approximate compute/stage profile; 48 is one reference SM wave; default off')
    parser.add_argument('--trace', action='store_true', help='Export admitted cosim DRAM requests; direct always exports')
    parser.add_argument('--max-trace-bytes', type=int, default=8 << 30)
    parser.add_argument('--full-workflow', action='store_true')
    parser.add_argument('--timeout-seconds', type=float, default=3600)
    args = parser.parse_args()
    if args.max_trace_bytes < 4096 or args.timeout_seconds <= 0:
        parser.error('trace quota must be >=4096 bytes and timeout positive')
    if not 0 <= args.phase_ctas <= 4096 or (args.phase_ctas and args.mode != 'direct'):
        parser.error('--phase-ctas requires direct mode and a size in 1..4096')
    binary, source, out = args.binary.resolve(), args.input.resolve(), args.output.resolve()
    if not binary.is_file() or not source.is_file():
        parser.error('binary and input must exist')
    out.mkdir(parents=True, exist_ok=False)
    argv = [str(binary), '--mode='+args.mode]
    if args.phase_ctas:
        argv += ['--phase-ctas='+str(args.phase_ctas)]
    if args.trace or args.mode == 'direct':
        argv += ['--trace='+str(out/'dram.tgn'), '--max-trace-bytes='+str(args.max_trace_bytes)]
    if args.full_workflow:
        argv += ['--full-workflow']
    receipt = dict(schema='TILEGEN_TRACE_COSIM_RUN_V1', argv=argv, mode=args.mode,
                   binary_sha256=sha(binary), input_sha256=sha(source), batch_size=1,
                   CPU_only=True, GPU_sampling=False, status='RUNNING')
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    code = None
    child_finished = None
    try:
        with source.open('rb') as stdin, (out/'result.json').open('wb') as stdout, (out/'stderr.log').open('wb') as stderr:
            process = subprocess.run(argv, stdin=stdin, stdout=stdout, stderr=stderr,
                                     timeout=args.timeout_seconds, check=False)
        child_finished = time.monotonic()
        code = process.returncode
        receipt['status'] = 'PASS' if code == 0 else 'FAILED'
        if code == 0:
            result = json.loads((out/'result.json').read_text())
            if not isinstance(result, dict):
                raise ValueError('engine result must be a JSON object')
            identity = (result.get('mode') == args.mode and result.get('batch_size') == 1
                        and result.get('version') == 'tilegen-trace-cosim-20260918-r1'
                        and result.get('writeback_request_bytes') == 32)
            successful = (result.get('schema') == 'NATIVE_MEMORY_FUNCTIONAL_DIRECT_V1'
                          and result.get('status') == 'COMPLETED') if args.mode == 'direct' else (
                          result.get('schema') == 'CANONICAL_FULL_RUNTIME_COMPRESSED_RESULT_V4'
                          and result.get('status') in {
                              'MODELED_BOUNDED_SUBSEQUENCE_EXECUTED', 'MODELED_FULL_1138_CONTINUOUS_EXECUTED',
                              'MODELED_HYBRID_BOUNDED_SUBSEQUENCE_EXECUTED', 'MODELED_HYBRID_FULL_1138_CONTINUOUS_EXECUTED'})
            if not identity or not successful:
                raise ValueError('engine result identity or completion status is invalid')
            if args.phase_ctas:
                phases = result.get('phase_profile', {})
                if not isinstance(phases, dict) or phases.get('schema') != 'TILEGEN_CTA_STAGE_PROFILE_V1' or not phases.get('stages'):
                    raise ValueError('requested stage profile is missing')
                receipt.update(phase_ctas=args.phase_ctas, phase_profile_exported=True)
            if args.trace or args.mode == 'direct':
                trace = result.get('trace', {})
                path = out/'dram.tgn'
                if (not isinstance(trace, dict) or result.get('full_selected_trace_saved') is not True
                        or trace.get('status') != 'PASS_CLOSED_TRACE_READBACK'
                        or trace.get('path') != str(path) or not path.is_file()
                        or path.stat().st_size != trace.get('file_bytes')
                        or sha(path) != trace.get('file_sha256')):
                    raise ValueError('requested trace is missing, incomplete or changed')
            receipt['result_sha256'] = sha(out/'result.json')
    except subprocess.TimeoutExpired:
        receipt.update(status='TIMEOUT', error='Child terminated at requested deadline; incomplete trace is not qualified.')
    except (OSError, ValueError, TypeError) as error:
        receipt.update(status='FAILED', error=type(error).__name__+': '+str(error))
    finally:
        finished = time.monotonic()
        elapsed = (child_finished or finished)-started
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu = after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime
        receipt.update(returncode=code, elapsed_seconds=elapsed, elapsed_minutes=elapsed/60,
                       CPU_seconds=cpu, CPU_minutes=cpu/60,
                       result_verification_seconds=finished-child_finished if child_finished else 0,
                       timing_scope='Child start through process exit including stdin transport and output; excludes input hashing and compilation.')
        (out/'run-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt, indent=2))
    return 0 if receipt['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
