"""Export aggregate traffic, cache residency, timing and identities only."""
import hashlib
import json
from pathlib import Path

ROOT=Path('/home/xmu/nvidiagds/codex-runs/gtsim-ada-sector32-20260922-r1')
CASES=('p32-old128','p32-sector32','p128-sector32','p128-phase-drain')

def read(p):return json.loads(p.read_text()) if p.is_file() else None

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None

def owners(value):
    if not value:return None
    out={'scope':value['scope']}
    for key in ('writeback_flows','resident_start','resident_end','writebacks_cumulative','resident_dirty_carry'):
        if key not in value:continue
        groups={}
        for row in value[key]:
            dims={k:row[k] for k in ('first_writer_phase','last_writer_phase','trigger_phase','reason') if k in row}
            ident=tuple(sorted(dims.items())); dst=groups.setdefault(ident,dict(dims))
            for k in ('dirty_bytes','write_bytes','enabled_write_byte_coverage','masked_writeback_requests','writeback_merge_read_bytes'):
                if k in row:dst[k]=dst.get(k,0)+row[k]
        out[key]=list(groups.values())
    return out

def phase(row):
    out={k:v for k,v in row.items() if isinstance(v,(int,float)) or k=='phase'}
    if 'dirty_ownership'in row:out['dirty_ownership']=owners(row['dirty_ownership'])
    return out

def main():
    result={'schema':'SECTOR32_SAFE_AGGREGATES_V1','root':str(ROOT),'cases':[]}
    preparation=read(ROOT/'prepare-result.json')
    if preparation:
        result['preparation_status']=preparation['status'];result['binary_sha256']=preparation['build']['binary']['sha256']
    for name in CASES:
        out=ROOT/'cases'/name/'result';job=out.parent/'job';r={'case':name,'status':'NOT_STARTED'}
        spec=read(out.parent/'spec.json')
        if spec:r.update(cpu=spec['cpu'],data_policy=spec['environment']['TILEGEN_L2_DATA_POLICY'],drain_policy=spec['argv'][spec['argv'].index('--drain-policy')+1])
        s=read(out/'status.json')
        if s:
            for k in ('status','error_type','wall_minutes','producer_CPU_minutes','child_CPU_minutes','executed_counts','source_stream','source_stream_without_drain_interventions','measured_cache_history_intervened'):
                if k in s:r[k]=s[k]
            r['status_sha256']=sha(out/'status.json')
        for edge in ('start','finish'):
            j=read(job/('job-'+edge+'.json'))
            if j:
                r['job_'+edge]={k:j[k] for k in ('status','cpu','started_utc','finished_utc','CPU_minutes','wall_minutes') if k in j}
                if 'process'in j:r['cleanup_verified']=j['process']['cleanup']['owned_descendants_empty']
        p=out/'progress.jsonl'
        if p.is_file():
            for line in reversed(p.read_text().splitlines()):
                try:v=json.loads(line)
                except json.JSONDecodeError:continue
                r['progress']={k:v[k] for k in ('event','phase','counts','wall_seconds','producer_CPU_seconds') if k in v};break
        traffic=read(out/'phase-traffic.json')
        if traffic:
            r['phase_rows']=[phase(v) for v in traffic['phase_rows']]
            r['diagnostic_drain_rows']=[phase(v) for v in traffic['diagnostic_drain_rows']]
            r['configuration']=traffic['actual_cache_configuration'];r['phase_traffic_sha256']=sha(out/'phase-traffic.json')
        summary=read(out/'cache-summary.json')
        if summary:
            r['cache_CPU_minutes']=summary['CPU_minutes'];r['configuration']=summary['configuration']
            r['final_counters']={k:v for k,v in summary['snapshot'].items() if isinstance(v,(int,float)) and not any(x in k.lower() for x in ('address','covered_min','covered_max'))}
            r['final_dirty_ownership']=owners(summary.get('dirty_ownership'))
        if preparation:
            admission=next(x['admission'] for x in preparation['cases'] if x['case']==name)
            r['baseline_source_stream']=admission['baseline_source_stream']
            r['source_stream_matches_closed_baseline']=s is not None and s.get('source_stream_without_drain_interventions')=={k:admission['baseline_source_stream'][k] for k in ('bytes','records','sha256')}
            if name=='p32-old128' and summary:
                baseline=read(Path(admission['baseline_status']['path']).parent/'cache-summary.json')
                legacy={k:v for k,v in baseline['snapshot'].items() if isinstance(v,(int,float)) and not any(x in k.lower() for x in ('address','covered_min','covered_max'))}
                mismatches=[dict(counter=k,expected=v,actual=summary['snapshot'].get(k)) for k,v in legacy.items() if summary['snapshot'].get(k)!=v]
                r['old128_regression']=dict(scope='all safe scalar legacy final counters including original source and postcache hashes',compared_counters=len(legacy),exact_match=not mismatches,mismatches=mismatches)
        result['cases'].append(r)
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
