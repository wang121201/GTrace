#!/usr/bin/env python3
"""Reproduce original regression and set-age prototype checks; bounded CPU only."""
import argparse, hashlib, json, os, pathlib, resource, shlex, shutil, subprocess, time
ROOT=pathlib.Path(__file__).resolve().parents[2]
ORIGINAL_FROZEN_COMMIT='17f03b070ecb86fa034597a6a7ac972ff319bef7'
def pin(p):
    p=p.resolve();b=p.read_bytes();return dict(path=str(p),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=pathlib.Path,required=True);ap.add_argument('--cxx',default='clang++');ap.add_argument('--frozen-commit',default=os.environ.get('TILEGEN_SECTOR32_FROZEN_COMMIT',ORIGINAL_FROZEN_COMMIT),help='commit holding the frozen sector32 candidate header; override when the original commit is absent');a=ap.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);commands=[]
    def run(name,argv,env=None):
        t=time.monotonic();before=resource.getrusage(resource.RUSAGE_CHILDREN);p=subprocess.run(list(map(str,argv)),cwd=ROOT,env=env,text=True,capture_output=True,timeout=240);after=resource.getrusage(resource.RUSAGE_CHILDREN)
        (out/(name+'.stdout')).write_text(p.stdout);(out/(name+'.stderr')).write_text(p.stderr)
        commands.append(dict(name=name,argv=list(map(str,argv)),returncode=p.returncode,CPU_minutes=(after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime)/60,wall_minutes=(time.monotonic()-t)/60));assert p.returncode==0,(name,p.stderr);return p
    py=shutil.which('python3');cxx=shutil.which(a.cxx);assert py and cxx
    sources={p.resolve():pin(p) for top in ('llm','source') for p in (ROOT/top).rglob('*') if p.is_file() and p.suffix in ('.h','.hpp','.cpp','.cc','.py')}
    run('global-regression',[py,ROOT/'llm/tests/run_sector32_tests.py','--output',out/'global','--cxx',cxx])
    frozen_commit=a.frozen_commit
    try:
        raw=run('export-frozen-sector32',['git','show',frozen_commit+':llm/executor-r1/candidate_direct_cache.h']).stdout
    except AssertionError as error:
        raise SystemExit(f'cannot export the frozen sector32 candidate header from {frozen_commit}: {error}\n'
            f'Pass --frozen-commit <sha> or set TILEGEN_SECTOR32_FROZEN_COMMIT. The original commit {ORIGINAL_FROZEN_COMMIT} '
            'is absent from a reconstructed repository; point this at the equivalent reconstructed commit instead.')
    assert raw.count('namespace direct_native {')==1
    frozen=out/'frozen_sector32_cache.h';frozen.write_text(raw.replace('namespace direct_native {','namespace frozen_sector32 { using direct_native::EfHitThrottle;'))
    sources[frozen]=pin(frozen)
    flags=['-I',str(out),'-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DTINY_SHA_PORTABLE']
    for include in ('llm/executor-r1','source','source/work/tilegen-full-r1/core-native-copy-r2/include','source/work/gddr6-support/delivery-stage/sources/third_party/gtsim-prepared/third_party'):flags+=['-I',include]
    run('build-set-age',[cxx,*flags,'-MMD','-MF',out/'set-age.d','llm/tests/set_age_cache_test.cpp','-o',out/'set-age-test'])
    tests=[]
    for profile in ('legacy32','r4'):
        env=dict(os.environ);env.pop('TILEGEN_ADA_REQUIRE_OBSERVED',None);env.update(ASAN_OPTIONS='detect_leaks=0',TILEGEN_ADA_L1_PROFILE=profile,TILEGEN_ADA_SHARED_BYTES='32768');tests.append(json.loads(run('set-age-'+profile,[out/'set-age-test'],env).stdout))
    run('clock-contract',[py,ROOT/'llm/tests/set_age_contract_test.py','--binary',out/'global/source-cache-runner','--input',out/'global/cli/sector32/input.jsonl','--output',out/'contract'])
    run('stream-boundaries',[py,ROOT/'llm/tools/sector32_stream_test.py'])
    data=(out/'set-age.d').read_text().replace('\\\n',' ');dependencies={(ROOT/x).resolve() for x in shlex.split(data.split(':',1)[1])};dependencies.update([ROOT/'llm/executor-r1/whole_stream.py',ROOT/'llm/tests/set_age_contract_test.py',pathlib.Path(__file__).resolve()])
    for p in dependencies:
        assert pin(p)==sources[p],f'source changed during build/test: {p}'
    result=dict(source_closure=True,frozen_sector32_reference=dict(commit=frozen_commit,original_sha256=hashlib.sha256(raw.encode()).hexdigest(),compiled_header=pin(frozen)),status='PASS_SET_AGE_PROTOTYPE_LOCAL_COMPONENTS',full_model_or_GPU_test=False,commands=commands,set_component_tests=tests,source_pins=[pin(p) for p in sorted(dependencies)],global_regression_receipt=pin(out/'global/receipt.json'),clock_contract_receipt=pin(out/'contract/receipt.json'),binary=pin(out/'set-age-test'))
    (out/'receipt.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(dict(status=result['status'],receipt=str(out/'receipt.json'),set_checks=sum(x['checks'] for x in tests))))
if __name__=='__main__':main()
