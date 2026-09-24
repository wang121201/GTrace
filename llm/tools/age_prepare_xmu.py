"""Build and admit a finite 32 B sector experiment under an outer CPU lease.

Frozen current-process source graphs remain on XMU. This script never changes
prior experiments, never captures GPU inputs, and never launches its case jobs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import time

ROOT = Path('/home/xmu/nvidiagds/codex-runs/gtsim-ada-age-20260922-r1')
CTRL = Path('/home/xmu/nvidiagds/simulators/hyfiss/analysis/full-inference-matrix-20260919-r1/sampled-workflow-r1/run_job.py')
BASE_SPECS = {
    32: Path('/home/xmu/nvidiagds/codex-runs/gtsim-ada-r4-llm-20260922-r1/qwen-r4-spec.json'),
    128: Path('/home/xmu/nvidiagds/codex-runs/gtsim-ada-r4-p128-20260922-r1/cases/qwen-p128d2/r4-spec.json'),
}
ARMS = [('p32-age0',32,'sector32','run-end',0,0),
        ('p128-age0',128,'sector32','run-end',1,0),
        ('p32-age96m',32,'sector32','run-end',2,96000000),
        ('p128-age96m',128,'sector32','run-end',8,96000000),
        ('p32-age128m',32,'sector32','run-end',10,128000000),
        ('p128-age128m',128,'sector32','run-end',11,128000000)]

def need(ok,msg):
    if not ok: raise ValueError(msg)

def pin(p):
    p=Path(p).resolve(); h=hashlib.sha256(); size=0
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1<<20),b''): h.update(b); size+=len(b)
    return dict(path=str(p),bytes=size,sha256=h.hexdigest())

def checked(row):
    need(pin(row['path'])==row,'immutable source pin mismatch: '+row['path']); return row

def save(p,v):
    with Path(p).open('x') as f: json.dump(v,f,indent=2); f.write('\n')

def unique(rows):
    d={}
    for r in rows:
        need(r['path'] not in d or d[r['path']]==r,'conflicting immutable pins')
        d[r['path']]=r
    return list(d.values())

def get(a,k):return a[a.index(k)+1]

def replace(a,k,v):a[a.index(k)+1]=str(v)

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--root',type=Path,required=True); a=ap.parse_args()
    need(a.root.resolve()==ROOT,'exact independent task root')
    repo=ROOT/'repo'; producer=repo/'llm/executor-r1/whole_stream.py'
    build=ROOT/'build-r1'; build.mkdir(exist_ok=False)
    manifest=json.loads((ROOT/'repo-manifest.json').read_text())
    sources=[]
    for r in manifest['files']:
        p=repo/r['relative_path']; actual=pin(p)
        need(actual['sha256']==r['sha256'] and actual['bytes']==r['bytes'],'uploaded file identity')
        sources.append(actual)
    sources=unique(sources+[pin(ROOT/'repo-manifest.json'),pin(CTRL)])
    pinned={r['path']:r for r in sources}
    includes=[repo/'llm/executor-r1',repo/'source',repo/'source/work/tilegen-full-r1/core-native-copy-r2/include',repo/'source/work/gddr6-support/delivery-stage/sources/third_party/gtsim-prepared/third_party']
    binary=build/'source-cache-runner'; dep=build/'runner.d'
    compiler=shutil.which('c++'); need(compiler is not None,'C++ compiler required')
    command=[compiler,'-std=c++20','-O3','-DTINY_SHA_PORTABLE','-MMD','-MF',str(dep)]+['-I'+str(x) for x in includes]+[str(repo/'llm/executor-r1/main.cpp'),'-o',str(binary)]
    start=time.monotonic()
    env={k:v for k,v in os.environ.items() if not k.startswith('TILEGEN_')}
    env.update(OMP_NUM_THREADS='1',MKL_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1',PYTHONDONTWRITEBYTECODE='1')
    proc=subprocess.run(command,env=env,capture_output=True,text=True)
    (build/'compiler.log').write_text(proc.stdout+proc.stderr); proc.check_returncode()
    need(binary.read_bytes()[:4]==b'\x7fELF','Linux ELF required')
    dependencies=[]
    for word in sorted(set(shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))):
        p=str(Path(word).resolve()); need(p in pinned,'unpinned compiler dependency: '+p)
        dependencies.append(checked(pinned[p]))
    evidence=dict(status='PASS_PINNED_BUILD_PENDING_LLM_TEST',binary=pin(binary),actual_compilation_dependencies=dependencies,
                  compile_wall_minutes=(time.monotonic()-start)/60,command=command)
    save(build/'evidence.json',evidence)
    cases=ROOT/'cases'; cases.mkdir(exist_ok=False); originals={}; admissions={}
    for p,sp in BASE_SPECS.items():
        original=json.loads(sp.read_text()); originals[p]=original
        for r in original['sources']:checked(r)
        argv=original['argv']; graph=json.loads(Path(get(argv,'--graph')).read_text()); c=graph['input_contract']
        need(c['model_key']=='qwen25_1p5b' and c['prefill_length']==p and c['decode_steps']==2,'finite paired input')
        need(c['batch_size']==1 and c['dtype']=='bfloat16' and c['warmup_runs']==1 and not c['cuda_graph'] and not c['output_feedback'],'frozen workload controls')
        baseresult=Path(get(argv,'--output'))
        state=json.loads((baseresult/'status.json').read_text())
        need(state['status']=='PASS_COMPLETE_NATIVE_GRAPH_CACHE_EXECUTION','closed old baseline required')
        admissions[p]=dict(base_spec=pin(sp),baseline_status=pin(baseresult/'status.json'),
                           baseline_source_stream=state['source_stream'],baseline_counts=state['executed_counts'],
                           baseline_phase_traffic=pin(baseresult/'phase-traffic.json'),
                           graph=pin(get(argv,'--graph')),registry=pin(get(argv,'--registry')),
                           resources=pin(get(argv,'--launch-resources')))
    prepared=[]
    for name,p,policy,drain,cpu,age in ARMS:
        old=originals[p]; out=cases/name; out.mkdir(exist_ok=False)
        argv=old['argv'][:]; argv[2]=str(producer); replace(argv,'--runner',binary)
        replace(argv,'--output',out/'preflight'); argv+=['--drain-policy',drain,'--expected-dirty-age-accesses',str(age)]
        caseenv=dict(old['environment'],TILEGEN_L2_DATA_POLICY=policy,TILEGEN_ADA_L1_PROFILE='r4',TILEGEN_L2_DIRTY_AGE_ACCESSES=str(age))
        for k,v in {'TILEGEN_EF_HIT_RATE':'288','TILEGEN_L2_DIRTY_AGE_ACCESSES':str(age),'TILEGEN_ADA_REQUIRE_OBSERVED':'1'}.items():need(caseenv.get(k)==v,'controlled cache parameter changed')
        proc=subprocess.run(argv+['--preflight-only'],env=dict(env,**caseenv),capture_output=True,text=True)
        (out/'preflight.log').write_text(proc.stdout+proc.stderr);proc.check_returncode()
        state=json.loads((out/'preflight/status.json').read_text())
        need(state['status']=='PASS_COMPLETE_NATIVE_INPUT_PREFLIGHT','complete source input admission')
        need(state['input_preflight']['kernel_count']==admissions[p]['baseline_counts']['kernels'],'source kernel coverage changed')
        allpins=unique(sources+old['sources']+state['fast_source_runtime_pins']+[pin(binary),pin(build/'evidence.json'),pin(BASE_SPECS[p])])
        for row in allpins:checked(row)
        replace(argv,'--output',out/'result')
        spec=dict(case_id=name+'-r1',tool='native-functional-sector32-age-ablation',input_kind='COMPLETE_NATIVE_TILEGRAPH_SOURCE_PROGRAMS_AND_API_HISTORY',cpu=cpu,gpu=None,seconds=21600,rss_limit_bytes=16<<30,argv=argv,environment=caseenv,sources=allpins)
        sp=out/'spec.json';save(sp,spec)
        prepared.append(dict(case=name,prefill=p,decode=2,policy=policy,drain_policy=drain,dirty_age_accesses=age,cpu=cpu,spec=pin(sp),output=str(out/'result'),job_directory=str(out/'job'),admission=admissions[p]))
    save(ROOT/'prepare-result.json',dict(status='PASS_FINITE_CASE_PREFLIGHT_NOT_LAUNCHED',cases=prepared,build=evidence,
        old_inputs_modified=False,new_GPU_capture=False,compute_stall_cosimulation=False))
    print(json.dumps(dict(status='PASS_FINITE_CASE_PREFLIGHT_NOT_LAUNCHED',cases=len(prepared),compile_wall_minutes=evidence['compile_wall_minutes'])))

if __name__=='__main__':main()
