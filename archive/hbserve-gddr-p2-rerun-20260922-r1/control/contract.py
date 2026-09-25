"""Saved input/output checks for one explicit cold complete Measured interval."""
from pathlib import Path
from collections import Counter
import hashlib
import json

HERE=Path(__file__).resolve().parent
D=HERE.parent
R=D.parents[1]
O=R/'work/tilegen-input-contract-r1/llama-p32-history-capture-r1'
SOURCE=D/'plans-r1/full-history-tuner-v1-not-admitted.json'
WINDOWS=D/'run-native-windows-r1/receipt.json'
COUNTS={'native_kernel':1138,'memory_api_submission':29,'epoch_begin':3,'epoch_end':3}
WORK={'CTAs':772512,'nodes':4712768255,'logical_read_bytes':79862407228,'logical_write_bytes':209577880}
API={'logical_read_bytes':84,'logical_write_bytes':548}

def need(ok,message):
    if not ok: raise ValueError(message)

def pin(path):
    p=Path(path); need(p.is_file() and not p.is_symlink(),'regular nonsymlink input: '+str(p))
    before=p.stat(); h=hashlib.sha256()
    with p.open('rb') as f:
        for chunk in iter(lambda:f.read(8<<20),b''): h.update(chunk)
    after=p.stat(); need((before.st_size,before.st_mtime_ns,before.st_ino)==(after.st_size,after.st_mtime_ns,after.st_ino),'file changed while hashing')
    return dict(path=str(p.resolve()),bytes=after.st_size,sha256=h.hexdigest())

def read(path): return json.loads(Path(path).read_text())
def write_new(path,obj):
    with Path(path).open('x') as f: json.dump(obj,f,indent=2,allow_nan=False); f.write('\n')

def selected(source):
    indices=[i for i,e in enumerate(source['timeline']) if 4<=e['epoch']<=6]
    need(indices==list(range(indices[0],indices[-1]+1)),'Measured is a contiguous source interval')
    return source['timeline'][indices[0]:indices[-1]+1]

def validate_plan(p,source):
    # Only the observed memory data rate is allowed to differ from the closed baseline.
    import copy
    baseline=read(D/'full-measured-r1/plan.json')
    expected=copy.deepcopy(baseline)
    cfg=D/'gddr-review-r1/observed-gpu0-p2-r2/rtx4000-ada-observed-p2.cfg'
    need(pin(cfg)['sha256']=='512a37d69f6e738e468e0146030bd94d784ece6cc46f6529ec4de019bd46fcdb','exact observed operating-rate cfg')
    expected['profile']['native_hbfsim_config_file']=str(cfg)
    expected['profile']['gddr6']['geometry']['pin_rate_Gbps']=17.1
    need(p==expected,'only the native cfg path and descriptive pin-rate may change')
    def cfg_values(path):
        return dict(line.strip().split('=',1) for line in Path(path).read_text().splitlines()
                    if '=' in line and not line.lstrip().startswith('#'))
    old_cfg=cfg_values(baseline['profile']['native_hbfsim_config_file']);new_cfg=cfg_values(cfg)
    need(set(old_cfg)==set(new_cfg),'same effective cfg keys')
    need({k:[old_cfg[k],new_cfg[k]] for k in old_cfg if old_cfg[k]!=new_cfg[k]}==
         {'hbm-pin-rate-gbps':['18.00','17.10']},'one effective GDDR parameter change')
    # Original source-slice and all workload checks below remain intact.
    source=copy.deepcopy(source);source['profile']=copy.deepcopy(p['profile'])
    need(p['schema']=='CURRENT_CONTINUOUS_HISTORY_EXECUTION_V1','schema')
    need(p['timeline']==selected(source),'exact complete continuous source slice')
    need(len(p['timeline'])==1173 and dict(Counter(e['kind'] for e in p['timeline']))==p['counts']==COUNTS,'all1173nodes')
    need([e['native_launch_id'] for e in p['timeline'] if e['kind']=='native_kernel']==list(range(1288,2426)),'all1138originalIDs')
    need(p['timeline'][0]['kind']=='epoch_begin' and p['timeline'][-1]['kind']=='epoch_end','complete boundaries')
    need(p['kernel_work']==WORK and p['API_work']==API,'complete work')
    for k in ('profile','service_address_map','history_source','dispatch','common_device_input','common_map_source','initialization_work'):
        need(p[k]==source[k],'source/config unchanged '+k)
    need(p['ada_alignment_profile']=='tuner-v1' and p['profile']['clock']['period_ps_numerator']==40000 and p['profile']['clock']['period_ps_denominator']==87,'fixed profile/clock')
    need(p['full_measured'] is True and p['full_history'] is False and p['complete_prefix_from_process_start'] is False,'explicit scope')
    need(p['initial_cache_state']=='EMPTY_EXPLICIT_DIAGNOSTIC' and p['no_end_dirty_flush'] is True and p['matched_NCU_ROI_available'] is False,'cold/no warm claim')
    reg=read(p['dispatch']['path']); total=Counter(); effects=Counter()
    for e in p['timeline']:
        if e['kind']=='native_kernel': total.update(reg['entries'][str(e['native_launch_id'])]['expected'])
        elif e['kind']=='memory_api_submission':
            for x in e['device_effect_ranges']: effects['logical_'+x['operation'].lower()+'_bytes']+=x['bytes']
    need(dict(total)==WORK and dict(effects)==API,'independent registry/API sum')
    return reg

def validate_result(result,plan):
    need(result['status']=='PASS_SELECTED_CONTINUOUS_CURRENT_HISTORY','native final status')
    need(result['counts']==COUNTS and result['kernels']==1138 and result['APIs']==29 and len(result['rows'])==1173,'actual all operations')
    need(result['CTAs']==WORK['CTAs'] and result['nodes']==WORK['nodes'],'actual all work')
    for label,expected in [('kernel_read',WORK['logical_read_bytes']),('kernel_write',WORK['logical_write_bytes']),('API_read',API['logical_read_bytes']),('API_write',API['logical_write_bytes'])]:
        need(result['logical_'+label+'_bytes']==expected,'actual logical '+label)
    need(result['full_measured_complete'] is True and result['full_history'] is False and result['complete_prefix_from_process_start'] is False,'actual scope')
    need(result['initial_cache_state']=='EMPTY_EXPLICIT_DIAGNOSTIC' and result['no_end_dirty_flush'] is True and result['full_trace_saved'] is False,'actual cold/no flush')
    need(result['hardware_accuracy_claimed'] is False and result['ada_alignment']['profile']=='tuner-v1' and result['ada_alignment']['core_MHz']==2175,'actual profile/limits')
    last=0
    for row,e in zip(result['rows'],plan['timeline']):
        need(row['kind']==e['kind'] and row['source_submission_event']==e['submission_event'] and row['phase']==e['phase'] and row['source_epoch']==e['epoch'],'exact operation identity')
        need(row['operation_start_cycle']==last and row['operation_end_cycle']>=last,'continuous clock'); last=row['operation_end_cycle']
        if e['kind']=='native_kernel': need(row['native_launch_id']==e['native_launch_id'],'actual native ID')
        if e['kind']=='memory_api_submission': need(row['source_effects_completed']==e['device_effect_ranges'] and row['accepted_line_requests']==row['completed_line_requests'],'complete API effects')
        if 'global_dirty' in row:
            d=row['global_dirty']; need(d['I']+d['C']==d['E']+d['F'],'per operation dirty')
    need(result['rows'][0]['dirty_cumulative']['resident_dirty_sectors']==0,'actual empty entry')
    d=result['dirty']; h=result['HBFSIM'];w=result['writer_attribution']
    need(d['dirty_sector_ledger_closed'] and d['writeback_byte_ledger_closed'] and d['dirty_sector_creations']==d['evicted_dirty_sectors']+d['resident_dirty_sectors'],'cold dirty conservation')
    need(all(d[k]==0 for k in ('outstanding_writeback_bytes','unadmitted_writeback_bytes','pending_dirty_lines','pending_dirty_sectors')),'dirty pending closed')
    need(h['accepted']==h['completed'] and h['reserved_bursts_at_end']==0 and h['native_enqueue_service_violations']==0 and h['no_trace_sink'] is True,'HBF closure')
    need(w['status']=='PASS_CLOSED_PASSIVE_ATTRIBUTION' and w['initial_inherited_dirty_sectors']==0 and w['last_observed_call']==1172 and w['writeback_bytes']==h['physical']['write_bytes'],'writer closure')
    need(h['physical']['write_bytes']==32*d['evicted_dirty_sectors'],'physical dirty bytes')
    return dict(counts=COUNTS,work=WORK,API_work=API,cycles=result['cycles'],physical=h['physical'],dirty=d,
                host_execution_seconds=result['host_execution_seconds'],hardware_accuracy_claimed=False)
