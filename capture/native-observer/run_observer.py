#!/usr/bin/env python3
"""Separate unprivileged native metadata run; default prints a plan only."""
import argparse
import collections
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import sys
import traceback

HERE = Path(__file__).resolve().parent
PACKAGE = next((p for p in (HERE.parent / 'capture', HERE.parent / 'p1024d32')
                if (p / 'run_capture.py').is_file()), None)
if PACKAGE is None:
    raise RuntimeError('frozen capture package missing beside observer wrapper')
sys.path.insert(0, str(PACKAGE))
import run_capture as base
import workload

OBSERVER = base.OLD_TASK / 'sglang-integration-r6/nvbit-build-r3/observer.so'
OBSERVER_SHA = '1248ba7c81c4bd35acb28a972fa1edd16d1993d3b2eb2c6d5d70fcfdbb3666c7'
PACKAGE_MANIFEST_SHA = 'fe559d9bca3a03446e801a37c0a544a09694a22cf9b028909c0af3df478d4884'
JOURNALS = {'launch-journal.jsonl', 'scope-journal.jsonl', 'functions.jsonl',
            'static-instructions.jsonl', 'lifecycle.jsonl', 'allocation-journal.jsonl'}
need = base.need


def read(path):
    return json.loads(Path(path).read_text())


def lines(path):
    with Path(path).open('rb') as f:
        for raw in f:
            need(raw.endswith(b'\n') and len(raw) <= 1 << 20, 'bounded newline-closed journal rows')
            yield json.loads(raw)


def wrapper_identity():
    manifest = read(HERE / 'upload-manifest.json')
    expected = {r['path'] for r in manifest['files']}
    need({p.name for p in HERE.iterdir() if p.is_file()} == expected | {'upload-manifest.json'}, 'unlisted wrapper files')
    for row in manifest['files']:
        p = HERE / row['path']
        need(p.parent == HERE and p.is_file() and not p.is_symlink(), 'regular flat wrapper file')
        need(p.stat().st_size == row['bytes'] and base.sha(p) == row['sha256'], 'wrapper source changed')
    return dict(manifest_sha256=base.sha(HERE / 'upload-manifest.json'), files=manifest['files'])


def validate_native(run, c, observer_parent=None):
    """Validate exact journals and derive new ordinals, with no old shape census."""
    run = Path(run)
    manifest = read(run / 'artifacts/manifest.json')
    need(manifest['status'] == 'COMPLETE' and manifest['input_contract'] == c and
         manifest['native_source_unchanged'] and manifest['coverage']['native_scope_abi_enabled'] and
         not manifest['coverage']['kernel_launch_metadata'], 'native host/observer source closure')
    workload.check_native_sources(manifest['native_source_files'])
    need(manifest['stage_files'] == [s + '.json' for s in c['phases']], 'complete native stages')
    process = manifest['process']
    roots = list(((Path(observer_parent) if observer_parent is not None else run) / 'observer').glob('process-*'))
    need(len(roots) == 1 and roots[0].name == 'process-%d-%d' % (process['pid'], process['start_ticks']), 'single matching observer process')
    root = roots[0]
    finish = read(root / 'finish.json')
    need(finish['status'] == 'PASS_METADATA_OBSERVER_CLOSED_NOT_TRACE' and
         finish['pid'] == process['pid'] and finish['start_ticks'] == process['start_ticks'], 'observer finish/process closure')
    need({r['name'] for r in finish['files']} == JOURNALS and len(finish['files']) == 6, 'exact six journals')
    for row in finish['files']:
        p = root / row['name']
        need(not p.is_symlink() and p.stat().st_size == row['bytes'] and base.sha(p) == row['sha256'], 'journal SHA/length: ' + row['name'])
    for field in ('launch_error_count', 'unsupported_dispatch_count', 'graph_node_callback_count',
                  'unknown_launch_attribute_count', 'open_context_count', 'active_epoch', 'internal_inspection_dispatch_count'):
        need(finish[field] == 0, 'observer error/capability: ' + field)
    need(not finish['errors'] and finish['epoch_begin_count'] == finish['epoch_end_count'] == len(c['phases']), 'epoch total/error closure')
    need(finish['launch_before_count'] == finish['launch_return_count'], 'finish launch closure')
    functions = {}
    for row in lines(root / 'functions.jsonl'):
        fid = row['function_id']
        need(fid not in functions, 'duplicate function ID')
        functions[fid] = row
    need(len(functions) == finish['function_count'] > 0, 'actual function census')
    static_hashes, static_counts = {}, collections.Counter()
    with (root / 'static-instructions.jsonl').open('rb') as f:
        for raw in f:
            need(raw.endswith(b'\n') and len(raw) <= 1 << 20, 'static row boundary')
            row = json.loads(raw)
            fid = row['function_id']
            need(fid in functions and row['schema'] == 'sg_nvbit_static_instruction_v1', 'static function identity')
            # Hash original bytes, preserving the observer's exact field order
            # and its byte-wise JSON escapes; no decode/re-encode substitution.
            canonical = raw.rstrip(b'\n').split(b'"instruction":', 1)[1][:-1] + b'\n'
            static_hashes.setdefault(fid, hashlib.sha256()).update(canonical)
            static_counts[fid] += 1
    for fid, f in functions.items():
        need(static_counts[fid] == f['instruction_count'] and fid in static_hashes and
             static_hashes[fid].hexdigest() == f['code_sha256'], 'static instruction census/SHA')
    epochs = {}
    for row in lines(root / 'scope-journal.jsonl'):
        need(row['pid'] == process['pid'] and row['start_ticks'] == process['start_ticks'], 'scope process identity')
        if row['type'] in ('epoch_begin', 'epoch_end'):
            e = epochs.setdefault(row['epoch_id'], {})
            need(row['type'] not in e, 'duplicate epoch boundary')
            e[row['type']] = row['event_ordinal']
    need(set(epochs) == set(range(1, len(c['phases']) + 1)), 'exact contract epoch IDs')
    previous_end = -1
    for epoch in range(1, len(c['phases']) + 1):
        e = epochs[epoch]
        need(set(e) == {'epoch_begin', 'epoch_end'} and previous_end < e['epoch_begin'] < e['epoch_end'], 'ordered nonoverlapping complete epochs')
        previous_end = e['epoch_end']
    before, after = {}, {}
    for row in lines(root / 'launch-journal.jsonl'):
        need(row['pid'] == process['pid'] and row['start_ticks'] == process['start_ticks'], 'launch process identity')
        need(row['edge'] in ('before', 'return'), 'launch edge')
        dest = before if row['edge'] == 'before' else after
        need(row['launch_id'] not in dest, 'duplicate launch edge')
        dest[row['launch_id']] = row
    need(set(before) == set(after) == set(range(finish['launch_before_count'])), 'complete launch ordinal/pair census')
    ignored = {'edge', 'event_ordinal', 'monotonic_ns', 'cuda_status'}
    modules = {m['call_id']: m for m in read(run / 'artifacts/module_calls.json')}
    phase_counts = collections.Counter()
    calls = []
    previous_event = -1
    for lid in sorted(before):
        b, a = before[lid], after[lid]
        need(previous_event < b['event_ordinal'] < a['event_ordinal'] and a['cuda_status'] == 0, 'launch order/return status')
        previous_event = b['event_ordinal']
        need({k: v for k, v in b.items() if k not in ignored} == {k: v for k, v in a.items() if k not in ignored}, 'launch before/return identity')
        epoch = b['epoch_id']
        if not epoch:
            continue
        need(epoch in epochs, 'launch outside known epochs')
        e, phase = epochs[epoch], c['phases'][epoch - 1]
        need(e['epoch_begin'] < b['event_ordinal'] < a['event_ordinal'] < e['epoch_end'], 'launch epoch enclosure')
        need(b['phase'] == phase and b['forward_id'] == epoch - 1 and b['role'] == 'measurement' and b['scope_bound'], 'native phase/scope attribution')
        need(b['metadata_supported'] and b['function_id'] in functions, 'native function unsupported')
        f = functions[b['function_id']]
        need(b['code_sha256'] == f['code_sha256'] and b['argument_sizes'] == f['argument_sizes'], 'launch/static function ABI identity')
        need(b['parameter_layout_sha256'] == hashlib.sha256(json.dumps(b['argument_sizes'], separators=(',', ':')).encode()).hexdigest(), 'ABI size layout SHA')
        if b['module_scope'] == '<phase-global>':
            need(b['call_id'] == 10000000 + epoch - 1 and b['layer_id'] == -1, 'phase-global owner')
        else:
            module = modules.get(b['call_id'])
            need(module and module['module'] == b['module_scope'] and module['phase'] == phase, 'same-process module call owner')
            owner, layer, visited = module, -1, set()
            while owner is not None:
                need(owner['call_id'] not in visited, 'cyclic module ancestry')
                visited.add(owner['call_id'])
                if owner['module_class'].endswith('.LlamaDecoderLayer'):
                    layer = int(owner['module'].split('.layers.')[1].split('.')[0])
                    break
                owner = modules.get(owner.get('parent_call_id'))
            need(b['layer_id'] == layer, 'actual decoder ancestry')
        need(all(type(n) is int and n > 0 for k in ('grid', 'block') for n in b[k]) and
             len(b['grid']) == len(b['block']) == 3, 'actual launch geometry')
        ordinal = phase_counts[phase]
        phase_counts[phase] += 1
        fields = ('cuda_api', 'function_id', 'function_name', 'code_sha256', 'code_sha256_kind',
                  'context_id', 'stream_u64', 'module_scope', 'call_id', 'layer_id', 'grid', 'block',
                  'static_shared_bytes', 'dynamic_shared_bytes', 'registers', 'local_bytes_per_thread',
                  'launch_attributes', 'argument_sizes', 'parameter_layout_sha256')
        calls.append(dict(source_launch_key='epoch-%d-launch-%d' % (epoch, ordinal), epoch_id=epoch,
                          epoch_launch_ordinal=ordinal, native_launch_id=lid, phase=phase,
                          **{k: b[k] for k in fields}))
    need(set(phase_counts) == set(c['phases']) and all(phase_counts[p] > 0 for p in c['phases']), 'all phases have observed native launches')
    return dict(schema='SGLANG_PD_NATIVE_METADATA_CENSUS_V1', status='PASS_NATIVE_METADATA_CENSUS_ONLY',
                process=process, input_contract_sha256=c['sha256'], phase_counts=dict(phase_counts),
                measured_launches=len(calls), total_launches=len(before), inspected_functions=len(functions),
                unique_decoded_code_hashes=len({f['code_sha256'] for f in functions.values()}), calls=calls,
                observer_finish_sha256=base.sha(root / 'finish.json'), journals=finish['files'],
                qualification=dict(decoded_static_SASS_hashes_verified=True, argument_size_layout_only=True,
                    raw_argument_values_captured=False, typed_pointer_binding=False, dynamic_memory_addresses=False,
                    dynamic_program_execution=False, cubin_hash=False, complete_allocator_coverage=False,
                    native_model_admitted=False, exhaustive_driver_callback_coverage_proven=False,
                    same_process_CUPTI_crosscheck=False))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run-name', default='observer-r1')
    p.add_argument('--seconds', type=int, default=1800)
    p.add_argument('--cpu', type=int, choices=(45, 46), default=46)
    p.add_argument('--discovery-receipt', type=Path)
    p.add_argument('--execute', action='store_true')
    a = p.parse_args()
    c = workload.contract()
    need(a.run_name.replace('-', '').replace('_', '').isalnum() and len(a.run_name) <= 100, 'unique simple run name')
    need(60 <= a.seconds <= 3600, 'bounded wall deadline')
    out = base.TASK / 'runs' / a.run_name
    plan = dict(schema='SGLANG_PD_NATIVE_OBSERVER_PLAN_V1', output=str(out), observer=str(OBSERVER),
                observer_sha256=OBSERVER_SHA, frozen_package_manifest_sha256=PACKAGE_MANIFEST_SHA,
                input_contract_sha256=c['sha256'], execute=a.execute, torch_profiler=False, NCU=False)
    if not a.execute:
        print(json.dumps(plan, indent=2))
        return 0
    need(HERE == base.TASK / 'observer' and PACKAGE == base.TASK / 'capture', 'isolated remote deployment required')
    need(os.geteuid() == 1000 and a.cpu in os.sched_getaffinity(0), 'unprivileged available CPU required')
    package = base.verify_package()
    need(package['manifest_sha256'] == PACKAGE_MANIFEST_SHA, 'exact frozen capture package')
    own = wrapper_identity()
    base.previous(a.discovery_receipt, 'discovery', c, package)
    need(OBSERVER.is_file() and not OBSERVER.is_symlink() and base.sha(OBSERVER) == OBSERVER_SHA, 'historical observer binary SHA')
    out.mkdir(parents=True, exist_ok=False)
    (out / 'observer').mkdir()
    receipt = dict(plan, status='PREPARING', start=base.now(), package=package, wrapper=own)
    fds = []
    def interrupted(sig, frame):
        raise InterruptedError('observer controller signal ' + str(sig))
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    try:
        cpu_lock = base.OLD_TASK / ('cpu45-build.lock' if a.cpu == 45 else 'cpu46.lock')
        for path in (cpu_lock, base.GPU_LOCK):
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
            fds.append(fd)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        need(not base.gpu_apps(), 'GPU busy; no other process is touched')
        os.sched_setaffinity(0, {a.cpu})
        need(os.statvfs(out).f_bavail * os.statvfs(out).f_frsize >= 32 << 30, '32 GiB free disk admission')
        receipt['model_identity'] = base.model_identity()
        env = dict(PATH='/usr/local/cuda-12.8/bin:/usr/bin:/bin', CUDA_HOME='/usr/local/cuda-12.8',
                   CUDA_VISIBLE_DEVICES=base.GPU, CUDA_DEVICE_ORDER='PCI_BUS_ID', PYTHONDONTWRITEBYTECODE='1',
                   PYTHONNOUSERSITE='1', PYTHONUNBUFFERED='1', OMP_NUM_THREADS='1', MKL_NUM_THREADS='1',
                   OPENBLAS_NUM_THREADS='1', NUMEXPR_NUM_THREADS='1', TOKENIZERS_PARALLELISM='false',
                   HF_HUB_OFFLINE='1', TRANSFORMERS_OFFLINE='1', LC_ALL='C.UTF-8', MAX_JOBS='1',
                   LD_PRELOAD=str(OBSERVER), SG_NVBIT_SCOPE_ABI='1', SG_NVBIT_OUTPUT_ROOT=str(out / 'observer'),
                   SG_NVBIT_MAX_BYTES=str(256 << 20), ACK_CTX_INIT_LIMITATION='1')
        for key, name in [('TRITON_CACHE_DIR', 'triton'), ('CUDA_CACHE_PATH', 'cuda'), ('XDG_CACHE_HOME', 'xdg'),
                          ('FLASHINFER_WORKSPACE_BASE', 'flashinfer'), ('TMPDIR', 'tmp')]:
            d = base.TASK / 'cache' / name
            need(d.is_dir(), 'discovery cache missing')
            env[key] = str(d)
        receipt['status'] = 'RUNNING'
        base.write(out / 'controller.json', receipt)
        step = out / 'native'
        argv = [base.PYTHON, '-B', str(PACKAGE / 'sglang_driver.py'), '--output', str(step / 'artifacts'),
                '--prefill-length', '1024', '--decode-steps', '32', '--max-total-tokens', '1280']
        # run_step caps native/ at 1 GiB. The independent observer root has its
        # own hard 256 MiB writer cap and must exist before the child starts.
        base.run_step(argv, step, env, a.seconds)
        census = validate_native(step, c, observer_parent=out)
        base.write(out / 'native-census.json', census)
        base.model_unchanged(receipt['model_identity'])
        need(base.verify_package() == package and wrapper_identity() == own and base.sha(OBSERVER) == OBSERVER_SHA,
             'all source/binary identities unchanged')
        need(not base.gpu_apps(), 'GPU not quiescent')
        receipt.update(status='PASS_NATIVE_METADATA_CENSUS_ONLY', native_census_sha256=base.sha(out / 'native-census.json'),
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
