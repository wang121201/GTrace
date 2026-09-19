#!/usr/bin/env python3
"""Build only on the existing CPU45 lease. Never queries or uses a GPU."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from support import HERE, TASK, identity, need, sha

LOCK = Path('/home/xmu/nvidiagds/codex-runs/hbserve-memgen-gtsim-alignment-20260914-01a09f50-r1/cpu45-build.lock')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--execute', action='store_true')
    a = p.parse_args()
    output = HERE / 'build'
    argv = ['/usr/bin/python3', '-B', str(HERE / 'build.py'), '--output', str(output)]
    plan = dict(schema='NATIVE_ARGUMENT_CPU45_BUILD_PLAN_V1', argv=argv, cpu=45, lease=str(LOCK),
                GPU_used=False, GPU_queried=False, CUDA_VISIBLE_DEVICES='', execute=a.execute)
    if not a.execute:
        print(json.dumps(plan, indent=2))
        return 0
    need(HERE == TASK / 'native-arguments-r1' and os.geteuid() == 1000, 'private unprivileged deployment')
    need(45 in os.sched_getaffinity(0) and not output.exists(), 'CPU45 available and fresh build directory')
    before = identity(HERE)
    logs = TASK / 'runs' / 'native-arguments-r1-build'
    logs.mkdir(parents=True, exist_ok=False)
    fd = os.open(LOCK, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    child = None
    result = dict(plan, status='PREPARING', package=before)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.sched_setaffinity(0, {45})
        def stop(sig, frame):
            raise InterruptedError('CPU build controller signal ' + str(sig))
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, stop)
        env = dict(PATH='/usr/local/cuda-12.8/bin:/usr/bin:/bin', CUDA_VISIBLE_DEVICES='',
                   OMP_NUM_THREADS='1', PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1', LC_ALL='C.UTF-8')
        with (logs / 'stdout.log').open('xb') as stdout, (logs / 'stderr.log').open('xb') as stderr:
            child = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
            result['returncode'] = child.wait(timeout=1500)
        need(child.returncode == 0, 'NVCC build failed')
        receipt = json.loads((output / 'build.json').read_text())
        need(receipt['status'] == 'PASS_BUILD_ONLY_NO_GPU' and receipt['inputs'] == receipt['inputs_after'], 'build identity closure')
        need(identity(HERE) == before, 'package changed during build')
        result.update(status='PASS_CPU45_BUILD_ONLY', build_receipt_sha256=sha(output / 'build.json'), binary=receipt['binary'])
    except BaseException as e:
        result.update(status='FAIL', error=type(e).__name__ + ': ' + str(e))
    finally:
        if child is not None and child.poll() is None:
            # The original build.py owns/reaps every compiler subprocess group
            # on TERM before this controller releases CPU45's lease.
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                signal.signal(sig, signal.SIG_IGN)
            child.terminate()
            child.wait()
        (logs / 'controller.json').write_text(json.dumps(result, indent=2) + '\n')
        os.close(fd)
    print(json.dumps(dict(status=result['status'], receipt=str(logs / 'controller.json'), binary=result.get('binary'))))
    return 0 if result['status'] == 'PASS_CPU45_BUILD_ONLY' else 1


if __name__ == '__main__':
    raise SystemExit(main())
