#!/usr/bin/env python3
"""Bounded actual-main checks; synthetic addresses, never a model-traffic claim."""
import argparse, hashlib, json, os, pathlib, subprocess, tempfile, time

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--binary',required=True);ap.add_argument('--output',required=True);args=ap.parse_args()
    binary=pathlib.Path(args.binary).resolve();out=pathlib.Path(args.output);out.mkdir(parents=True,exist_ok=False)
    alloc={'kind':'allocation_API_observation','submission_event':1,'return_event':2,'raw_before':{},'raw_return':{},'observation':{'action':'allocate','cuda_api':'cuMemAlloc_v2','base_u64':4096,'bytes':1048576,'allocation_generation':1,'async':False,'generation_semantics':'host_API_return_not_device_completion'}}
    def begin(i,p):return {'type':'begin_kernel','id':i,'phase':p,'semantic':'module_'+p,'grid':[1,1,1],'block':[32,1,1],'observed_shared_bytes':32768}
    def mem(op,a,n):return {'type':'memory','event':{'domain':'global','operation':op,'cta_linear_id':0,'cta_warp_id':0,'pc':16,'width':n,'global_effective_mask':1,'effective_mask':1,'lane_addresses':{'0':a}},'policy':{'bypass_l1':op=='ATOMIC_RMW','l2_priority':'normal','semantic':'output'}}
    rows=[{'type':'run_begin','schema':'TILEGEN_SOURCE_CACHE_STREAM_V1','sm_policy':'cta_mod_48'},{'type':'allocation_metadata','node':alloc},begin(1,'Measured/Prefill'),mem('WRITE',4096,4),mem('ATOMIC_RMW',4224,4),{'type':'end_kernel','id':1},{'type':'snapshot','label':'Measured/Prefill/end'},begin(2,'Measured/Decode1'),mem('READ',4096,4),mem('WRITE',4097,1),{'type':'end_kernel','id':2},{'type':'snapshot','label':'Measured/Decode1/end'},{'type':'drain','phase':'Diagnostic/run-end','label':'diagnostic-drain'},{'type':'run_end'}]
    tests=[]
    def run(name,policy,commands,success=True):
        d=out/name;d.mkdir();ip=d/'input.jsonl';ip.write_text(''.join(json.dumps(x,separators=(',',':'))+'\n' for x in commands));env=dict(os.environ)
        env.pop('TILEGEN_ADA_SHARED_BYTES',None);env.update(TILEGEN_ADA_L1_PROFILE='r4',TILEGEN_ADA_REQUIRE_OBSERVED='1',TILEGEN_L2_DATA_POLICY=policy,TILEGEN_EF_HIT_RATE='288',TILEGEN_L2_DIRTY_AGE_ACCESSES='64000000',ASAN_OPTIONS='detect_leaks=0')
        start=time.monotonic();p=subprocess.run([str(binary),'--input',str(ip),'--summary',str(d/'summary.json'),'--snapshots',str(d/'snapshots.jsonl')],env=env,text=True,capture_output=True,timeout=45)
        (d/'stdout.txt').write_text(p.stdout);(d/'stderr.txt').write_text(p.stderr)
        assert (p.returncode==0)==success,(name,p.returncode,p.stderr)
        item={'name':name,'returncode':p.returncode,'wall_seconds':time.monotonic()-start,'expected_success':success,'stdout':str((d/'stdout.txt').resolve()),'stderr':str((d/'stderr.txt').resolve())};tests.append(item)
        if not success:return None
        s=json.loads((d/'summary.json').read_text());snaps=[json.loads(x) for x in (d/'snapshots.jsonl').read_text().splitlines()];item['CPU_minutes']=s['CPU_minutes'];return s,snaps
    sector,snaps=run('sector32','sector32',rows)
    s=sector['snapshot'];assert s['DRAM_read_bytes']==64 and s['DRAM_atomic_read_bytes']==32 and s['DRAM_read_merge_bytes']==32 and s['DRAM_load_fill_bytes']==0
    assert s['DRAM_write_bytes']==64 and s['drain_writeback_bytes']==64 and s['writeback_enabled_byte_coverage']==8 and s['masked_writeback_requests']==2
    natural=[x for x in snaps if x['type']=='snapshot'];assert all(x['cumulative']['DRAM_write_bytes']==0 and x['cumulative']['dirty_tail_bytes']==64 for x in natural)
    owners=sector['dirty_ownership']['writebacks_cumulative'];assert sum(x['write_bytes'] for x in owners)==64 and all(x['trigger_phase']=='Diagnostic/run-end' and x['reason']=='drain' for x in owners)
    old,_=run('old128','old128',rows);s=old['snapshot'];assert s['DRAM_read_bytes']==256 and s['DRAM_store_RFO_bytes']==128 and s['DRAM_atomic_RFO_bytes']==128 and s['DRAM_write_bytes']==64
    assert old['snapshot']['source_effect_projection_fnv1a64']==sector['snapshot']['source_effect_projection_fnv1a64']
    continuous,_=run('continuous-no-drain','sector32',[x for x in rows if x['type']!='drain']);assert continuous['snapshot']['DRAM_write_bytes']==0 and continuous['snapshot']['dirty_tail_bytes']==64
    run('invalid-policy','sector31',rows,False)
    run('nested-drain','sector32',rows[:4]+[rows[-2]],False)
    result={'status':'PASS_SECTOR32_ACTUAL_MAIN_BOUNDED_CLI','full_model_or_GPU_test':False,'binary':{'path':str(binary),'bytes':binary.stat().st_size,'sha256':hashlib.sha256(binary.read_bytes()).hexdigest()},'tests':tests,'source_requested_projection_equal_old128_sector32':True,'continuous_no_implicit_flush':True,'explicit_drain_separate_from_natural_phase':True}
    (out/'receipt.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'status':result['status'],'tests':len(tests),'receipt':str((out/'receipt.json').resolve())}))
if __name__=='__main__':main()
