#!/usr/bin/env python3
"""Observer r2: the same native workload/validator, with a 1 GiB metadata cap."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import traceback

from support import HERE, TASK, R1_SHA, CAPTURE_SHA, identity, load_r1, need, sha


def build_identity():
    directory = HERE / 'build'
    receipt = json.loads((directory / 'build.json').read_text())
    need(receipt['status'] == 'PASS_BUILD_ONLY_NO_GPU' and not receipt['GPU_run'] and
         receipt['inputs'] == receipt['inputs_after'], 'closed GPU-free build required')
    source_manifest = json.loads((HERE / 'manifest.json').read_text())
    for name, row in source_manifest['build_inputs'].items():
        matches = [p for p in receipt['inputs'] if p['path'] == str(HERE / name)]
        need(len(matches) == 1 and all(matches[0][k] == row[k] for k in ('bytes', 'sha256')), 'build source differs')
    for row in receipt['inputs']:
        p = Path(row['path'])
        need(p.is_file() and not p.is_symlink() and p.stat().st_size == row['bytes'] and sha(p) == row['sha256'], 'build input changed')
    binary = receipt['binary']
    p = directory / 'observer.so'
    need(binary['path'] == str(p) and p.is_file() and not p.is_symlink() and
         p.stat().st_size == binary['bytes'] and sha(p) == binary['sha256'], 'new observer binary identity')
    return dict(receipt_sha256=sha(directory / 'build.json'), binary=binary)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run-name', default='observer-r2')
    p.add_argument('--seconds', type=int, default=1800)
    p.add_argument('--cpu', type=int, choices=(45, 46), default=46)
    p.add_argument('--discovery-receipt', type=Path)
    p.add_argument('--execute', action='store_true')
    a = p.parse_args()
    need(a.run_name.replace('-', '').replace('_', '').isalnum() and len(a.run_name) <= 100, 'unique simple run name')
    need(60 <= a.seconds <= 3600, 'bounded owned deadline')
    plan = dict(schema='SGLANG_PD_NATIVE_OBSERVER_R2_PLAN_V1', output=str(TASK / 'runs' / a.run_name),
                observer=str(HERE / 'build/observer.so'), metadata_cap_bytes=1 << 30,
                workload_manifest_sha256=CAPTURE_SHA, validator_manifest_sha256=R1_SHA,
                execute=a.execute, torch_profiler=False, NCU=False)
    if not a.execute:
        print(json.dumps(plan, indent=2))
        return 0
    need(HERE == TASK / 'observer-r2' and os.geteuid() == 1000, 'private unprivileged deployment')
    need(a.cpu in os.sched_getaffinity(0), 'selected CPU available')
    own = identity(HERE)
    r1 = load_r1()
    base, workload = r1.base, r1.workload
    package, validator = base.verify_package(), r1.wrapper_identity()
    c = workload.contract()
    base.previous(a.discovery_receipt, 'discovery', c, package)
    build = build_identity()
    out = TASK / 'runs' / a.run_name
    out.mkdir(parents=True, exist_ok=False)
    (out / 'observer').mkdir()
    receipt = dict(plan, status='PREPARING', start=base.now(), package=own, workload_package=package,
                   validator=validator, build=build)
    fds = []
    def stop(sig, frame):
        raise InterruptedError('observer r2 signal ' + str(sig))
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)
    try:
        cpu_lock = base.OLD_TASK / ('cpu45-build.lock' if a.cpu == 45 else 'cpu46.lock')
        for path in (cpu_lock, base.GPU_LOCK):
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
            fds.append(fd)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        need(not base.gpu_apps(), 'GPU busy; no other process stopped')
        os.sched_setaffinity(0, {a.cpu})
        need(os.statvfs(out).f_bavail * os.statvfs(out).f_frsize >= 32 << 30, '32 GiB disk admission')
        receipt['model_identity'] = base.model_identity()
        env = dict(PATH='/usr/local/cuda-12.8/bin:/usr/bin:/bin', CUDA_HOME='/usr/local/cuda-12.8',
                   CUDA_VISIBLE_DEVICES=base.GPU, CUDA_DEVICE_ORDER='PCI_BUS_ID', PYTHONDONTWRITEBYTECODE='1',
                   PYTHONNOUSERSITE='1', PYTHONUNBUFFERED='1', OMP_NUM_THREADS='1', MKL_NUM_THREADS='1',
                   OPENBLAS_NUM_THREADS='1', NUMEXPR_NUM_THREADS='1', TOKENIZERS_PARALLELISM='false',
                   HF_HUB_OFFLINE='1', TRANSFORMERS_OFFLINE='1', LC_ALL='C.UTF-8', MAX_JOBS='1',
                   LD_PRELOAD=build['binary']['path'], SG_NVBIT_SCOPE_ABI='1',
                   SG_NVBIT_OUTPUT_ROOT=str(out / 'observer'), SG_NVBIT_MAX_BYTES=str(1 << 30), ACK_CTX_INIT_LIMITATION='1')
        for key, name in [('TRITON_CACHE_DIR', 'triton'), ('CUDA_CACHE_PATH', 'cuda'), ('XDG_CACHE_HOME', 'xdg'),
                          ('FLASHINFER_WORKSPACE_BASE', 'flashinfer'), ('TMPDIR', 'tmp')]:
            d = TASK / 'cache' / name
            need(d.is_dir(), 'discovery cache missing')
            env[key] = str(d)
        receipt['status'] = 'RUNNING'
        base.write(out / 'controller.json', receipt)
        step = out / 'native'
        argv = [base.PYTHON, '-B', str(r1.PACKAGE / 'sglang_driver.py'), '--output', str(step / 'artifacts'),
                '--prefill-length', '1024', '--decode-steps', '32', '--max-total-tokens', '1280']
        base.run_step(argv, step, env, a.seconds)
        census = r1.validate_native(step, c, observer_parent=out)
        finish = r1.read(next((out / 'observer').glob('process-*/finish.json')))
        need(finish['max_total_bytes'] == 1 << 30, 'exact new observer cap')
        base.write(out / 'native-census.json', census)
        base.model_unchanged(receipt['model_identity'])
        need(identity(HERE) == own and base.verify_package() == package and r1.wrapper_identity() == validator and
             build_identity() == build, 'all source/build identities unchanged')
        need(not base.gpu_apps(), 'GPU quiescence')
        receipt.update(status='PASS_NATIVE_METADATA_CENSUS_ONLY', native_census_sha256=sha(out / 'native-census.json'),
                       phase_counts=census['phase_counts'], measured_launches=census['measured_launches'])
    except BaseException as e:
        receipt.update(status='FAIL', error=type(e).__name__ + ': ' + str(e), traceback=traceback.format_exc())
    finally:
        receipt['finish'] = base.now()
        base.write(out / 'controller.json', receipt)
        for fd in reversed(fds):
            os.close(fd)
    print(json.dumps(dict(status=receipt['status'], receipt=str(out / 'controller.json'))))
    return 0 if receipt['status'] == 'PASS_NATIVE_METADATA_CENSUS_ONLY' else 1


if __name__ == '__main__':
    raise SystemExit(main())
