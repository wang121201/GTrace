"""Read only this Prefill/D2 task and export whitelisted scalar aggregates, never traces."""
import argparse
import collections
import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import importlib.util

_retry_spec=importlib.util.spec_from_file_location('prefill_retry_chain',Path(__file__).with_name('prefill_retry_chain.py'))
retry_chain=importlib.util.module_from_spec(_retry_spec)
_retry_spec.loader.exec_module(retry_chain)

REMOTE_ROOT=Path('/home/xmu/nvidiagds/codex-runs/gtsim-ada-r4-prefill-20260922-r1')
PREFILL_LENGTHS=(64,256,512)
COUNTERS=('DRAM_read_bytes','DRAM_write_bytes','DRAM_read_requests','DRAM_write_requests',
 'source_read_effect_bytes','source_write_effect_bytes','source_read_bytes','source_write_bytes',
 'source_memory_instructions','source_ranges','source_warp_events','source_global_to_shared_events','source_atomic_RMW_events',
 'source_kernel_payload_bytes_once','source_API_payload_bytes','API_ranges','API_128B_chunks','kernel_boundaries','API_boundaries_without_cache_flush',
 'age_writeback_bytes','capacity_eviction_writeback_bytes','age_writeback_sectors','L2_forwarded_access_sequence','L2_hits','L2_clean_evictions',
 'L1_read_hits','L1_read_misses','L1_pre_reads','L1_pre_writes','L1_bypass','L1_evictions',
 'L1_read_sector_requests','L1_read_sector_hits','L1_read_sector_misses','L1_write_sector_requests','L1_write_sector_hits','L1_write_sector_misses',
 'L1_forwarded_read_sector_requests','L1_forwarded_write_sector_requests','L1_bypassed_read_sector_requests','L1_bypassed_write_sector_requests',
 'dirty_sector_creations','evicted_dirty_sectors','resident_dirty_lines','resident_dirty_sectors','dirty_tail_bytes',
 'source_effect_projection_fnv1a64','source_projection_fnv1a64','postcache_record_fnv1a64')
COUNTS=('allocation_metadata_no_cache_flush','source_global_instructions','kernels','memory_APIs','native_CPP_GEMM_programs',
 'native_CPP_GEMV_programs','native_CPP_PREFILL_programs','native_CPP_LLAMA_P32_PREFILL_programs','source_control_events',
 'explicit_serial_splitK_programs','native_CPP_SPLITK_programs','source_EL_events_modeled_normal_priority','excluded_observer_only_operations')
STATUSES={'NOT_STARTED','PREFLIGHT_RUNNING','PASS_COMPLETE_NATIVE_INPUT_PREFLIGHT','RUNNING_COMPLETE_NATIVE_GRAPH_CACHE',
 'PASS_COMPLETE_NATIVE_GRAPH_CACHE_EXECUTION','FAIL_NATIVE_GRAPH_EXECUTION','RUNNING','PASS_PROCESS_ONLY','FAIL_PROCESS','FAIL_TIMEOUT',
 'PASS_STREAMED_SOURCE_FUNCTIONAL_CACHE_RUN','PASS_PREFILL_R4_PREFLIGHT_NOT_EXECUTED'}


def number(v):
    return type(v) in (int,float) and math.isfinite(v)


def numeric(source,keys):
    return {k:source[k] for k in keys if k in source and number(source[k])}


def safe_status(value):
    return value if value in STATUSES else 'OTHER_STATE_NOT_EXPORTED'


def sha(path):
    if not path.is_file():return None
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1<<20),b''):h.update(b)
    return h.hexdigest()


def load(path):
    if not path.is_file():return None
    try:return json.loads(path.read_text())
    except json.JSONDecodeError:return None


def records(path):
    if path.is_file():
        with path.open() as f:
            for line in f:
                try:yield json.loads(line)
                except json.JSONDecodeError:continue


def phase_ok(label,decode):
    return label in [s+'/'+p for s in ('Warmup','Measured') for p in ['Prefill']+['Decode'+str(i) for i in range(1,decode+1)]]+['Measured/Full']


def configuration(source):
    result={}
    l1=source.get('L1',{});l2=source.get('L2',{})
    result['L1']=numeric(l1,('bytes_per_SM','SMs','sets','ways','line_bytes','validity_bytes','shared_carveout_bytes'))
    for k,allowed in {'profile':{'legacy32','r2','r4'},'replacement':{'LRU','FIFO','CLOCK'},'hash':{'LINEAR_GLOBAL','ALLOCATION_RELATIVE_HASH2'},
        'persistence':{'KERNEL_FLUSH'},'shared_carveout_origin':{'not_applied_legacy32','bootstrap_before_first_kernel_no_shared_claim','assumed_explicit_environment_not_observed','observed_shared_bytes_explicit_caller_witness'},
        'shared_capacity_qualification':{'legacy32_not_adaptive','uncalibrated_extrapolation_8_16KiB','original_serial_read_calibrated_bin_extrapolated_to_LLM'}}.items():
        if l1.get(k) in allowed:result['L1'][k]=l1[k]
    for k in ('store_bypass','write_allocate','shared_is_per_CTA_dynamic_bytes'):
        if type(l1.get(k)) is bool:result['L1'][k]=l1[k]
    result['L2']=numeric(l2,('bytes','read_fill_RFO_bytes','writeback_request_bytes','EF_hit_numerator','EF_hit_denominator','dirty_age_accesses'))
    if l2.get('geometry')=='PAPER_ADA_L2_V1_20x1024x16':result['L2']['geometry']=l2['geometry']
    for k in ('skip_store_RFO',):
        if type(l2.get(k)) is bool:result['L2'][k]=l2[k]
    if type(source.get('end_flush')) is bool:result['end_flush']=source['end_flush']
    return result


def ncu_reference(root,case_id,prefill):
    try:retry_chain.retry_number(case_id,prefill)
    except ValueError:return dict(status='INPUT_CONTRACT_MISMATCH',ROI_rows=None)
    directory=root/'cases'/case_id;decode=2
    path=root/'references'/retry_chain.logical_id(prefill)/'ncu-result.json';x=load(path)
    if not x:return dict(status='NOT_READY',ROI_rows=None)
    identity=dict(source_sha256=sha(path))
    preparation=load(directory/'prepare-result.json') or {}
    expected=preparation.get('input_contract')
    if not isinstance(expected,dict):return dict(status='PAIRING_NOT_READY',ROI_rows=None,**identity)
    contract=x.get('input_contract',{})
    finite=(contract.get('model_key'),contract.get('prefill_length'),contract.get('decode_steps'),contract.get('batch_size'),contract.get('dtype'),contract.get('warmup_runs'))
    if finite!=('qwen25_1p5b',prefill,2,1,'bfloat16',1) or contract!=expected:
        return dict(status='INPUT_CONTRACT_MISMATCH',ROI_rows=None,**identity)
    if x.get('metadata_actual_controls_compared') is not True or x.get('independent_ROIs_are_additive') is not False or not str(x.get('status','')).startswith('PASS_CLOSED_RAW_NCU_'):
        return dict(status='REFERENCE_NOT_CLOSED_OR_UNPAIRED',ROI_rows=None,**identity)
    rows=[]
    raw_rows=x.get('ROI_rows',[])
    if not isinstance(raw_rows,list) or sorted(r.get('roi','') for r in raw_rows)!=sorted(('full','Prefill','D1','D2')):
        return dict(status='REFERENCE_ROI_SET_INCOMPLETE',ROI_rows=None,**identity)
    for r in raw_rows:
        roi=r.get('roi')
        if roi not in ['full','Prefill','DecodeAll']+['D'+str(i) for i in range(1,decode+1)]:continue
        nested=r.get('ncu',{})
        if not isinstance(nested,dict):return dict(status='REFERENCE_SCHEMA_MISMATCH',ROI_rows=None,**identity)
        fields=('n','dram_read_bytes','dram_write_bytes','CPU_minutes','wall_minutes','NCU_replay_passes','observed_host_replay_count','gpu_duration_ns')
        values={}
        for k in fields:
            if k in r and k in nested and r[k]!=nested[k]:return dict(status='CONFLICTING_NCU_VALUES',ROI_rows=None,**identity)
            if k in nested:values[k]=nested[k]
            elif k in r:values[k]=r[k]
        out=dict(roi=roi,**numeric(values,fields[:-1]))
        if any(not number(out.get(k)) or out[k]<0 for k in ('dram_read_bytes','dram_write_bytes')) or out.get('n')!=1:
            return dict(status='REFERENCE_METRIC_SCHEMA_MISMATCH',ROI_rows=None,**identity)
        t=values.get('gpu_duration_ns')
        if isinstance(t,str) and re.fullmatch('[0-9]+',t):t=int(t)
        out['gpu_duration_ns']=t if number(t) else None
        rows.append(out)
    return dict(status='REFERENCE_PRESENT_NOT_SIMULATOR_ACCEPTANCE',ROI_rows=rows,**identity,
                metadata_actual_controls_compared=True,complete_input_contract_equal=True,independent_ROIs_are_additive=False)


def aggregate_variant(root,directory,case_id,prefill):
    decode=2;profile='r4'
    out=directory/profile;job=directory/(profile+'-job')
    row=dict(case_id=case_id,model_key='qwen25_1p5b',prefill_length=prefill,decode_steps=2,profile='r4',status='NOT_STARTED',completed_phases=[],NCU=ncu_reference(root,case_id,prefill))
    preparation=load(directory/'prepare-result.json')
    row['preparation']=None
    if preparation:
        ready=dict(status=safe_status(preparation.get('status')),expected_counts=numeric(preparation.get('expected_counts',{}),('kernels','memory_APIs','allocation_observations','phases')),
                   **numeric(preparation,('source_pin_count','preflight_wall_minutes')))
        for field in ('graph','launch_resources','binary','runner_evidence'):
            digest=preparation.get(field,{}).get('sha256')
            ready[field+'_sha256']=digest if isinstance(digest,str) and re.fullmatch('[0-9a-f]{64}',digest) else None
        row['preparation']=ready
    state=load(out/'status.json')
    if state:
        row.update(numeric(state,('producer_CPU_minutes','child_CPU_minutes','wall_minutes')));row['status']=safe_status(state.get('status'))
        row['executed_counts']=numeric(state.get('executed_counts',{}),COUNTS)
        stream=state.get('source_stream',{});row['source_stream']=numeric(stream,('bytes','records'))
        if isinstance(stream.get('sha256'),str) and re.fullmatch('[0-9a-f]{64}',stream['sha256']):row['source_stream']['sha256']=stream['sha256']
        row['state_sha256']=sha(out/'status.json')
        err=state.get('error_type');row['error_type']=err if err in {'ValueError','RuntimeError','AssertionError','KeyError','FileNotFoundError','BrokenPipeError','CalledProcessError','TimeoutExpired'} else None
    for suffix in ('start','finish'):
        j=load(job/('job-'+suffix+'.json'))
        if j:
            v=numeric(j,('cpu','CPU_minutes','wall_minutes'));v['status']=safe_status(j.get('status'))
            for k in ('started_utc','finished_utc'):
                if isinstance(j.get(k),str) and re.fullmatch(r'[0-9T:.+Z-]+',j[k]):v[k]=j[k]
            row['job_'+suffix]=v
            clean=j.get('process',{}).get('cleanup',{}).get('owned_descendants_empty')
            if type(clean) is bool:row['cleanup_verified']=clean
    latest=None
    for p in records(out/'progress.jsonl'):latest=p
    if latest:
        p=numeric(latest,('source_event_ordinal','wall_seconds','producer_CPU_seconds'));p['counts']=numeric(latest.get('counts',{}),COUNTS)
        if latest.get('event') in ('node_completed','phase_begin','phase_end'):p['event']=latest['event']
        p['phase']=latest['phase'] if phase_ok(latest.get('phase'),decode) else 'UNSCOPED_HISTORY'
        row['progress']=p
    boundaries={};groups=collections.Counter()
    for s in records(out/'cache-snapshots.jsonl'):
        if s.get('type')=='snapshot':
            label=s.get('label','');base=label.rsplit('/',1)[0]
            if phase_ok(base,decode) and label.endswith(('/begin','/end')):boundaries[label]=s
        elif s.get('type')=='kernel_L1_configuration':
            c=configuration({'L1':s.get('L1',{})})['L1']
            groups[json.dumps(c,sort_keys=True)]+=1
    row['L1_configuration_groups']=[dict(configuration=json.loads(k),kernel_count=v) for k,v in groups.items()]
    for label,after in boundaries.items():
        if not label.endswith('/end'):continue
        before=boundaries.get(label[:-4]+'/begin')
        if before is None:continue
        b,e=before['cumulative'],after['cumulative'];d=dict(phase=label[:-4])
        for k in COUNTERS:
            if 'fnv' not in k and k not in ('dirty_tail_bytes','resident_dirty_lines','resident_dirty_sectors') and number(b.get(k)) and number(e.get(k)):d[k]=e[k]-b[k]
        for k,v in [('dirty_start_bytes',b.get('dirty_tail_bytes')),('dirty_end_bytes',e.get('dirty_tail_bytes'))]:
            if number(v):d[k]=v
        t0,t1=before.get('CPU_minutes_since_run_start'),after.get('CPU_minutes_since_run_start')
        d['cache_CPU_minutes']=t1-t0 if number(t0) and number(t1) else None
        d['counter_deltas_nonnegative']=all(v>=0 for k,v in d.items() if number(v))
        row['completed_phases'].append(d)
    summary=load(out/'cache-summary.json')
    if summary:
        row['summary_sha256']=sha(out/'cache-summary.json');row['configuration']=configuration(summary.get('configuration',{}))
        row['cache_CPU_minutes']=summary.get('CPU_minutes') if number(summary.get('CPU_minutes')) else None
        row['cache_wall_minutes']=summary.get('wall_minutes') if number(summary.get('wall_minutes')) else None
        row['final_counters']=numeric(summary.get('snapshot',{}),COUNTERS)
    return row


def aggregate(root):
    selected=retry_chain.selected_preparations(root)
    known={p:(path.parent,value['case_id']) for p,(path,value,depth) in selected.items()}
    cases=[]
    for prefill in PREFILL_LENGTHS:
        default_id='qwen-p'+str(prefill)+'d2'
        directory,case_id=known.get(prefill,(root/'cases'/default_id,default_id))
        row=aggregate_variant(root,directory,case_id,prefill)
        row['logical_case_id']=default_id
        row['validated_failed_retry_count']=selected[prefill][2] if prefill in selected else 0
        cases.append(row)
    return dict(schema='PREFILL_R4_LLM_SAFE_AGGREGATES_V1',generated_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                scope='safe aggregate snapshot; new P64/P256/P512 only; reused P32/P128 reported separately; no raw addresses/symbols; not full acceptance',cases=cases)


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--root',type=Path,required=True);a=ap.parse_args()
    if a.root.resolve()!=REMOTE_ROOT:raise ValueError('exact task-owned remote root required')
    print(json.dumps(aggregate(a.root.resolve()),indent=2))

if __name__=='__main__':main()
