"""Enter shared CPU admission before immutable compilation and preflight."""
import json
from pathlib import Path
import subprocess
from sector32_prepare_xmu import ROOT, CTRL, pin, save, need

def main():
    need(Path(__file__).resolve()==ROOT/'repo/llm/tools/sector32_bootstrap_xmu.py','exact task-owned launcher')
    doc=json.loads((ROOT/'repo-manifest.json').read_text());sources=[]
    for row in doc['files']:
        actual=pin(ROOT/'repo'/row['relative_path'])
        need(actual['bytes']==row['bytes'] and actual['sha256']==row['sha256'],'staged file identity')
        sources.append(actual)
    sources += [pin(ROOT/'repo-manifest.json'),pin(CTRL)]
    spec=dict(case_id='sector32-build-admit-r1',tool='pinned-CPU-build-and-preflight',input_kind='SOURCE_ONLY_REUSE_FROZEN_GRAPHS',
        cpu=9,gpu=None,seconds=1200,rss_limit_bytes=8<<30,
        argv=['/usr/bin/python3','-B',str(ROOT/'repo/llm/tools/sector32_prepare_xmu.py'),'--root',str(ROOT)],
        environment=dict(PYTHONDONTWRITEBYTECODE='1',OMP_NUM_THREADS='1',MKL_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1'),sources=sources)
    path=ROOT/'prepare-spec.json';save(path,spec)
    result=subprocess.run(['/usr/bin/python3','-B',str(CTRL),'--spec',str(path),'--output',str(ROOT/'prepare-job'),'--execute'],capture_output=True,text=True)
    (ROOT/'prepare-controller.log').write_text(result.stdout+result.stderr)
    finish=json.loads((ROOT/'prepare-job/job-finish.json').read_text())
    need(result.returncode==0 and finish['status']=='PASS_PROCESS_ONLY' and finish['process']['cleanup']['owned_descendants_empty'],'controlled build/preflight failed; inspect retained task log')
    print(json.dumps({k:finish[k] for k in ('status','CPU_minutes','wall_minutes') if k in finish}))
if __name__=='__main__':main()
