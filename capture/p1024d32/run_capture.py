#!/usr/bin/env python3
"""Print a capture plan by default; --execute owns only new child processes.

Discovery is unprivileged. NCU requires caller-provided profiler privilege.
The existing cross-task GPU/CPU lock files are reused, never replaced.
"""
import argparse
import csv
import ctypes
import datetime
import decimal
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import traceback

import workload

HERE = Path(__file__).resolve().parent
TASK = Path('/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi')
OLD_TASK = Path('/home/xmu/nvidiagds/codex-runs/hbserve-memgen-gtsim-alignment-20260914-01a09f50-r1')
GPU = 'GPU-18ace299-5348-e6e4-d48c-1ee5a602859b'
GPU_LOCK = Path('/home/xmu/nvidiagds/codex-runs/llm-footprint-v1/.locks') / ('gpu-' + GPU + '.lock')
PYTHON = '/home/xmu/sgl/bin/python'
NCU = '/usr/local/cuda-12.8/bin/ncu'
METRICS = ('dram__bytes_read.sum', 'dram__bytes_write.sum', 'gpu__time_duration.sum')
SCOPES = ('full', 'Prefill', 'Decode1', 'Decode8', 'Decode16', 'Decode32')


def need(ok, why):
    if not ok:
        raise RuntimeError(why)


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(8 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def verify_package():
    manifest = json.loads((HERE / 'upload-manifest.json').read_text())
    for row in manifest['files']:
        p = HERE / row['path']
        need(p.is_file() and not p.is_symlink() and p.parent == HERE, 'regular flat source required')
        need(p.stat().st_size == row['bytes'] and sha(p) == row['sha256'], 'package changed: ' + row['path'])
    actual = {p.name for p in HERE.iterdir() if p.is_file()}
    need(actual == {r['path'] for r in manifest['files']} | {'upload-manifest.json'}, 'unlisted package files')
    return dict(manifest_sha256=sha(HERE / 'upload-manifest.json'), files=manifest['files'])


def model_identity():
    rows = []
    for name, expected in workload.MODEL_SHA.items():
        p = Path(workload.MODEL) / name
        before = p.stat()
        need(sha(p) == expected, 'checkpoint content changed: ' + name)
        after = p.stat()
        need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
             (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), 'model changed while hashing')
        rows.append(dict(path=str(p), bytes=after.st_size, sha256=expected,
                         device=after.st_dev, inode=after.st_ino, mtime_ns=after.st_mtime_ns))
    return rows


def model_unchanged(rows):
    for row in rows:
        s = Path(row['path']).stat()
        need((s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns) ==
             (row['device'], row['inode'], row['bytes'], row['mtime_ns']), 'checkpoint changed during capture')


def process_rows():
    rows = {}
    for p in Path('/proc').iterdir():
        if not p.name.isdecimal():
            continue
        try:
            s = (p / 'stat').read_text().rsplit(')', 1)[1].split()
            rows[int(p.name)] = dict(ppid=int(s[1]), pgid=int(s[2]), state=s[0],
                                    startticks=int(s[19]), rss_bytes=int(s[21]) * os.sysconf('SC_PAGE_SIZE'))
        except (OSError, ValueError, IndexError):
            continue
    return rows


def gpu_apps():
    p = subprocess.run(['/usr/bin/nvidia-smi', '--query-compute-apps=pid,gpu_uuid',
                        '--format=csv,noheader,nounits'], capture_output=True, text=True, check=True, timeout=15)
    return [int(line.split(',')[0]) for line in p.stdout.splitlines() if GPU in line]


def owned_snapshot(pid, startticks, seen):
    rows = process_rows()
    # Follow descendants of a currently witnessed process and its original PGID.
    live = {p: r for p, r in rows.items() if p in seen and r['startticks'] == seen[p]['startticks']}
    if pid in rows and rows[pid]['startticks'] == startticks:
        live[pid] = rows[pid]
    while True:
        new = {p: r for p, r in rows.items() if p not in seen and
               (r['ppid'] in live or (live and r['pgid'] == pid))}
        if not new:
            break
        seen.update(new)
        live.update(new)
    seen.update(live)
    return {p: r for p, r in live.items() if r['state'] != 'Z'}, rows


def tree_bytes(root):
    return sum(p.stat().st_size for p in root.rglob('*') if p.is_file())


def child_guard(parent):
    if ctypes.CDLL(None).prctl(1, signal.SIGKILL, 0, 0, 0) != 0 or os.getppid() != parent:
        os._exit(125)


def run_step(argv, dest, env, seconds, hardware=True):
    dest.mkdir(parents=True, exist_ok=False)
    write(dest / 'command.json', dict(argv=argv, environment=env))
    start = time.monotonic()
    receipt = dict(status='RUNNING', start=now(), argv=argv, peak_owned_rss_bytes=0)
    child = None
    seen = {}
    birth = None
    try:
        with (dest / 'stdout.log').open('xb') as stdout, (dest / 'stderr.log').open('xb') as stderr:
            parent = os.getpid()
            child = subprocess.Popen(argv, env=env, cwd=TASK, stdin=subprocess.DEVNULL,
                                     stdout=stdout, stderr=stderr, start_new_session=True,
                                     preexec_fn=lambda: child_guard(parent))
            initial = process_rows().get(child.pid)
            need(initial is not None, 'child birth not observed')
            birth = initial['startticks']
            seen[child.pid] = initial
            while child.poll() is None:
                live, rows = owned_snapshot(child.pid, birth, seen)
                rss = sum(r['rss_bytes'] for r in live.values())
                receipt['peak_owned_rss_bytes'] = max(receipt['peak_owned_rss_bytes'], rss)
                need(rss <= 48 << 30, 'combined owned RSS exceeded 48 GiB')
                need(time.monotonic() - start < seconds, 'owned step deadline')
                need(stdout.tell() + stderr.tell() <= 64 << 20, 'combined logs exceeded 64 MiB')
                need(tree_bytes(dest) <= 1 << 30, 'step durable output exceeded 1 GiB')
                if hardware:
                    pids = gpu_apps()
                    _, rows = owned_snapshot(child.pid, birth, seen)
                    foreign = [p for p in pids if p not in seen or
                               (p in rows and rows[p]['startticks'] != seen[p]['startticks'])]
                    need(not foreign, 'foreign GPU owner; stop only owned processes: ' + repr(foreign))
                time.sleep(.5)
            receipt['returncode'] = child.wait()
            need(child.returncode == 0, 'child process failed')
            receipt['status'] = 'PASS_OWNED_PROCESS'
    except BaseException as e:
        receipt.update(status='FAIL', error=type(e).__name__ + ': ' + str(e))
        raise
    finally:
        # Leases remain held until every witnessed owned process is gone. Never
        # signal a PID with a different birth, or an unrelated process group.
        old_handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        try:
            if child is not None:
                if birth is None:
                    if child.poll() is None:
                        child.kill()
                    child.wait()
                else:
                    for sig in (signal.SIGTERM, signal.SIGKILL):
                        live, _ = owned_snapshot(child.pid, birth, seen)
                        for pid in live:
                            try:
                                os.kill(pid, sig)
                            except ProcessLookupError:
                                pass
                        until = time.monotonic() + 15
                        while time.monotonic() < until:
                            child.poll()
                            live, _ = owned_snapshot(child.pid, birth, seen)
                            if not live:
                                break
                            time.sleep(.1)
                    child.wait()
                    while owned_snapshot(child.pid, birth, seen)[0]:
                        receipt['status'] = 'FAIL_CLEANUP_PENDING_LEASE_HELD'
                        write(dest / 'process.json', receipt)
                        time.sleep(1)
            receipt['owned_processes_drained'] = True
            if hardware:
                until = time.monotonic() + 15
                while gpu_apps() and time.monotonic() < until:
                    time.sleep(.2)
                receipt['gpu_quiescent'] = not gpu_apps()
                if not receipt['gpu_quiescent']:
                    receipt['status'] = 'FAIL_GPU_NOT_QUIESCENT'
            receipt.update(finish=now(), elapsed_seconds=time.monotonic() - start,
                           witnessed_owned_births={str(p): r['startticks'] for p, r in seen.items()})
            write(dest / 'process.json', receipt)
        finally:
            for sig, handler in old_handlers.items():
                signal.signal(sig, handler)
    need(receipt['status'] == 'PASS_OWNED_PROCESS', 'process/quiescence failed')
    return receipt


def parse_ncu_csv(path):
    lines = Path(path).read_text().splitlines()
    begin = next(i for i, line in enumerate(lines) if line.startswith('"ID","Process ID"'))
    rows = list(csv.reader(io.StringIO('\n'.join(lines[begin:]))))
    need(len(rows) == 3 and len(set(rows[0])) == len(rows[0]), 'exactly one app-range action required')
    header, units, values = rows
    need(len(header) == len(units) == len(values), 'NCU column cardinality')
    row, unit = dict(zip(header, values)), dict(zip(header, units))
    need(row['Kernel Name'] == 'range', 'NCU action is not app-range')
    result = {}
    for name, required in zip(METRICS, ('byte', 'byte', 'ns')):
        need(unit.get(name) == required, 'NCU base unit: ' + name)
        number = decimal.Decimal(row[name].replace(',', ''))
        need(number.is_finite() and number >= 0 and (name != METRICS[2] or number > 0), 'invalid counter')
        if required == 'byte':
            need(number == number.to_integral_value(), 'noninteger bytes')
            result[name] = int(number)
        else:
            result[name] = str(number)
    return dict(metrics=result, process_id=int(row['Process ID']), action_id=row['ID'],
                bytes_and_time_same_app_range=True, hardware_accuracy_accepted=False)


def check_host(dest, c, roi):
    paths = sorted((dest / 'host').glob('process-*/finish.json'))
    need(paths, 'missing completed host receipt')
    receipts = [json.loads(p.read_text()) for p in paths]
    for r in receipts:
        need(r['status'] == 'PASS_NATIVE_WORKFLOW_AND_ROI' and r['input_contract'] == c and
             r['roi'] == roi and r['natural_phases'] == c['phases'] and r['source_unchanged'], 'host closure mismatch')
    return receipts


def previous(path, mode, c, package):
    need(path is not None, 'completed prior qualification run is required')
    p = Path(path).resolve()
    need(TASK / 'runs' in p.parents and not p.is_symlink(), 'prior receipt outside private runs')
    r = json.loads(p.read_text())
    need(r['status'] == 'PASS_CAPTURE_CLOSURE_NOT_MODEL_QUALIFICATION' and r['mode'] == mode and
         r['input_contract'] == c and r['package'] == package, 'prior source/workload closure mismatch')
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', choices=('discovery', 'ncu'), required=True)
    p.add_argument('--run-name', required=True)
    p.add_argument('--groups', type=int, choices=(1, 3), default=1)
    p.add_argument('--seconds', type=int, default=1800)
    p.add_argument('--cpu', type=int, choices=(45, 46), default=46)
    p.add_argument('--discovery-receipt', type=Path)
    p.add_argument('--pilot-receipt', type=Path)
    p.add_argument('--execute', action='store_true')
    workload.add_arguments(p)
    a = p.parse_args()
    c = workload.from_args(a)
    need(a.run_name.replace('-', '').replace('_', '').isalnum() and len(a.run_name) <= 100, 'simple unique run name')
    need(60 <= a.seconds <= 3600 and (a.mode != 'discovery' or a.groups == 1), 'bounded run options')
    scopes = [s for s in SCOPES if s == 'full' or s in c['phases']]
    cpu_lock = OLD_TASK / ('cpu45-build.lock' if a.cpu == 45 else 'cpu46.lock')
    out = TASK / 'runs' / a.run_name
    plan = dict(schema='SGLANG_PD_CAPTURE_PLAN_V1', mode=a.mode, groups=a.groups, scopes=scopes,
                run_directory=str(out), input_contract=c, GPU_UUID=GPU, cpu=a.cpu,
                shared_leases=[str(cpu_lock), str(GPU_LOCK)], execute=a.execute,
                metrics=METRICS, full_dynamic_trace_collected=False)
    if not a.execute:
        print(json.dumps(plan, indent=2))
        return 0
    need(TASK in HERE.parents and not HERE.is_symlink(), 'package must be deployed below private TASK')
    need(os.geteuid() == (1000 if a.mode == 'discovery' else 0), 'discovery runs as xmu; NCU requires caller-authorized root')
    need(a.cpu in os.sched_getaffinity(0), 'CPU unavailable')
    package = verify_package()
    if a.mode == 'ncu':
        previous(a.discovery_receipt, 'discovery', c, package)
        if a.groups == 3:
            pilot = previous(a.pilot_receipt, 'ncu', c, package)
            need(pilot['groups'] == 1 and len(pilot['samples']) == len(scopes), 'one full pilot group must close first')
    out.mkdir(parents=True, exist_ok=False)
    receipt = dict(plan, status='PREPARING', start=now(), package=package, samples=[])
    handles = []
    def interrupted(sig, frame):
        raise InterruptedError('owned controller signal ' + str(sig))
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    try:
        for path in (cpu_lock, GPU_LOCK):
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
            handles.append(fd)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        need(not gpu_apps(), 'GPU busy; no other process is stopped')
        os.sched_setaffinity(0, {a.cpu})
        need(os.statvfs(out).f_bavail * os.statvfs(out).f_frsize >= 32 << 30, 'at least 32 GiB free disk required')
        receipt['model_identity'] = model_identity()
        env = dict(PATH='/usr/local/cuda-12.8/bin:/usr/bin:/bin', CUDA_HOME='/usr/local/cuda-12.8',
                   CUDA_VISIBLE_DEVICES=GPU, CUDA_DEVICE_ORDER='PCI_BUS_ID', PYTHONDONTWRITEBYTECODE='1',
                   PYTHONNOUSERSITE='1', PYTHONUNBUFFERED='1', OMP_NUM_THREADS='1', MKL_NUM_THREADS='1',
                   OPENBLAS_NUM_THREADS='1', NUMEXPR_NUM_THREADS='1', TOKENIZERS_PARALLELISM='false',
                   HF_HUB_OFFLINE='1', TRANSFORMERS_OFFLINE='1', LC_ALL='C.UTF-8', MAX_JOBS='1',
                   NV_COMPUTE_PROFILER_DISABLE_STOCK_FILE_DEPLOYMENT='1')
        for key, folder in [('TRITON_CACHE_DIR', 'triton'), ('CUDA_CACHE_PATH', 'cuda'),
                            ('XDG_CACHE_HOME', 'xdg'), ('FLASHINFER_WORKSPACE_BASE', 'flashinfer'), ('TMPDIR', 'tmp')]:
            d = TASK / 'cache' / folder
            d.mkdir(parents=True, exist_ok=True)
            env[key] = str(d)
        shape = ['--prefill-length', str(c['prefill_length']), '--decode-steps', str(c['decode_steps']),
                 '--max-total-tokens', str(c['max_total_tokens'])]
        receipt['status'] = 'RUNNING'
        write(out / 'controller.json', receipt)
        if a.mode == 'discovery':
            dest = out / 'validate'
            run_step([PYTHON, '-B', str(HERE / 'native_ncu_driver.py'), '--mode', 'validate',
                      '--roi', 'full', '--output', str(dest / 'host'), *shape], dest, env, a.seconds)
            check_host(dest, c, 'full')
            dest = out / 'metadata'
            run_step([PYTHON, '-B', str(HERE / 'sglang_driver.py'), '--output', str(dest / 'artifacts'),
                      '--profile', *shape], dest, env, a.seconds)
            m = json.loads((dest / 'artifacts/manifest.json').read_text())
            need(m['status'] == 'COMPLETE' and m['input_contract'] == c and m['native_source_unchanged'] and
                 m['stage_files'] == [s + '.json' for s in c['phases']], 'all metadata phases must close')
            launch_meta = json.loads((dest / 'artifacts/kernel_launches.json').read_text())
            phase_markers = [e['name'][6:] for e in launch_meta['events'] if e.get('cat') == 'user_annotation'
                             and e.get('ph') == 'X' and e.get('name', '').startswith('phase/')]
            need(launch_meta['kernel_count'] > 0 and sorted(phase_markers) == sorted(c['phases']),
                 'nonempty kernel metadata and exactly one marker for every frozen phase required')
            receipt['metadata_census'] = dict(recorded_kernels=launch_meta['kernel_count'],
                phase_markers=phase_markers, per_kernel_phase_join_completed=False,
                exhaustive_CUDA_API_census_established=False, decoded_SASS_identity_established=False)
            receipt['metadata_manifest_sha256'] = sha(dest / 'artifacts/manifest.json')
        else:
            for group in range(1, a.groups + 1):
                for roi in scopes:
                    need(not gpu_apps(), 'GPU busy between scopes')
                    dest = out / ('group-%d-%s' % (group, roi))
                    target = [PYTHON, '-B', str(HERE / 'native_ncu_driver.py'), '--mode', 'capture',
                              '--roi', roi, '--output', str(dest / 'host'), *shape]
                    argv = [NCU, '--config-file', 'off', '--rename-kernels', 'off', '--disable-extra-suffixes',
                            '--target-processes', 'application-only', '--replay-mode', 'app-range',
                            '--cache-control', 'none', '--clock-control', 'none', '--metrics', ','.join(METRICS),
                            '--export', str(dest / 'capture'), *target]
                    run_step(argv, dest, env, a.seconds)
                    hosts = check_host(dest, c, roi)
                    report = dest / 'capture.ncu-rep'
                    need(report.is_file(), 'NCU report missing')
                    imp = dest / 'import'
                    run_step([NCU, '--import', str(report), '--page', 'raw', '--csv', '--print-units', 'base'],
                             imp, env, 120, hardware=False)
                    counters = parse_ncu_csv(imp / 'stdout.log')
                    need(counters['process_id'] in {r['pid'] for r in hosts}, 'NCU action PID lacks host closure')
                    receipt['samples'].append(dict(group=group, roi=roi, counters=counters,
                        report_sha256=sha(report), csv_sha256=sha(imp / 'stdout.log'), host_replays=len(hosts)))
                    write(out / 'controller.json', receipt)
                    need(tree_bytes(out) <= 8 << 30, 'run durable output exceeded 8 GiB')
                    model_unchanged(receipt['model_identity'])
                    need(verify_package() == package, 'package changed during capture')
        model_unchanged(receipt['model_identity'])
        need(verify_package() == package and not gpu_apps(), 'final source/GPU closure')
        receipt['status'] = 'PASS_CAPTURE_CLOSURE_NOT_MODEL_QUALIFICATION'
    except BaseException as e:
        receipt.update(status='FAIL', error=type(e).__name__ + ': ' + str(e), traceback=traceback.format_exc())
    finally:
        receipt['finish'] = now()
        write(out / 'controller.json', receipt)
        for fd in reversed(handles):
            os.close(fd)
    print(json.dumps(dict(status=receipt['status'], receipt=str(out / 'controller.json'))))
    return 0 if receipt['status'] == 'PASS_CAPTURE_CLOSURE_NOT_MODEL_QUALIFICATION' else 1


if __name__ == '__main__':
    raise SystemExit(main())
