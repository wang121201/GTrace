from pathlib import Path
import hashlib,importlib.util,json,os,shlex,subprocess,sys,time
sys.dont_write_bytecode=True
D=Path(__file__).resolve().parent;R=D.parents[2];S=R/'work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot'; B=S/'build/shared-h288-r1';I=S/'source/work/tilegen-full-r1/core-native-copy-r2/include'
W=R/'work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-write-attribution-r1'
OUT=D/(sys.argv[sys.argv.index('--out')+1] if '--out' in sys.argv else 'run-r1')
def need(v,s):
 if not v:raise ValueError(s)
def read(p):return json.loads(Path(p).read_text())
def pin(p):
 p=Path(p).resolve();b=p.read_bytes();return dict(path=str(p),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
def write(p,d):Path(p).write_text(json.dumps(d,indent=2)+'\n')
def deps(p):
 t,items=Path(p).read_text().replace('\\\n',' ').split(':',1);return Path(t.strip()).resolve(),sorted({str(Path(i).resolve()) for i in shlex.split(items)})
def raw():return time.clock_gettime(time.CLOCK_MONOTONIC_RAW)
def worker():
 data=read(OUT/'commands.json');w=dict(status='RUNNING',steps=[]);start=raw()
 with (OUT/'version.stdout').open('xb') as o,(OUT/'version.stderr').open('xb') as e:r=subprocess.run([data['compiler'],'--version'],stdout=o,stderr=e)
 need(r.returncode==0 and (OUT/'version.stdout').read_text()==data['compiler_version'] and not (OUT/'version.stderr').stat().st_size,'frozen compiler exact')
 def launch(item):
  o=(OUT/(item['name']+'.stdout')).open('xb');e=(OUT/(item['name']+'.stderr')).open('xb');return subprocess.Popen(item['argv'],stdout=o,stderr=e),item,raw(),o,e
 def finish(x):
  p,i,t,o,e=x;o.close();e.close();w['steps'].append(dict(name=i['name'],argv=i['argv'],returncode=p.returncode,seconds=raw()-t,stdout=pin(OUT/(i['name']+'.stdout')),stderr=pin(OUT/(i['name']+'.stderr'))));write(OUT/'worker.json',w);return p.returncode==0
 pending=list(data['core']);active=[];failed=False
 while active or (pending and not failed):
  while pending and len(active)<2 and not failed:active.append(launch(pending.pop(0)))
  new=[]
  for x in active:
   if x[0].poll() is None:new.append(x)
   elif not finish(x):failed=True
  active=new
  if active:time.sleep(.02)
 if not failed:
  for item in data['tests']:
   x=launch(item);x[0].wait()
   if not finish(x):failed=True;break
 w.update(status='FAIL_PRESERVED' if failed else 'PASS_COMPILE_AND_COMPONENT',seconds=raw()-start);write(OUT/'worker.json',w);return int(failed)
def main():
 OUT.mkdir(exist_ok=False);owned=R/'work/tilegen-hbf-traceoff-full-r1/owned_group.py';spec=importlib.util.spec_from_file_location('_owned',owned);G=importlib.util.module_from_spec(spec);spec.loader.exec_module(G)
 start=G.raw();deadline=start+120;receipt=dict(schema='ADA_STRUCTURE_OWNED_CPU_V1',status='RUNNING',max_seconds=120,cleanup_reserve_seconds=8,jobs=2,GPU=False,full_model=False)
 def save():receipt['elapsed_seconds']=G.raw()-start;write(OUT/'receipt.json',receipt)
 save()
 try:
  build=read(B/'build-receipt.json');need(build['status']=='PASS_BUILD_ONLY_NO_SIMULATION' and build['sources_unchanged'],'original build closed')
  src={Path(__file__),owned,W/'overlay.json',W/'readiness.json',W/'writer_observer.h',W/'source-proof.json',W/'build-r1/receipt.json',B/'build-receipt.json'}
  for n in ('prepare.py','fixture.cpp','config.h','cache_geometry.h','ada_address_mapping.h','source.patch','overlay.json','source-proof.json','provenance.json','profile.json'):src.add(D/n)
  for p in read(D/'provenance.json')['source_pins']:need(pin(p['path'])==p,'provenance pin exact');src.add(Path(p['path']))
  for p in read(W/'readiness.json')['source_pins']:need(pin(p['path'])==p,'writer sealed source');src.add(Path(p['path']))
  proof=read(D/'source-proof.json');need(proof['inverse_byte_exact'] and proof['grouped_LRU_byte_exact'],'literal inverse required');inv=(D/'cache_geometry.h').read_text()
  for a,b in reversed(proof['literal_edits']):need(inv.count(b)==1,'patch inverse literal');inv=inv.replace(b,a)
  need(inv==(I/'cache_geometry.h').read_text(),'old header exact inverse')
  for key in ('original_geometry','candidate_geometry','mapping_source','mapping_copy','core_memory_byte_exact','HBF_backend_byte_exact','profile_original'):
   p=proof[key];need(pin(p['path'])==p,'source proof pin');src.add(Path(p['path']))
  pins={str((S/k).resolve()):v for k,v in build['source_pins'].items()}
  for p,h in pins.items():need(pin(p)['sha256']==h,'original compiled source unchanged');src.add(Path(p))
  objects_prior=read(W/'build-r1/receipt.json');need(objects_prior['status']=='PASS_OWNED_SIX_TU_ATTRIBUTION_BUILD_ONLY' and objects_prior['sources_unchanged'],'old object qualification')
  old_objs={x['original_object']['path']:x['original_object'] for x in read(W/'build-r1/dependency-proof.json')['TUs'] if 'original_object' in x};src.add(W/'build-r1/dependency-proof.json')
  dep_rows=[]
  for i in range(22):
   name=f'{i:02d}';dp=B/(name+'.d');target,items=deps(dp);need(target==B/(name+'.o'),'dependency target');geo=str(I/'cache_geometry.h') in items;writer=str(read(W/'source-proof.json')['original']['path']) in items;need(geo==(i<=6) and writer==(i<=6),'00..06 geometry/writer dependent,07..21 independent')
   for p in items:need(p in pins,'all original TU dependency pins')
   src.add(dp);row=dict(name=name,geometry=geo,writer=writer,dependencies=items,dependency_file=pin(dp),disposition='driver_needs_both_overlays' if i==0 else 'recompile_both_overlays' if i<=6 else 'reuse')
   if i:obj=B/(name+'.o');need(pin(obj)==old_objs[str(obj)],'original object pin unchanged');src.add(obj);row['original_object']=pin(obj)
   dep_rows.append(row)
  write(OUT/'dependency-proof.json',dict(status='PASS_SIX_CORE_DEPENDENT_FIFTEEN_HBF_INDEPENDENT',TUs=dep_rows))
  overlays=['-ivfsoverlay',str(D/'overlay.json'),'-ivfsoverlay',str(W/'overlay.json')];steps={x['name']:x for x in build['steps']};cmd=[]
  for i in range(1,7):
   name=f'{i:02d}';a=list(steps[name]['command']);need(a[:1+len(build['flags'])]==[build['compiler'],*build['flags']],'original flags exact');a[a.index('-MF')+1]=str(OUT/(name+'.d'));a[a.index('-o')+1]=str(OUT/(name+'.o'));at=a.index('-MMD');a[at:at]=overlays;cmd.append(dict(name=name,argv=a))
  coreobjs=[OUT/(f'{i:02d}.o') if i<=6 else B/(f'{i:02d}.o') for i in range(1,22)]
  tests=[];cfg=read(D/'profile.json')['native_hbfsim_config_file'];src.add(Path(cfg));src.add(R/'work/unified-cache-cosim-r1/decode-sector-l1-component-r1/serializers.inc')
  for name,candidate in [('baseline',False),('candidate',True)]:
   objs=coreobjs if candidate else [B/(f'{i:02d}.o') for i in range(1,22)]
   a=[build['compiler'],*build['flags'],*(overlays if candidate else ['-DORIGINAL']),'-MMD','-MF',str(OUT/(name+'.d')),str(D/'fixture.cpp'),*map(str,objs),'-lz','-o',str(OUT/name)]
   tests.extend([dict(name=name+'-compile',argv=a),dict(name=name+'-run',argv=[str(OUT/name),cfg])])
  write(OUT/'commands.json',dict(compiler=build['compiler'],compiler_version=build['compiler_version'],core=cmd,tests=tests,overlay_flags=overlays,flags=build['flags']))
  before=[pin(p) for p in sorted(src)];receipt.update(pins=before,commands=pin(OUT/'commands.json'),dependency_proof=pin(OUT/'dependency-proof.json'),overlay_flags=overlays);save();need(G.raw()<deadline-8,'admission budget')
  group=None;reason=None
  with G.SignalLatch() as signals:
   try:
    group=G.OwnedGroup([sys.executable,'-B',str(Path(__file__).resolve()),'--worker','--out',OUT.name],origin_raw=G.raw());reason=group.wait_reason(deadline-8,signals)
   finally:
    if group:receipt['cleanup']=group.cleanup(term_grace=3,kill_grace=3)
    receipt['signals']=signals.receipt()
  receipt.update(reason=reason,returncode=group.child.returncode);save();cl=receipt['cleanup'];need(reason=='leader_exited' and group.child.returncode==0 and cl['cleanup_complete'] and cl['group_absent_after_cleanup'] and cl['direct_child_reaped'] and not cl['errors'] and signals.first_signal is None,'actual owned closure')
  worker_data=read(OUT/'worker.json');need(worker_data['status']=='PASS_COMPILE_AND_COMPONENT' and len(worker_data['steps'])==10,'actual10steps')
  baseline=read(OUT/'baseline-run.stdout');candidate=read(OUT/'candidate-run.stdout');need(baseline['status']==candidate['status']=='PASS_ADA_STRUCTURE_COMPONENT' and baseline['baseline']==candidate['baseline'],'unmodified legacy q1/q8 actual whole JSON exact')
  for name in ('baseline','candidate'):need((OUT/(name+'-run.stderr')).stat().st_size==0,'empty runtime stderr')
  extra={str((D/'fixture.cpp').resolve()),str((D/'config.h').resolve()),str((I/'ada_address_mapping.h').resolve()),str((R/'work/unified-cache-cosim-r1/decode-sector-l1-component-r1/serializers.inc').resolve())};allowed=set(pins)|extra
  for name in ['01','02','03','04','05','06','baseline','candidate']:
   _,items=deps(OUT/(name+'.d'));need(set(items)<=allowed,'entire actual non-system include closure pinned')
   if name!='baseline':need(str(I/'cache_geometry.h') in items and str(I/'ada_address_mapping.h') in items and str(read(W/'source-proof.json')['original']['path']) in items,'both overlay headers actually compiled')
   else:need(str(I/'ada_address_mapping.h') not in items,'original baseline no new mapping')
  need([pin(p['path']) for p in before]==before,'source/overlay/config/objects unchanged')
  receipt.update(status='PASS_OWNED_ADA_STRUCTURE_ASYNC_COMPONENT',sources_unchanged=True,baseline_q1_q8_exact=True,worker=pin(OUT/'worker.json'),results={n:pin(OUT/(n+'-run.stdout')) for n in ('baseline','candidate')},binaries={n:pin(OUT/n) for n in ('baseline','candidate')},objects=[dict(name=f'{i:02d}',disposition='recompiled_both_overlays' if i<=6 else 'reused_unrelated_HBF',object=pin(p),dependencies=pin((OUT if i<=6 else B)/(f'{i:02d}.d'))) for i,p in enumerate(coreobjs,1)],compile_flags=build['flags'],fixture_dependencies={n:pin(OUT/(n+'.d')) for n in ('baseline','candidate')})
 except BaseException as e:receipt.update(status='FAIL_OWNED_ADA_STRUCTURE_PRESERVED',error=type(e).__name__+': '+str(e))
 save();print(json.dumps({k:receipt[k] for k in ('status','elapsed_seconds','error') if k in receipt}));return 0 if receipt['status'].startswith('PASS') else 1
if __name__=='__main__':raise SystemExit(worker() if '--worker' in sys.argv else main())
