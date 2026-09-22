"""Prepare, but never launch, one admitted Prefill/D2 case with its qualified XMU binary."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

REMOTE_ROOT = Path('/home/xmu/nvidiagds/codex-runs/gtsim-ada-r4-prefill-20260922-r1')
PREFILL_LENGTHS = (64, 256, 512)
KEY_BINARY_SOURCES = ('llm/executor-r1/main.cpp', 'llm/executor-r1/runner.h',
    'llm/executor-r1/candidate_direct_cache.h', 'llm/executor-r1/llm_l1_adapter.h',
    'llm/executor-r1/frozen_cache_geometry.h', 'llm/executor-r1/ef_hit_throttle.h',
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
    need(value['schema'] == 'ADMITTED_PREFILL_CASE_V1', 'admission schema')
    need(isinstance(value['case_id'], str) and re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}', value['case_id']), 'safe case ID')
    need(value['model_key'] == 'qwen25_1p5b' and type(value['prefill_length']) is int and value['prefill_length'] in PREFILL_LENGTHS, 'finite Qwen Prefill workload')
    d = value['decode_steps']
    need(type(d) is int and d == 2, 'fixed two decode steps')
    paths = {k:absolute(value[k]) for k in ('graph','registry','runtime','native_tree','support_tree','launch_resources','runner')}
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


def binary_evidence(admission, paths):
    """Read explicit build/shape qualification, never infer support from filename."""
    witness_pin=checked(admission['runner_evidence'])
    witness=json.loads(Path(witness_pin['path']).read_text())
    need(witness['schema']=='PREFILL_NATIVE_RUNNER_EVIDENCE_V1' and witness['status']=='PASS_BUILD_AND_SOURCE_CASE_VALIDATION', 'qualified runner evidence required')
    binary=checked(witness['binary'])
    need(binary['path']==str(paths['runner']), 'admitted runner differs from binary evidence')
    with paths['runner'].open('rb') as stream:
        need(stream.read(4)==b'\x7fELF', 'Linux ELF runner required')
    sources=merge_pins([checked(row) for row in witness['sources']])
    need(sources and witness.get('complete_compilation_dependencies') is True, 'complete compilation dependencies required')
    for name in KEY_BINARY_SOURCES:
        need(any(Path(row['path']).as_posix().endswith('/'+name) for row in sources), 'critical runner dependency missing: '+name)
    build_pin=checked(witness['build_receipt']);build=json.loads(Path(build_pin['path']).read_text())
    need(build['status']=='PASS_PROCESS_ONLY' and build['process']['cleanup']['owned_descendants_empty'] is True, 'runner build must close successfully')
    build_sources={row['path']:row for row in build['sources']}
    need(all(build_sources.get(row['path'])==row for row in sources), 'runner sources differ from actual build pins')
    case_pin=checked(witness['case_validation']);qualification=json.loads(Path(case_pin['path']).read_text())
    need(qualification['status']=='PASS_NATIVE_RUNNER_CASE_SOURCE_VALIDATION'
         and qualification['runner_sha256']==binary['sha256'], 'case qualification must bind actual binary')
    identity={k:admission[k] for k in ('model_key','prefill_length','decode_steps')}
    cases=[r for r in qualification['cases'] if all(r.get(k)==v for k,v in identity.items())]
    need(len(cases)==1 and cases[0].get('graph_sha256')==pin(paths['graph'])['sha256']
         and cases[0].get('source_qualified') is True and cases[0].get('typed_source_equivalent') is True,
         'current graph source/typed case qualification required')
    proof_pin=checked(cases[0]['source_validation_receipt'])
    proof=json.loads(Path(proof_pin['path']).read_text())
    need(proof['schema']=='PREFILL_NATIVE_CASE_VALIDATION_V1'
         and proof['status']=='PASS_CURRENT_RAW_ABI_SASS_AND_CPP_SOURCE_EQUIVALENCE', 'closed native source proof required')
    # The wrapper binds the proof to this graph and binary after the detailed
    # ABI/SASS/event checks; this preparer does not reproduce those algorithms.
    return merge_pins([witness_pin,binary,build_pin,case_pin,proof_pin]+sources)



def producer_path(root, admission):
    producer = absolute(admission.get('producer', str(root/'repo/llm/executor-r1/whole_stream.py')))
    need(producer.is_relative_to(root) and producer.name == 'whole_stream.py', 'task-owned frozen producer required')
    if 'producer' in admission:
        pinned = [row for row in admission['source_pins'] if row['path'] == str(producer)]
        need(len(pinned) == 1, 'explicit producer must be pinned in admission')
        checked(pinned[0])
    return producer


def build_specs(root, admission, paths, cpu, source_pins):
    need(type(cpu) is int and 0 <= cpu <= 15, 'single shared CPU ID in0..15')
    directory=root/'cases'/admission['case_id']
    producer=producer_path(root, admission)
    env=dict(TILEGEN_ADA_L1_PROFILE='r4',TILEGEN_NATIVE_TREE=str(paths['native_tree']),TILEGEN_NATIVE_SUPPORT_TREE=str(paths['support_tree']),
             TILEGEN_ADA_REQUIRE_OBSERVED='1',TILEGEN_L2_DIRTY_AGE_ACCESSES='64000000',TILEGEN_EF_HIT_RATE='288',
             PYTHONDONTWRITEBYTECODE='1',OMP_NUM_THREADS='1',MKL_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1')
    argv=['/usr/bin/python3','-B',str(producer),'--graph',str(paths['graph']),'--registry',str(paths['registry']),
          '--runtime',str(paths['runtime']),'--runner',str(paths['runner']),'--fast-gemv','--fast-prefill','--fast-prefill-sweep',
          '--launch-resources',str(paths['launch_resources'])]
    specs={}
    for profile in ('r4',):
        specs[profile]=dict(case_id=admission['case_id']+'-'+profile,tool='native-functional-LLM-r4-prefill-sweep',
            input_kind='COMPLETE_NATIVE_TILEGRAPH_SOURCE_PROGRAMS_AND_API_HISTORY',cpu=cpu,gpu=None,seconds=43200,rss_limit_bytes=16<<30,
            argv=argv+['--output',str(directory/profile)],environment=dict(env,TILEGEN_ADA_L1_PROFILE=profile),sources=source_pins)
    return directory,argv,env,specs


def save(path,value):
    with path.open('x') as stream:json.dump(value,stream,indent=2);stream.write('\n')


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--root',type=Path,required=True);ap.add_argument('--admission',type=Path,required=True)
    ap.add_argument('--cpu',type=int,required=True);a=ap.parse_args()
    root=a.root.resolve();need(root==REMOTE_ROOT,'exact task-owned remote root required')
    admission_path=a.admission.resolve();admission=json.loads(admission_path.read_text())
    paths,sources,actual=validate_admission(admission)
    producer=producer_path(root, admission)
    allpins=merge_pins(sources+binary_evidence(admission,paths)+[pin(producer),pin(Path(__file__)),pin(admission_path),pin(paths['launch_resources'])])
    directory,argv,env,specs=build_specs(root,admission,paths,a.cpu,allpins)
    directory.parent.mkdir(exist_ok=True)
    for receipt in directory.parent.glob('*/prepare-result.json'):
        other=json.loads(receipt.read_text())
        need(other['prefill_length']!=admission['prefill_length'],'one admitted case per prefill length')
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
    receipt=dict(schema='PREFILL_R4_PREPARATION_V1',status='PASS_PREFILL_R4_PREFLIGHT_NOT_EXECUTED',case_id=admission['case_id'],model_key='qwen25_1p5b',prefill_length=admission['prefill_length'],decode_steps=2,profile='r4',expected_counts=actual,
        input_contract=json.loads(paths['graph'].read_text())['input_contract'],admission=pin(admission_path),graph=pin(paths['graph']),launch_resources=pin(paths['launch_resources']),producer=pin(producer),binary=pin(paths['runner']),runner_evidence=admission['runner_evidence'],source_pin_count=len(allpins),specs=spec_pins,
        preflight_wall_minutes=(time.monotonic()-start)/60,compiled=False,launched=False,GPU_executed=False,
        lease_scope='outer shared run_job must acquire one CPU per spec; this preparer does not acquire leases',NCU_status='NOT_REQUIRED_FOR_SOURCE_PREFLIGHT')
    save(directory/'prepare-result.json',receipt)
    print(json.dumps({k:receipt[k] for k in ('status','case_id','prefill_length','decode_steps','expected_counts','compiled','launched','preflight_wall_minutes')}))

if __name__=='__main__':main()
