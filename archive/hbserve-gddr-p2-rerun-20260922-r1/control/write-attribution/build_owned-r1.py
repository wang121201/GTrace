"""Recompile only six writer-dependent core TUs; no link, driver, or simulation."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import shlex
import subprocess
import sys
import time

sys.dont_write_bytecode = True
D = Path(__file__).resolve().parent
R = D.parents[3]
S = R / 'work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot'
B = S / 'build/shared-h288-r1'
OUT = D / 'build-r1'

def need(value, why):
    if not value:
        raise ValueError(why)

def read(p):
    return json.loads(Path(p).read_text())

def pin(p):
    p = Path(p).resolve()
    data = p.read_bytes()
    return dict(path=str(p), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())

def write(p, data):
    Path(p).write_text(json.dumps(data, indent=2) + '\n')

def deps(p):
    raw = Path(p).read_text().replace('\\\n', ' ')
    target, rest = raw.split(':', 1)
    return Path(target.strip()).resolve(), sorted({str(Path(x).resolve()) for x in shlex.split(rest)})

def worker(commands_file):
    # This process and every compiler inherit the one outer owned process group.
    data = read(commands_file)
    result = dict(jobs=2, steps=[])
    t = time.clock_gettime(time.CLOCK_MONOTONIC_RAW)
    with (OUT/'compiler-version.stdout').open('xb') as stdout, (OUT/'compiler-version.stderr').open('xb') as stderr:
        v = subprocess.run([data['compiler'], '--version'], stdout=stdout, stderr=stderr)
    need(v.returncode == 0 and (OUT/'compiler-version.stderr').stat().st_size == 0, 'compiler version query')
    need((OUT/'compiler-version.stdout').read_text() == data['compiler_version'], 'compiler identity changed')
    active = []
    todo = iter(data['commands'])
    failed = False
    while True:
        while len(active) < 2 and not failed:
            item = next(todo, None)
            if item is None:
                break
            stdout = (OUT/(item['name']+'.stdout')).open('xb')
            stderr = (OUT/(item['name']+'.stderr')).open('xb')
            started = time.clock_gettime(time.CLOCK_MONOTONIC_RAW)
            child = subprocess.Popen(item['argv'], stdout=stdout, stderr=stderr)
            active.append((child, item, started, stdout, stderr))
        if not active:
            break
        remaining = []
        for child, item, started, stdout, stderr in active:
            rc = child.poll()
            if rc is None:
                remaining.append((child, item, started, stdout, stderr))
                continue
            stdout.close(); stderr.close()
            step = dict(name=item['name'], argv=item['argv'], returncode=rc,
                        seconds=time.clock_gettime(time.CLOCK_MONOTONIC_RAW)-started,
                        stdout=pin(OUT/(item['name']+'.stdout')), stderr=pin(OUT/(item['name']+'.stderr')))
            result['steps'].append(step)
            failed = failed or rc != 0
            write(OUT/'worker.json', result)
        active = remaining
        if active:
            time.sleep(0.02)
    if not failed and len(result['steps']) == 6:
        result['constructor_steps'] = []
        for item in data['constructor_commands']:
            at = time.clock_gettime(time.CLOCK_MONOTONIC_RAW)
            with (OUT/(item['name']+'.stdout')).open('xb') as stdout, (OUT/(item['name']+'.stderr')).open('xb') as stderr:
                child = subprocess.run(item['argv'], stdout=stdout, stderr=stderr)
            result['constructor_steps'].append(dict(name=item['name'], argv=item['argv'], returncode=child.returncode,
                seconds=time.clock_gettime(time.CLOCK_MONOTONIC_RAW)-at,
                stdout=pin(OUT/(item['name']+'.stdout')), stderr=pin(OUT/(item['name']+'.stderr'))))
            write(OUT/'worker.json', result)
            if child.returncode:
                failed = True
                break
    result['elapsed_seconds'] = time.clock_gettime(time.CLOCK_MONOTONIC_RAW)-t
    result['status'] = 'PASS_SIX_TU_BUILD_ONLY' if not failed and len(result['steps']) == 6 else 'FAIL_BUILD_PRESERVED'
    write(OUT/'worker.json', result)
    return 0 if result['status'].startswith('PASS') else 1

def main():
    OUT.mkdir(exist_ok=False)
    owned = R/'work/tilegen-hbf-traceoff-full-r1/owned_group.py'
    spec = importlib.util.spec_from_file_location('_owned_writer_build', owned)
    G = importlib.util.module_from_spec(spec); spec.loader.exec_module(G)
    started = G.raw(); deadline = started + 180
    receipt = dict(schema='CURRENT_HISTORY_ATTRIBUTION_SIX_TU_BUILD_V1', status='RUNNING',
                   CPU_only=True, simulation_executed=False, GPU_executed=False,
                   driver_compiled=False, linked=False, max_seconds=180, cleanup_reserve_seconds=8, jobs=2)
    def save():
        receipt['elapsed_seconds'] = G.raw()-started
        write(OUT/'receipt.json', receipt)
    save()
    try:
        build = read(B/'build-receipt.json')
        need(build['status'] == 'PASS_BUILD_ONLY_NO_SIMULATION' and build['sources_unchanged'], 'original build must be closed')
        proof = read(D/'source-proof.json')
        need(proof['status'] == 'PASS_SOURCE_SINGLE_LITERAL_INVERSE' and proof['inverse_byte_exact'], 'sealed narrow source proof')
        original = Path(proof['original']['path']); candidate = Path(proof['candidate']['path'])
        need(pin(original) == proof['original'] and pin(candidate) == proof['candidate'], 'writer source pins')
        new = candidate.read_text(); old = original.read_text()
        need(new.count(proof['new_literal']) == 1 and new.replace(proof['new_literal'], proof['old_literal']) == old,
             'sole literal constructor range change inverse exact')
        overlay = read(D/'overlay.json')
        need(overlay == {'version':0, 'case-sensitive':True, 'use-external-names':False,
                        'roots':[{'type':'file','name':str(original),'external-contents':str(candidate)}]}, 'exact single-header overlay')
        frozen = read(D/'readiness.json')
        need(frozen['status'] == 'SOURCE_READY_NOT_MODEL_QUALIFIED', 'source package must be frozen')
        source = {Path(__file__), D/'constructor_fixture-r1.cpp', owned, B/'build-receipt.json', D/'source-proof.json', D/'overlay.json', D/'readiness.json', original, candidate}
        for item in frozen['source_pins']:
            need(pin(item['path']) == item, 'frozen attribution source pin')
            source.add(Path(item['path']))
        source_pins = {str((S/k).resolve()):v for k,v in build['source_pins'].items()}
        for path, digest in source_pins.items():
            need(pin(path)['sha256'] == digest, 'original build source pin')
            source.add(Path(path))
        prior_receipt = D.parent/'current-history-loader-r3/run-r1/receipt.json'
        prior = read(prior_receipt)
        need(prior['status'].startswith('PASS') and prior['sources_unchanged'], 'existing closed object qualification')
        object_pins = {p['path']:p for p in prior['pins'] if p['path'].endswith('.o')}
        source.add(prior_receipt)
        rows = []
        for i in range(22):
            name = f'{i:02d}'
            dp = B/(name+'.d')
            target, items = deps(dp)
            need(target == B/(name+'.o'), 'original dependency target')
            includes_writer = str(original) in items
            need(includes_writer == (i <= 6), 'exact writer-dependent TU range')
            for path in items:
                need(path in source_pins and pin(path)['sha256'] == source_pins[path], 'all TU dependencies are pinned original source')
            source.add(dp)
            row = dict(name=name, dependency_file=pin(dp), includes_writer=includes_writer, dependencies=items,
                       disposition='root_driver_must_use_same_overlay' if i == 0 else 'recompiled_with_overlay' if i <= 6 else 'reuse_original_object')
            if i:
                obj = B/(name+'.o')
                need(pin(obj) == object_pins[str(obj)], 'old object equals previously closed loader pin')
                row['original_object'] = pin(obj); source.add(obj)
            rows.append(row)
        for group in ('dependent_TUs', 'independent_HBFSIM_TUs'):
            for item in proof[group]:
                need(pin(item['dependency_file']['path']) == item['dependency_file'], 'source proof dependency pin')
        write(OUT/'dependency-proof.json', dict(status='PASS_00_TO_06_DEPENDENT_07_TO_21_INDEPENDENT',
              original_build=pin(B/'build-receipt.json'), prior_object_qualification=pin(prior_receipt), TUs=rows))
        commands = []
        steps = {x['name']:x for x in build['steps']}
        for i in range(1,7):
            name = f'{i:02d}'; argv = list(steps[name]['command'])
            need(argv[:1+len(build['flags'])] == [build['compiler'], *build['flags']], 'all original build flags exact')
            need(argv[-7:] == ['-MMD','-MF',str(B/(name+'.d')),'-c',argv[-3],'-o',str(B/(name+'.o'))], 'original compile command structure')
            argv[argv.index('-MF')+1] = str(OUT/(name+'.d'))
            argv[argv.index('-o')+1] = str(OUT/(name+'.o'))
            at = argv.index('-MMD'); argv[at:at] = ['-ivfsoverlay',str(D/'overlay.json')]
            commands.append(dict(name=name, argv=argv))
        constructor_commands = [dict(name='constructor-compile', argv=[build['compiler'], *build['flags'],
            '-ivfsoverlay', str(D/'overlay.json'), '-MMD', '-MF', str(OUT/'constructor.d'),
            str(D/'constructor_fixture-r1.cpp'), '-o', str(OUT/'constructor-test')]),
            dict(name='constructor-run', argv=[str(OUT/'constructor-test')])]
        write(OUT/'commands.json', dict(compiler=build['compiler'], compiler_version=build['compiler_version'], commands=commands, constructor_commands=constructor_commands))
        before = [pin(p) for p in sorted(source)]
        receipt.update(pins=before, dependency_proof=pin(OUT/'dependency-proof.json'), commands=pin(OUT/'commands.json'))
        save()
        need(G.raw() < deadline-8, 'build budget after read-only admission')
        group = None
        with G.SignalLatch() as signals:
            try:
                group = G.OwnedGroup([sys.executable,'-B',str(Path(__file__).resolve()),'--worker',str(OUT/'commands.json')], origin_raw=G.raw())
                reason = group.wait_reason(deadline-8, signals)
            finally:
                if group is not None:
                    receipt['cleanup'] = group.cleanup(term_grace=3,kill_grace=3)
                receipt['signals'] = signals.receipt()
        receipt.update(reason=reason, returncode=group.child.returncode); save()
        need(reason == 'leader_exited' and group.child.returncode == 0 and receipt['cleanup']['cleanup_complete'] and
             receipt['cleanup']['group_absent_after_cleanup'] and receipt['cleanup']['direct_child_reaped'] and
             not receipt['cleanup']['errors'] and signals.first_signal is None, 'owned six-TU compilation closure')
        w = read(OUT/'worker.json')
        need(w['status'] == 'PASS_SIX_TU_BUILD_ONLY' and sorted(x['name'] for x in w['steps']) == [f'{i:02d}' for i in range(1,7)], 'exact six completed compiles')
        test = read(OUT/'constructor-run.stdout')
        need(test['status'] == 'PASS_ACTUAL_CONSTRUCTOR_DOMAIN_AND_ZERO_NATIVE_LEDGER' and
             test['extended_domain']['calls'] == 3018 and test['default_domain']['calls'] == 1138 and test['rejections'] == 2,
             'actual constructor range and zero native ledger')
        need((OUT/'constructor-run.stderr').stat().st_size == 0, 'constructor runtime stderr')
        _, test_deps = deps(OUT/'constructor.d')
        need(str(original) in test_deps and all(p in source_pins or p == str(D/'constructor_fixture-r1.cpp') for p in test_deps), 'constructor same overlay dependency qualification')
        result_objects = []
        for i in range(1,22):
            name = f'{i:02d}'
            if i <= 6:
                target, items = deps(OUT/(name+'.d'))
                need(target == OUT/(name+'.o') and items == rows[i]['dependencies'], 'overlay preserves logical dependency closure')
                result_objects.append(dict(name=name, disposition='recompiled_with_overlay', object=pin(OUT/(name+'.o')), dependencies=pin(OUT/(name+'.d'))))
            else:
                result_objects.append(dict(name=name, disposition='reuse_original_object', object=pin(B/(name+'.o')), dependencies=pin(B/(name+'.d'))))
        need([pin(p['path']) for p in before] == before, 'all original sources, overlay and objects unchanged')
        receipt.update(status='PASS_OWNED_SIX_TU_ATTRIBUTION_BUILD_ONLY', sources_unchanged=True,
                       worker=pin(OUT/'worker.json'), objects=result_objects, constructor_result=pin(OUT/'constructor-run.stdout'), constructor_binary=pin(OUT/'constructor-test'), constructor_dependencies=pin(OUT/'constructor.d'),
                       compiler_version=pin(OUT/'compiler-version.stdout'), link_requires_same_overlay_for_driver=True)
    except BaseException as exc:
        receipt.update(status='FAIL_OWNED_ATTRIBUTION_BUILD_PRESERVED', error=type(exc).__name__+': '+str(exc))
    save()
    print(json.dumps({k:receipt[k] for k in ('status','elapsed_seconds','error') if k in receipt}))
    return 0 if receipt['status'].startswith('PASS') else 1

if __name__ == '__main__':
    raise SystemExit(worker(sys.argv[2]) if len(sys.argv)==3 and sys.argv[1]=='--worker' else main())
