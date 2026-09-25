"""Owned bounded native co-simulation; accepts only this experiment's frozen plans."""
from pathlib import Path
import argparse
import importlib.util
import json
import sys
from prepare import D, R, H, pin, write_new

sys.dont_write_bytecode = True


def need(ok, message):
    if not ok:
        raise RuntimeError(message)


def main():
    args = argparse.ArgumentParser()
    args.add_argument('--build-receipt', required=True)
    args.add_argument('--objects', required=True, help='JSON list of absolute .o paths')
    args.add_argument('--out', required=True)
    args.add_argument('--plans', nargs='+', required=True)
    args.add_argument('--seconds', type=int, default=420)
    a = args.parse_args()
    need(60 <= a.seconds <= 900, 'bounded diagnostic only')
    out = D / a.out
    need(out.parent == D and not out.exists(), 'fresh direct experiment subdirectory')
    out.mkdir()
    owned = R / 'work/tilegen-hbf-traceoff-full-r1/owned_group.py'
    spec = importlib.util.spec_from_file_location('_owned_ada_alignment', owned)
    G = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(G)
    started = G.raw()
    deadline = started + a.seconds
    receipt = dict(status='RUNNING', schema='ADA_ALIGNMENT_NATIVE_WINDOWS_V1',
        steps=[], full_inference=False, full_trace_saved=False, GPU=False,
        max_seconds=a.seconds, host_setup_seconds=None)

    def save():
        receipt['elapsed_seconds'] = G.raw() - started
        temporary = out / 'receipt.tmp'
        temporary.write_text(json.dumps(receipt, indent=2) + '\n')
        temporary.replace(out / 'receipt.json')

    def invoke(name, argv, limit):
        t = G.raw()
        need(deadline - t > 8, 'owned total budget')
        shim = ('import json,os,sys;p=json.loads(sys.argv[1]);'
                'a=os.open(p["stdout"],os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);'
                'b=os.open(p["stderr"],os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);'
                'os.dup2(a,1);os.dup2(b,2);os.close(a);os.close(b);'
                'os.execvp(p["argv"][0],p["argv"])')
        payload = dict(argv=argv, stdout=str(out / (name + '.stdout')),
                       stderr=str(out / (name + '.stderr')))
        group = None
        cleanup = None
        reason = None
        with G.SignalLatch() as signals:
            try:
                group = G.OwnedGroup([sys.executable, '-B', '-c', shim, json.dumps(payload)], origin_raw=t)
                receipt['active_step'] = dict(name=name, pid=group.child.pid, pgid=group.pgid)
                save()
                reason = group.wait_reason(min(deadline - 8, t + limit), signals)
            finally:
                if group is not None:
                    cleanup = group.cleanup(term_grace=3, kill_grace=3)
        step = dict(name=name, argv=argv, seconds=G.raw() - t, reason=reason,
                    returncode=group.child.returncode if group else None,
                    cleanup=cleanup, signals=signals.receipt())
        for stream in ('stdout', 'stderr'):
            p = out / (name + '.' + stream)
            if p.exists():
                step[stream] = pin(p)
        receipt['steps'].append(step)
        receipt.pop('active_step', None)
        save()
        need(step['returncode'] == 0 and reason == 'leader_exited' and
             cleanup['cleanup_complete'] and cleanup['group_absent_after_cleanup'] and
             cleanup['direct_child_reaped'] and not cleanup['errors'] and
             signals.first_signal is None, 'owned step failed: ' + name)

    try:
        bp = Path(a.build_receipt).resolve()
        build = json.loads(bp.read_text())
        need(build['status'].startswith('PASS'), 'structure build/test must first close')
        objects = [Path(x).resolve() for x in json.loads(Path(a.objects).read_text())]
        need(len(objects) == 21 and all(p.is_file() for p in objects), 'all 21 qualified link objects')
        need([pin(p) for p in objects] == [x['object'] for x in build['objects']],
             'objects exactly match closed structure build')
        for item in build['pins']:
            need(pin(item['path']) == item, 'structure source pin changed')
        snapshot = R / 'work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot'
        original_build = json.loads((snapshot / 'build/shared-h288-r1/build-receipt.json').read_text())
        overlays = [D / 'structure-adapter/overlay.json',
                    H.parent / 'current-history-write-attribution-r1/overlay.json']
        plans = [D / 'plans-r1' / x for x in a.plans]
        allowed = {Path(x['plan']['path']): x['plan'] for x in json.loads((D / 'plans-r1/manifest.json').read_text())['windows']}
        for path in plans:
            need(path in allowed and pin(path) == allowed[path], 'exact prepared diagnostic plan')
            p = json.loads(path.read_text())
            need(not p['full_history'] and not p['complete_prefix_from_process_start'], 'diagnostic scope explicit')
        sources = [bp, Path(a.objects), Path(__file__), owned, D / 'runtime-r2/history_runtime.cpp',
                   D / 'runtime-r2/derivation.json', *overlays, *objects, *plans,
                   D / 'structure-adapter/config.h', D / 'structure-adapter/cache_geometry.h',
                   D / 'structure-adapter/ada_address_mapping.h']
        before = [pin(p) for p in sources]
        before += build['pins']
        receipt['pins'] = before
        receipt['host_setup_seconds'] = G.raw() - started
        save()
        flags = original_build['flags'][:]
        for overlay in overlays:
            flags += ['-ivfsoverlay', str(overlay)]
        binary = out / 'fixture'
        invoke('compile', ['clang++', *flags, '-Wno-unused-parameter',
                          str(D / 'runtime-r2/history_runtime.cpp'), *map(str, objects),
                          '-lz', '-o', str(binary)], 150)
        receipt['binary'] = pin(binary)
        receipt['cases'] = []
        for plan in plans:
            result_file = out / (plan.stem + '.json')
            invoke('run-' + plan.stem, [str(binary), str(plan), str(result_file)], 150)
            result = json.loads(result_file.read_text())
            p = json.loads(plan.read_text())
            need(result['status'] == 'PASS_SELECTED_CONTINUOUS_CURRENT_HISTORY', 'complete window')
            need(result['counts'] == p['counts'] and result['CTAs'] == p['kernel_work']['CTAs'] and
                 result['nodes'] == p['kernel_work']['nodes'], 'no omitted work')
            need(result['ada_alignment']['profile'] == p['ada_alignment_profile'] and
                 not result['hardware_accuracy_claimed'], 'executed profile and limited claim')
            hbf = result['HBFSIM']
            need(hbf['accepted'] == hbf['completed'] and hbf['reserved_bursts_at_end'] == 0 and
                 hbf['native_enqueue_service_violations'] == 0, 'closed physical backend')
            receipt['cases'].append(dict(plan=pin(plan), result=pin(result_file),
                profile=result['ada_alignment'], counts=result['counts'], CTAs=result['CTAs'],
                nodes=result['nodes'], cycles=result['cycles'], physical=hbf['physical'],
                host_execution_seconds=result['host_execution_seconds']))
            save()
        need([pin(p['path']) for p in before] == before, 'frozen input/code/objects unchanged')
        receipt.update(status='PASS_ACTUAL_NATIVE_ALIGNMENT_WINDOWS', sources_unchanged=True)
    except BaseException as e:
        receipt.update(status='FAIL_ACTUAL_NATIVE_ALIGNMENT_WINDOWS_PRESERVED', error=repr(e))
    save()
    print(json.dumps({k:receipt[k] for k in ('status', 'elapsed_seconds', 'error') if k in receipt}))
    return 0 if receipt['status'].startswith('PASS') else 1


if __name__ == '__main__':
    raise SystemExit(main())
