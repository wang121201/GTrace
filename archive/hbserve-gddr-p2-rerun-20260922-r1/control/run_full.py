"""One owned cold full-Measured run. Default is saved preflight only; no compiler or alternative binary selection."""
import argparse
import importlib.util
import json
import os
import re
import sys
from contract import *
sys.dont_write_bytecode=True
PARENT=HERE.parent/'gddr-full-p2-r1'
ADMISSION=PARENT/'admission.json'

def host_gate(expected_sha):
    hp=HERE/'host-budget.json';h=read(hp)
    need(pin(hp)['sha256']==expected_sha and h['schema']=='GDDR_P2_SAME_INPUT_HOST_BUDGET_OVERRIDE_V1','fixed host budget manifest')
    need(h['original_admission']==pin(ADMISSION),'same original admission')
    need(h['original_budget']==dict(target_seconds=3600,max_seconds=5400,execution_deadline_seconds=5340,cleanup_reserve_seconds=60),'original budget provenance')
    need(h['effective_host_budget']==dict(target_seconds=3600,max_seconds=7200,execution_deadline_seconds=7140,cleanup_reserve_seconds=60),'only authorized host budget')
    need(h['model_changes']==[] and h['compile_executed'] is False and h['one_hour_target_claimed'] is False,'host-only change scope')
    need({q['path'] for q in h['controller_pins']}=={str(HERE/'run_full.py'),str(HERE/'contract.py')},'new controller source set')
    for q in h['controller_pins']:need(pin(q['path'])==q,'new host controller source changed')
    need(pin(HERE/'contract.py')['sha256']==pin(PARENT/'contract.py')['sha256'],'byteexact original validators')
    return dict(manifest=pin(hp),effective_host_budget=h['effective_host_budget'],original_budget=h['original_budget'],controller_pins=h['controller_pins'])

def preflight(expected_sha):
    ap=ADMISSION;a=read(ap)
    need(pin(ap)['sha256']==expected_sha and a['schema']=='ADA_COLD_MEASURED_BINARY_ADMISSION_V1','fixed admission identity')
    need(a['target_seconds']==3600 and a['hard_controller_budget_seconds']==5400 and a['execution_deadline_seconds']==5340,'fixed budget')
    need(a['no_recompile'] is True and a['full_measured'] is True and a['full_history'] is False,'fixed scope')
    for q in a['pins']:need(pin(q['path'])==q,'frozen input changed: '+q['path'])
    need(a['binary'] in a['pins'] and pin(a['binary']['path'])==a['binary'],'fixed qualified binary')
    win=read(a['qualification']['path']);need(win['status']=='PASS_ACTUAL_NATIVE_ALIGNMENT_WINDOWS' and len(win['cases'])==5 and win['binary']==a['binary'],'five-window binary admission')
    p=read(a['plan']['path']);validate_plan(p,read(a['source_plan']['path']))
    need(pin(ap)['sha256']==expected_sha,'admission changed')
    return a,p

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--admission-sha',required=True);parser.add_argument('--run-name',default='run-r1')
    parser.add_argument('--host-manifest-sha',required=True)
    parser.add_argument('--execute',action='store_true');args=parser.parse_args()
    need(re.fullmatch(r'run-[A-Za-z0-9_-]{1,60}',args.run_name),'bounded fresh run directory')
    owned=R/'work/tilegen-hbf-traceoff-full-r1/owned_group.py'
    spec=importlib.util.spec_from_file_location('_owned_full_measured',owned);G=importlib.util.module_from_spec(spec);spec.loader.exec_module(G)
    started=G.raw();deadline=started+7200
    host=host_gate(args.host_manifest_sha)
    if not args.execute:
        a,p=preflight(args.admission_sha)
        print(json.dumps(dict(status='PREPARED_NOT_EXECUTED',binary=a['binary'],plan=a['plan'],counts=COUNTS,target_seconds=3600,hard_cleanup_budget_seconds=7200,execution_deadline_seconds=7140,host_budget_override=host)))
        return 0
    out=HERE/args.run_name;out.mkdir(exist_ok=False)
    rec=dict(schema='ADA_COLD_FULL_MEASURED_OWNED_V1',status='RUNNING',steps=[],GPU=False,full_trace_saved=False,
        full_measured=True,full_history=False,initial_cache_state='EMPTY_EXPLICIT_DIAGNOSTIC',
        target_seconds=3600,max_seconds=7200,execution_deadline_seconds=7140,cleanup_reserve_seconds=60,host_budget_override=host,
        compiler_executed=False,matched_warm_NCU_claim=False,hardware_accuracy_claimed=False)
    def save():
        rec['elapsed_seconds']=G.raw()-started
        tmp=out/'receipt.tmp';tmp.write_text(json.dumps(rec,indent=2,allow_nan=False)+'\n');tmp.replace(out/'receipt.json')
    with G.SignalLatch() as signals:
        try:
            save();a,p=preflight(args.admission_sha)
            rec.update(admission=pin(ADMISSION),binary=a['binary'],plan=a['plan'],pins=a['pins'],host_preflight_seconds=G.raw()-started);save()
            need(signals.first_signal is None and G.raw()<deadline-60,'cancel/deadline before launch')
            result_path=out/'result.json';argv=[a['binary']['path'],a['plan']['path'],str(result_path)]
            shim=('import json,os,sys;p=json.loads(sys.argv[1]);'
                  'a=os.open(p["stdout"],os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);'
                  'b=os.open(p["stderr"],os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);'
                  'os.dup2(a,1);os.dup2(b,2);os.close(a);os.close(b);os.execvp(p["argv"][0],p["argv"])')
            payload=dict(argv=argv,stdout=str(out/'native.stdout'),stderr=str(out/'native.stderr'))
            group=None;cleanup=None;reason=None;t=G.raw()
            try:
                group=G.OwnedGroup([sys.executable,'-B','-c',shim,json.dumps(payload)],origin_raw=t)
                rec['active_step']=dict(name='full-measured',controller_pid=os.getpid(),pid=group.child.pid,pgid=group.pgid,
                    argv=argv,deadline_raw=deadline-60);save()
                reason=group.wait_reason(deadline-60,signals)
            finally:
                if group is not None:cleanup=group.cleanup(term_grace=3,kill_grace=3)
                step=dict(name='full-measured',argv=argv,seconds=G.raw()-t,reason=reason,cleanup=cleanup,
                    returncode=group.child.returncode if group else None,signals=signals.receipt())
                rec['steps'].append(step);rec.pop('active_step',None);save()
            need(reason=='leader_exited' and step['returncode']==0 and cleanup['cleanup_complete'] and
                 cleanup['group_absent_after_cleanup'] and cleanup['direct_child_reaped'] and not cleanup['errors'] and
                 signals.first_signal is None,'owned run did not close cleanly')
            result=read(result_path);rec['validation']=validate_result(result,p)
            journal=[json.loads(line) for line in Path(str(result_path)+'.operations.jsonl').read_text().splitlines()]
            need(journal==result['rows'],'complete exact summary journal')
            progress=read(str(result_path)+'.progress.json')
            need(progress['completed_timeline_nodes']==progress['selected_timeline_nodes']==1173 and progress['kernels']==1138 and progress['APIs']==29,'final progress complete')
            # Original Fine Attention progress is retained, not a newly generated event trace.
            registry=read(p['dispatch']['path']); expected=[]
            for e in p['timeline']:
                if e['kind']=='native_kernel' and registry['entries'][str(e['native_launch_id'])]['owner']=='attention':
                    expected.extend((e['phase'].split('/')[-1],i) for i in range(1,9))
            stderr=[json.loads(line) for line in (out/'native.stderr').read_text().splitlines()]
            need(len(stderr)==len(expected)==768,'all original Attention progress records')
            for row,(role,cta) in zip(stderr,expected):
                need(set(row)=={'continuous_cycle','progress_completed_CTAs','role','total_CTAs'} and row['total_CTAs']==8 and row['role']==role and row['progress_completed_CTAs']==cta,'known bounded progress only')
            before={q['path']:q for q in a['pins']}
            def loader_pins(x):
                if isinstance(x,dict):
                    if {'path','bytes','sha256'}<=x.keys():need(before.get(x['path'])==x,'runtime loader opened unsealed input')
                    else:
                        for v in x.values():loader_pins(v)
                elif isinstance(x,list):
                    for v in x:loader_pins(v)
            loader_pins(result['loader'])
            for q in a['pins']:need(pin(q['path'])==q,'input/code/binary changed during run')
            need(pin(ADMISSION)==rec['admission'],'admission changed')
            need(host_gate(args.host_manifest_sha)==host,'new host controller/manifest changed during run')
            need(signals.first_signal is None and G.raw()<deadline,'closed within total budget')
            rec.update(status='PASS_CLOSED_COMPLETE_MEASURED_COLD_GDDR_P2',sources_unchanged=True,
                target_3600s_met=G.raw()-started<=3600,native_host_execution_seconds=result['host_execution_seconds'])
        except BaseException as e:
            rec.update(status='FAIL_COLD_FULL_MEASURED_PRESERVED',error=type(e).__name__+': '+str(e))
        finally:
            rec['signals']=signals.receipt()
            if signals.first_signal is not None:rec['status']='FAIL_CANCELLED_COLD_FULL_MEASURED_PRESERVED'
            rec['outputs']=[pin(q) for q in sorted(out.iterdir()) if q.is_file() and q.name not in ('receipt.json','receipt.tmp')]
            save()
    print(json.dumps({k:rec[k] for k in ('status','elapsed_seconds','target_3600s_met','error') if k in rec}))
    return 0 if rec['status'].startswith('PASS_') else 1
if __name__=='__main__':raise SystemExit(main())
