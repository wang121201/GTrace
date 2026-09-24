#!/usr/bin/env python3
"""Bounded CLI tests only. Does not expand or simulate a model graph."""
import copy,json,os,subprocess,tempfile,time
from pathlib import Path
HERE=Path(__file__).resolve().parent
BINARY=HERE/'build/source-cache-runner'
def allocation():
 return {'kind':'allocation_API_observation','submission_event':1,'return_event':2,'raw_before':{},'raw_return':{},'observation':{'action':'allocate','cuda_api':'cuMemAlloc_v2','base_u64':4096,'bytes':1048576,'allocation_generation':1,'async':False,'free_matched_observed_generation':False,'generation_semantics':'host_API_return_not_device_completion'}}
def event(op,address=4096,bypass=False):
 return {'type':'memory','event':{'operation':op,'domain':'global','cta_linear_id':0,'cta_warp_id':0,'pc':16,'width':4,'effective_mask':1,'global_effective_mask':1,'lane_addresses':{'0':address}},'policy':{'bypass_l1':bypass,'l2_priority':'normal','semantic':'activation'}}
def stream():
 rows=[{'type':'run_begin','schema':'TILEGEN_SOURCE_CACHE_STREAM_V1','sm_policy':'cta_mod_48'}, {'type':'begin_api','id':1,'phase':'initialize','api':'cuMemsetD8_v2','dma_model':'L2_COHERENT_FUNCTIONAL_128B_CHUNKS'}, {'type':'api_range','operation':'WRITE','address':16777216,'bytes':1}, {'type':'end_api','id':1}, {'type':'allocation_metadata','node':allocation()}]
 for i,shared in enumerate([8192,16384,32768,65536,102400]):
  rows += [{'type':'begin_kernel','id':i+1,'phase':'Measured/Decode'+str(i+1),'semantic':'test','grid':[1,1,1],'block':[32,1,1],'observed_shared_bytes':shared},event('READ'),event('GLOBAL_TO_SHARED',4128),event('ATOMIC_RMW',4160),{'type':'end_kernel','id':i+1}]
 rows += [{'type':'run_end'}];return rows
results=[]
def run(name,profile,rows,extra,ok=True):
 with tempfile.TemporaryDirectory() as td:
  p=Path(td);env=dict(os.environ,TILEGEN_ADA_L1_PROFILE=profile,TILEGEN_EF_HIT_RATE='288',TILEGEN_L2_DIRTY_AGE_ACCESSES='64000000');env.pop('TILEGEN_ADA_SHARED_BYTES',None);env.pop('TILEGEN_ADA_REQUIRE_OBSERVED',None);env.update(extra)
  command=[str(BINARY),'--input','-','--summary',str(p/'summary.json'),'--snapshots',str(p/'snapshots.jsonl')]
  start=time.monotonic();done=subprocess.run(command,input=''.join(json.dumps(r)+'\n'for r in rows),capture_output=True,text=True,env=env,timeout=30)
  assert (done.returncode==0)==ok,(name,done.stderr)
  result={'name':name,'returncode':done.returncode,'wall_seconds':time.monotonic()-start,'stderr':done.stderr}
  if ok:
   out=json.loads((p/'summary.json').read_text());snaps=[json.loads(l)for l in(p/'snapshots.jsonl').read_text().splitlines()];conf=[s['L1']for s in snaps if s['type']=='kernel_L1_configuration'];assert len(conf)==5
   assert out['snapshot']['kernel_boundaries']==5 and out['snapshot']['API_boundaries_without_cache_flush']==1
   assert out['no_end_flush'] and out['snapshot']['DRAM_write_bytes']==0 and out['snapshot']['dirty_tail_bytes']==64
   assert out['snapshot']['source_atomic_RMW_events']==5 and out['snapshot']['source_global_to_shared_events']==5
   assert out['snapshot']['L1_adapter_observation']['driver_allocates']==1
   if profile!='legacy32':
    assert [c['shared_carveout_bytes']for c in conf]==[8192,16384,32768,65536,102400]
    assert all(c['shared_carveout_origin']=='observed_shared_bytes_explicit_caller_witness'for c in conf)
    assert out['snapshot']['L1_read_sector_requests']==out['snapshot']['L1_read_sector_hits']+out['snapshot']['L1_read_sector_misses']
    assert out['snapshot']['L1_write_sector_requests']==out['snapshot']['L1_write_sector_hits']+out['snapshot']['L1_write_sector_misses']
   result.update(snapshot=out['snapshot'],configuration=out['configuration'],kernel_configurations=conf)
  results.append(result)
 return result
for profile in ['legacy32','r2','r4']:run(profile,profile,stream(),{'TILEGEN_ADA_REQUIRE_OBSERVED':'1'})
assert len({x['snapshot']['source_effect_projection_fnv1a64']for x in results})==1
bad=stream();del bad[5]['observed_shared_bytes'];run('missing_observed','r4',bad,{'TILEGEN_ADA_REQUIRE_OBSERVED':'1'},False)
bad=stream();bad[5]['observed_shared_bytes']=2**32+32768;run('overflow_observed','r4',bad,{'TILEGEN_ADA_REQUIRE_OBSERVED':'1'},False)
bad=stream();bad[6]=event('READ',16777216);run('unknown_allocation','r4',bad,{'TILEGEN_ADA_REQUIRE_OBSERVED':'1'},False)
run('mixed_assumed_observed','r4',stream(),{'TILEGEN_ADA_REQUIRE_OBSERVED':'1','TILEGEN_ADA_SHARED_BYTES':'32768'},False)
run('missing_profile_shared','r4',stream(),{},False)
(HERE/'cli-results.json').write_text(json.dumps({'status':'PASS_LLM_L1_RUNNER_CLI','tests':results},indent=2)+'\n')
print(json.dumps({'status':'PASS_LLM_L1_RUNNER_CLI','tests':len(results)}))
