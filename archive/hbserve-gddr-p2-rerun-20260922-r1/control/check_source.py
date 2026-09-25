from pathlib import Path
import ast,hashlib,json,subprocess,sys,time
D=Path(__file__).resolve().parent;P=D.parent/'gddr-full-p2-r1'
def pin(p):
 b=p.read_bytes();return dict(path=str(p),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
a=json.loads((P/'admission.json').read_text());h=pin(D/'host-budget.json');checks=[]
def need(v,name):
 if not v:raise ValueError(name)
 checks.append(name)
need((P/'contract.py').read_bytes()==(D/'contract.py').read_bytes(),'all original plan/result validators byteexact')
old=(P/'run_full.py').read_text();new=(D/'run_full.py').read_text();ast.parse(new)
# Exact native-through-closeout body except fixed admission path and extra host-source guard.
start="            result_path=out/'result.json'";end="            need(signals.first_signal is None and G.raw()<deadline,'closed within total budget')"
before=old[old.index(start):old.index(end)];after=new[new.index(start):new.index(end)]
after=after.replace('pin(ADMISSION)',"pin(HERE/'admission.json')").replace("            need(host_gate(args.host_manifest_sha)==host,'new host controller/manifest changed during run')\n",'')
need(before==after,'native argv, owned cleanup, complete work and final source validation byteexact')
need("started=G.raw();deadline=started+7200"in new and 'target_seconds=3600,max_seconds=7200,execution_deadline_seconds=7140,cleanup_reserve_seconds=60,host_budget_override=host,'in new,'actual budget equals receipt budget; target unchanged')
need("reason=group.wait_reason(deadline-60,signals)"in new,'7140 execution deadline, 60 reserve')
need(not(D/'run-r1').exists(),'no native run directory created')
cmd=[sys.executable,'-B',str(D/'run_full.py'),'--admission-sha',pin(P/'admission.json')['sha256'],'--host-manifest-sha',h['sha256']]
t=time.monotonic();p=subprocess.run(cmd,capture_output=True,text=True,timeout=30);elapsed=time.monotonic()-t
need(p.returncode==0 and not p.stderr,'actual preflight exit0 with no stderr');out=json.loads(p.stdout)
need(out['status']=='PREPARED_NOT_EXECUTED'and out['binary']==a['binary']and out['plan']==a['plan'],'same fixture and plan; no execution')
need(out['target_seconds']==3600 and out['hard_cleanup_budget_seconds']==7200 and out['execution_deadline_seconds']==7140,'preflight declares exact host budget')
need(not(D/'run-r1').exists(),'preflight never creates runtime output')
q=subprocess.run(cmd[:-1]+['0'*64],capture_output=True,text=True,timeout=5)
need(q.returncode!=0 and 'fixed host budget manifest'in q.stderr,'wrong host manifest SHA rejected before launch')
(D/'preflight.json').write_text(json.dumps(out,indent=2)+'\n')
(D/'source-checks.json').write_text(json.dumps(dict(status='PASS_SOURCE_AND_SAVED_PREFLIGHT_ONLY',checks=checks,elapsed_preflight_seconds=elapsed,admission_pin_count=len(a['pins']),native_executed=False,compiler_executed=False,preflight_argv=cmd,pins=[pin(D/'run_full.py'),pin(D/'contract.py'),h,pin(P/'admission.json')]),indent=2)+'\n')
print(json.dumps(dict(status='PASS_SOURCE_AND_SAVED_PREFLIGHT_ONLY',checks=len(checks),elapsed_preflight_seconds=elapsed,admission_pins=len(a['pins']),manifest=h)))
