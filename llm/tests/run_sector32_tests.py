#!/usr/bin/env python3
"""Build and run only bounded CPU components/actual CLI, with immutable source pins."""
import argparse, hashlib, json, os, pathlib, platform, resource, shlex, shutil, subprocess, time
ROOT=pathlib.Path(__file__).resolve().parents[2]
def pin(p):
    p=p.resolve();b=p.read_bytes();return {'path':str(p),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()}
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',required=True);ap.add_argument('--cxx',default=os.environ.get('CXX','clang++'));args=ap.parse_args()
    out=pathlib.Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False);cxx=shutil.which(args.cxx);assert cxx
    sources={p.resolve():pin(p) for top in ('llm','source') for p in (ROOT/top).rglob('*') if p.is_file() and p.suffix in ('.h','.hpp','.cpp','.cc')}
    commands=[]
    def run(name,argv,env=None,timeout=180):
        start=time.monotonic();u0=resource.getrusage(resource.RUSAGE_CHILDREN);p=subprocess.run(argv,cwd=ROOT,text=True,capture_output=True,env=env,timeout=timeout);u1=resource.getrusage(resource.RUSAGE_CHILDREN)
        (out/(name+'.stdout')).write_text(p.stdout);(out/(name+'.stderr')).write_text(p.stderr)
        row={'name':name,'argv':list(map(str,argv)),'returncode':p.returncode,'CPU_minutes':(u1.ru_utime+u1.ru_stime-u0.ru_utime-u0.ru_stime)/60,'wall_minutes':(time.monotonic()-start)/60,'stdout':str(out/(name+'.stdout')),'stderr':str(out/(name+'.stderr'))};commands.append(row)
        assert p.returncode==0,(name,p.stderr)
        return p
    flags=['-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DTINY_SHA_PORTABLE']
    for include in ['llm/executor-r1','source','source/work/tilegen-full-r1/core-native-copy-r2/include','source/work/gddr6-support/delivery-stage/sources/third_party/gtsim-prepared/third_party']:flags+=['-I',include]
    for name,src in [('sector32-cache-test','llm/tests/sector32_cache_test.cpp'),('source-cache-runner','llm/executor-r1/main.cpp')]:
        run('build-'+name,[cxx,*flags,'-MMD','-MF',str(out/(name+'.d')),src,'-o',str(out/name)])
    tests=[]
    for profile in ('legacy32','r4'):
        env=dict(os.environ);env.pop('TILEGEN_ADA_REQUIRE_OBSERVED',None);env.update(ASAN_OPTIONS='detect_leaks=0',TILEGEN_ADA_L1_PROFILE=profile,TILEGEN_ADA_SHARED_BYTES='32768',TILEGEN_L2_DATA_POLICY='sector32',TILEGEN_EF_HIT_RATE='288',TILEGEN_L2_DIRTY_AGE_ACCESSES='64000000')
        result=run(profile,[str(out/'sector32-cache-test')],env=env);tests.append(json.loads(result.stdout))
    run('actual-cli',[shutil.which('python3'),str(ROOT/'llm/tests/sector32_cli_test.py'),'--binary',str(out/'source-cache-runner'),'--output',str(out/'cli')])
    dependencies=set()
    for name in ('sector32-cache-test','source-cache-runner'):
        data=(out/(name+'.d')).read_text().replace('\\\n',' ');dependencies.update((ROOT/x).resolve() for x in shlex.split(data.split(':',1)[1]))
    pins=[]
    for p in sorted(dependencies):
        current=pin(p);assert current==sources[p],f'build source changed: {p}';pins.append(current)
    result={'schema':'SECTOR32_CACHE_COMPONENT_VALIDATION_V1','status':'PASS_SECTOR32_COMPONENTS_AND_ACTUAL_MAIN','platform':platform.platform(),'sanitizers':['address','undefined'],'leak_sanitizer_enabled':False,'commands':commands,'component_tests':tests,'actual_cli_receipt':pin(out/'cli/receipt.json'),'source_pins':pins,'source_closure':True,'binaries':[pin(out/name) for name in ('sector32-cache-test','source-cache-runner')],'full_model_or_GPU_test':False}
    (out/'receipt.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'status':result['status'],'receipt':str(out/'receipt.json'),'checks':sum(x['checks'] for x in tests),'actual_CLI_tests':5}))
if __name__=='__main__':main()
