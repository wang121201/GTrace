"""Prepare, but never launch, one admitted P128 case with a frozen XMU binary."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

REMOTE_ROOT = Path('/home/xmu/nvidiagds/codex-runs/gtsim-ada-r4-p128-20260922-r1')
OLD_ROOT = Path('/home/xmu/nvidiagds/codex-runs/gtsim-ada-r4-llm-20260922-r1')
BINARY = OLD_ROOT / 'build-qwen/source-cache-runner'
BINARY_SPEC = OLD_ROOT / 'qwen-r2-spec.json'
DECODE_STEPS = (2, 4, 8, 16)
KEY_BINARY_SOURCES = ('llm/executor-r1/main.cpp', 'llm/executor-r1/runner.h',
    'llm/executor-r1/candidate_direct_cache.h', 'llm/executor-r1/llm_l1_adapter.h',
    'llm/executor-r1/native_gemv.h', 'llm/executor-r1/qwen_gemv.h',
    'llm/executor-r1/frozen_cache_geometry.h', 'llm/executor-r1/ef_hit_throttle.h',
    'llm/fast-prefill-r2/native_prefill.h', 'llm/fast-prefill-r2/native_prefill_contracts.h',
    'source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h',
    'source/work/tilegen-full-r1/core-native-copy-r2/include/ada_r4_profile.h')


def need(ok, message):
    if not ok:
        raise ValueError(message)


def pin(path):
    path = Path(path).resolve()
    digest = hashlib.sha256()
    size = 0
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block); size += len(block)
    return dict(path=str(path), bytes=size, sha256=digest.hexdigest())


def absolute(value):
    need(isinstance(value, str) and Path(value).is_absolute(), 'absolute source path required')
    return Path(value).resolve()


def checked(row):
    need(set(row) == {'path', 'bytes', 'sha256'}, 'source pin fields')
    need(type(row['bytes']) is int and row['bytes'] >= 0, 'source pin byte count')
    need(re.fullmatch('[0-9a-f]{64}', row['sha256']) is not None, 'source SHA256')
    need(pin(absolute(row['path'])) == row, 'sealed source pin mismatch')
    return row


def merge_pins(rows):
    result = {}
    for row in rows:
        name = row['path']
        need(name not in result or result[name] == row, 'conflicting source pins')
        result[name] = row
    return list(result.values())


def phases(decode):
    return [stage+'/'+phase for stage in ('Warmup', 'Measured')
            for phase in ['Prefill']+['Decode'+str(i) for i in range(1, decode+1)]]


def validate_admission(value):
    need(value['schema'] == 'ADMITTED_P128_CASE_V1', 'admission schema')
    need(isinstance(value['case_id'], str) and re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}', value['case_id']), 'safe case ID')
    need(value['model_key'] == 'qwen25_1p5b' and value['prefill_length'] == 128, 'finite Qwen P128 workload')
    d = value['decode_steps']
    need(type(d) is int and d in DECODE_STEPS, 'supported decode steps')
    paths = {k:absolute(value[k]) for k in ('graph','registry','runtime','native_tree','support_tree','launch_resources')}
    need(paths['native_tree'].is_dir() and paths['support_tree'].is_dir(), 'source roots must exist')
    need(paths['runtime'].is_relative_to(paths['native_tree']) or paths['runtime'].is_relative_to(paths['support_tree']), 'runtime outside declared input roots')
    expected = value['expected_counts']
    need(set(expected) == {'kernels','memory_APIs','allocation_observations','phases'}, 'expected count fields')
    need(all(type(v) is int and v >= 0 for v in expected.values()) and expected['kernels'] > 0, 'expected count types')
    need(expected['phases'] == 2*(d+1), 'full warmup/measured phase count')
    need(isinstance(value['source_pins'], list) and value['source_pins'], 'admitted input source pins required')
    sources = merge_pins([checked(row) for row in value['source_pins']])
    pinned = {row['path']:row for row in sources}
    for k in ('graph','registry','runtime'):
        need(str(paths[k]) in pinned, k+' must be in admitted source pins')
    graph = json.loads(paths['graph'].read_text())
    contract = graph['input_contract']
    for k in ('model_key','prefill_length','decode_steps'):
        need(contract[k] == value[k], 'graph contract differs from admission')
    need(contract['batch_size']==1 and contract['dtype']=='bfloat16' and contract['warmup_runs']==1
         and contract['sampling_retained'] is True and contract['output_feedback'] is False
         and contract['cuda_graph'] is False, 'complete fixed native BF16 B1 contract')
    need([p['phase'] for p in graph['phases']] == phases(d), 'graph phase order')
    node_pin = graph['artifacts']['nodes']
    checked(node_pin)
    need(pinned.get(node_pin['path']) == node_pin, 'nodes must be in admitted source pins')
    counts = collections.Counter(); launches=set()
    with Path(node_pin['path']).open() as stream:
        for line in stream:
            n=json.loads(line); counts[n['kind']]+=1
            if n['kind']=='native_kernel':
                ident=n['native_launch_id']
                need(type(ident) is int and ident >= 0 and ident not in launches, 'unique native launch ID')
                launches.add(ident)
    actual=dict(kernels=counts['native_kernel'],memory_APIs=counts['memory_api_submission'],
                allocation_observations=counts['allocation_API_observation'],phases=len(graph['phases']))
    need(actual == expected, 'actual graph counts differ from admission')
    resources=json.loads(paths['launch_resources'].read_text())
    need(resources['schema']=='OBSERVED_LAUNCH_CARVEOUT_V1' and resources['evidence_scope']=='per_kernel_launch', 'per-launch resource schema')
    need(resources['graph_sha256']==pinned[str(paths['graph'])]['sha256'], 'resource graph SHA mismatch')
    seen=set()
    for row in resources['kernels']:
        ident,shared=row['native_launch_id'],row['observed_shared_bytes']
        need(type(ident) is int and ident >= 0 and ident not in seen, 'unique resource launch ID')
        need(type(shared) is int and shared in (8192,16384,32768,65536,102400), 'supported shared bin')
        seen.add(ident)
    need(seen == launches, 'resources must cover complete initialization/warmup/measured kernels')
    return paths, sources, actual


def binary_evidence():
    sealed=json.loads(BINARY_SPEC.read_text()); index={row['path']:row for row in sealed['sources']}
    wanted=[BINARY]+[OLD_ROOT/'repo'/r for r in KEY_BINARY_SOURCES]
    for path in wanted:
        need(str(path) in index, 'old binary/source missing from frozen spec')
        checked(index[str(path)])
    with BINARY.open('rb') as stream:
        need(stream.read(4)==b'\x7fELF', 'existing Linux binary required')
    return [index[str(path)] for path in wanted]+[pin(BINARY_SPEC)]


def build_specs(root, admission, paths, cpus, source_pins):
    need(len(cpus)==2 and all(type(c) is int and 0 <= c <= 15 for c in cpus) and cpus[0]!=cpus[1], 'two distinct shared CPU IDs in0..15')
    directory=root/'cases'/admission['case_id']
    producer=root/'repo/llm/executor-r1/whole_stream.py'
    env=dict(TILEGEN_NATIVE_TREE=str(paths['native_tree']),TILEGEN_NATIVE_SUPPORT_TREE=str(paths['support_tree']),
             TILEGEN_ADA_REQUIRE_OBSERVED='1',TILEGEN_L2_DIRTY_AGE_ACCESSES='64000000',TILEGEN_EF_HIT_RATE='288',
             PYTHONDONTWRITEBYTECODE='1',OMP_NUM_THREADS='1',MKL_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1')
    argv=['/usr/bin/python3','-B',str(producer),'--graph',str(paths['graph']),'--registry',str(paths['registry']),
          '--runtime',str(paths['runtime']),'--runner',str(BINARY),'--fast-gemv','--fast-prefill',
          '--launch-resources',str(paths['launch_resources'])]
    specs={}
    for profile,cpu in zip(('r2','r4'),cpus):
        specs[profile]=dict(case_id=admission['case_id']+'-'+profile,tool='native-functional-LLM-L1-paired-control',
            input_kind='COMPLETE_NATIVE_TILEGRAPH_SOURCE_PROGRAMS_AND_API_HISTORY',cpu=cpu,gpu=None,seconds=43200,rss_limit_bytes=16<<30,
            argv=argv+['--output',str(directory/profile)],environment=dict(env,TILEGEN_ADA_L1_PROFILE=profile),sources=source_pins)
    return directory,argv,env,specs


def save(path,value):
    with path.open('x') as stream:json.dump(value,stream,indent=2);stream.write('\n')


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--root',type=Path,required=True);ap.add_argument('--admission',type=Path,required=True)
    ap.add_argument('--cpu-r2',type=int,required=True);ap.add_argument('--cpu-r4',type=int,required=True);a=ap.parse_args()
    root=a.root.resolve();need(root==REMOTE_ROOT,'exact task-owned remote root required')
    admission_path=a.admission.resolve();admission=json.loads(admission_path.read_text())
    paths,sources,actual=validate_admission(admission)
    producer=root/'repo/llm/executor-r1/whole_stream.py'
    allpins=merge_pins(sources+binary_evidence()+[pin(producer),pin(Path(__file__)),pin(admission_path),pin(paths['launch_resources'])])
    directory,argv,env,specs=build_specs(root,admission,paths,(a.cpu_r2,a.cpu_r4),allpins)
    directory.parent.mkdir(exist_ok=True)
    for receipt in directory.parent.glob('*/prepare-result.json'):
        other=json.loads(receipt.read_text())
        need(other['decode_steps']!=admission['decode_steps'],'one admitted case per decode length')
    directory.mkdir(exist_ok=False)
    start=time.monotonic()
    # The parent run_job owns the single-CPU lease. Preflight never launches the cache binary.
    inherited={k:v for k,v in os.environ.items() if not k.startswith('TILEGEN_')}
    command=argv+['--output',str(directory/'preflight'),'--preflight-only']
    proc=subprocess.run(command,env=dict(inherited,**env),capture_output=True,text=True)
    (directory/'preflight.log').write_text(proc.stdout+proc.stderr)
    proc.check_returncode()
    state=json.loads((directory/'preflight/status.json').read_text())
    need(state['status']=='PASS_COMPLETE_NATIVE_INPUT_PREFLIGHT','native preflight status')
    got=state['input_preflight']
    need(got['kernel_count']==actual['kernels'] and got['phases']==phases(admission['decode_steps']), 'native preflight count/phase mismatch')
    for row in allpins:checked(row)
    runtime_pins=[checked(row) for row in state.get('fast_source_runtime_pins',[])]
    allpins=merge_pins(allpins+runtime_pins)
    spec_pins=[]
    for profile,spec in specs.items():
        spec['sources']=allpins
        target=directory/(profile+'-spec.json');save(target,spec)
        spec_pins.append(dict(profile=profile,cpu=spec['cpu'],spec=pin(target),output=str(directory/profile),job_directory=str(directory/(profile+'-job'))))
    receipt=dict(schema='P128_PAIRED_PREPARATION_V1',status='PASS_P128_PAIRED_PREFLIGHT_NOT_EXECUTED',case_id=admission['case_id'],model_key='qwen25_1p5b',prefill_length=128,decode_steps=admission['decode_steps'],expected_counts=actual,
        input_contract=json.loads(paths['graph'].read_text())['input_contract'],admission=pin(admission_path),graph=pin(paths['graph']),launch_resources=pin(paths['launch_resources']),producer=pin(producer),binary=pin(BINARY),source_pin_count=len(allpins),specs=spec_pins,
        preflight_wall_minutes=(time.monotonic()-start)/60,compiled=False,launched=False,GPU_executed=False,
        lease_scope='outer shared run_job must acquire one CPU per spec; this preparer does not acquire leases',NCU_status='NOT_REQUIRED_FOR_SOURCE_PREFLIGHT')
    save(directory/'prepare-result.json',receipt)
    print(json.dumps({k:receipt[k] for k in ('status','case_id','decode_steps','expected_counts','compiled','launched','preflight_wall_minutes')}))

if __name__=='__main__':main()
