#!/usr/bin/env python3
"""CPU-only SiLU typed/legacy materializer qualification; no simulator run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
# Root of the original sealed-input work tree. Overridable so this test is not
# tied to the author's macOS path; see --source-root / TILEGEN_SEALED_SOURCE_ROOT.
OLD = Path(os.environ.get(
    'TILEGEN_SEALED_SOURCE_ROOT',
    '/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1'))


def pin(p):
    p = Path(p)
    return dict(path=str(p.resolve()), bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source-root', type=Path, default=OLD,
                        help='root containing canonical-silu-driver-r1/sealed-inputs.json '
                             '(default: $TILEGEN_SEALED_SOURCE_ROOT, else the original author path)')
    a = parser.parse_args()
    out = a.output.resolve();out.mkdir(parents=True, exist_ok=False)
    config = json.loads((ROOT/'build-config.json').read_text())
    pool = ROOT/'source/work/tilegen-full-r1/driver-pooled-fusednorm-r1/streaming.cpp'
    raw = pool.read_text();cut = raw.index('\nJ service_map(const std::vector<std::unique_ptr<Prepared>>&')
    # Exact existing SourceBundle/SourceCatalog definitions, excluding all
    # simulation entry points. No replacement source validation in the fixture.
    (out/'native_sequence_source_pool.inc').write_text(raw[:cut]+'\n} // namespace native_sequence\n')
    old = a.source_root.resolve()
    manifest = old/'canonical-silu-driver-r1/sealed-inputs.json'
    if not manifest.is_file():
        parser.error(f'sealed-inputs.json not found under {old}; pass --source-root')
    sealed = json.loads(manifest.read_text())
    evidence = []
    for name in ['plan_file', 'template_file', 'program_file', 'register_file', 'model_manifest', 'model_source']:
        expected = sealed[name];actual = pin(expected['path'])
        assert all(actual[k] == expected[k] for k in ['bytes','sha256'])
        evidence.append(actual)
    plan = json.loads(Path(sealed['plan_file']['path']).read_text())
    spans = sorted((o['pointer']//128*128, (o['end_exclusive']+127)//128*128) for c in plan['calls'] for o in c['objects'].values())
    merged = []
    for lo, hi in spans:
        if merged and lo <= merged[-1][1]: merged[-1][1] = max(hi, merged[-1][1])
        else: merged.append([lo,hi])
    rows, offset = [], 0
    for lo, hi in merged:
        rows.append(dict(source_base=lo,bytes=hi-lo,service_base=offset));offset += hi-lo
    envelope = dict(schema='CANONICAL_SILU_MODELED_SEQUENCE_V1', plan_file=sealed['plan_file'],memory_model=sealed['memory_model'],
        service_address_map=dict(schema='SG_SOURCE_TO_SERVICE_MAP_V1',qualification='PACKED_SOURCE_GENERATED_128B_LINES_NOT_PHYSICAL_HARDWARE_ADDRESSES',spans=rows),
        selected_source_keys=[c['source_launch_key'] for c in plan['calls']],max_kernel_cycles=100000000,aggregate_observations=False)
    fixture = out/'all96-envelope.json';fixture.write_text(json.dumps(envelope, separators=(',',':'))+'\n')
    flags = ['-std=c++20','-O2','-ffunction-sections','-fdata-sections']
    flags += ['-D'+v for v in config['definitions']]
    flags += ['-I'+str(ROOT/p) for p in ['source','source/work/tilegen-full-r1/driver-pooled-fusednorm-r1']+config['include_directories']]
    flags += ['-I'+str(out)]
    env = dict(os.environ,CUDA_VISIBLE_DEVICES='')
    if a.sanitize:
        flags += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        env['ASAN_OPTIONS']='detect_leaks=0:halt_on_error=1'
        env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
    cpp=ROOT/'tests/silu_typed_binding_test.cpp';binary=out/'silu_typed_binding_test'
    includes=[pool,cpp,Path(__file__),ROOT/'build-config.json',ROOT/'source/native_typed_identity.h',ROOT/'source/work/tilegen-full-r1/canonical-silu-driver-r1/prepared_memory.h',
              ROOT/'source/work/tilegen-full-r1/canonical-silu-driver-r1/typed_binding.h',ROOT/'source/work/tilegen-full-r1/canonical-silu-driver-r1/model_plan.h',
              ROOT/'source/work/tilegen-full-r1/canonical-silu-driver-r1/sealed_inputs.h']
    sources=[pin(p) for p in includes]
    steps=[]
    for name,argv in [('compile',[shutil.which(config['compiler']),*flags,str(cpp),'-o',str(binary)]),('run',[str(binary),str(fixture)])]:
        start=time.monotonic();usage=resource.getrusage(resource.RUSAGE_CHILDREN)
        with (out/(name+'.stdout')).open('wb') as stdout,(out/(name+'.stderr')).open('wb') as stderr:
            r=subprocess.run(argv,stdout=stdout,stderr=stderr,env=env,timeout=180)
        end_usage=resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu_user=end_usage.ru_utime-usage.ru_utime;cpu_system=end_usage.ru_stime-usage.ru_stime
        steps.append(dict(name=name,argv=argv,returncode=r.returncode,seconds=time.monotonic()-start,
            cpu_user_seconds=cpu_user,cpu_system_seconds=cpu_system,cpu_seconds=cpu_user+cpu_system))
        if r.returncode:raise RuntimeError(name+' failed; '+str(out/(name+'.stderr')))
    result=json.loads((out/'run.stdout').read_text())
    assert result['status']=='PASS_SILU_TYPED_MEMORY_CANDIDATE' and result['legacy_calls']==96 and result['typed_decode_calls']==64
    assert sources==[pin(p['path']) for p in sources] and evidence==[pin(p['path']) for p in evidence]
    receipt=dict(status='PASS',schema='SILU_TYPED_BINDING_CPU_TEST_V1',CPU_only=True,GPU_executed=False,HBFSIM_executed=False,
        sanitizer='address,undefined' if a.sanitize else None,leak_detection=False if a.sanitize else None,sources=sources,
        original_evidence=evidence,fixture=pin(fixture),binary=pin(binary),steps=steps,result=result)
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt))


if __name__=='__main__':main()
