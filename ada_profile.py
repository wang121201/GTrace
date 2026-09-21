#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build/replay the pinned Ada functional cache profile. No GPU or NCU work."""
import argparse,pathlib,json,subprocess,resource,time,hashlib,shutil,sys
ROOT=pathlib.Path(__file__).resolve().parent

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 a=argparse.ArgumentParser(description=__doc__);a.add_argument('--input',type=pathlib.Path,required=True);a.add_argument('--output',type=pathlib.Path,required=True);a.add_argument('--emit-trace',action='store_true');a.add_argument('--binary',type=pathlib.Path);a.add_argument('--compiler',help='C++20 compiler override; recorded in receipt');args=a.parse_args();out=args.output.resolve();src=args.input.resolve()
 if not src.is_file():a.error('input does not exist')
 if out.exists():a.error('choose a new output directory')
 subprocess.run([sys.executable,str(ROOT/'tools/import_ada_tuner.py'),'--check'],check=True,stdout=subprocess.DEVNULL)
 out.mkdir(parents=True);config=json.loads((ROOT/'build-config.json').read_text());before={str(p.relative_to(ROOT)):sha(p)for p in sorted((ROOT/'source').rglob('*'))if p.is_file()};receipt={'schema':'GTSIM_ADA_RUN_RECEIPT_V1','input_sha256':sha(src),'profile_sha256':sha(ROOT/'configs/rtx4000-ada-accelsim-v1/profile.json'),'source_sha256':before,'CPU_units':'minutes user+system','mode':'functional cache only'}
 binary=args.binary.resolve()if args.binary else out/'ada_cache_replay'
 if not args.binary:
  compiler=shutil.which(args.compiler or config['compiler'])
  if not compiler:a.error('C++20 compiler not found')
  argv=[compiler,'-std=c++20','-O2','-Wall','-Wextra','-DTILEGEN_DIRTY_SECTOR_MODE=2']+['-I'+str(ROOT/p)for p in ['source']+config['include_directories']]+[str(ROOT/'source/ada_cache_replay.cpp'),'-o',str(binary)]
  now=time.monotonic();b=subprocess.run(argv,capture_output=True,text=True);receipt['compile']={'argv':argv,'returncode':b.returncode,'wall_minutes':(time.monotonic()-now)/60};(out/'compile.stderr').write_text(b.stderr)
  if b.returncode:(out/'receipt.json').write_text(json.dumps(receipt,indent=2));raise SystemExit(b.returncode)
 if not binary.is_file():raise SystemExit('binary missing')
 argv=[str(binary),str(src)]+([str(out/'postcache.requests.jsonl')]if args.emit_trace else[]);r0=resource.getrusage(resource.RUSAGE_CHILDREN);now=time.monotonic()
 with (out/'result.json').open('wb')as stdout,(out/'run.stderr').open('wb')as stderr:p=subprocess.run(argv,stdout=stdout,stderr=stderr)
 r1=resource.getrusage(resource.RUSAGE_CHILDREN);receipt.update(argv=argv,returncode=p.returncode,wall_minutes=(time.monotonic()-now)/60,cpu_minutes=(r1.ru_utime+r1.ru_stime-r0.ru_utime-r0.ru_stime)/60,binary_sha256=sha(binary))
 receipt['source_unchanged']=before=={str(p.relative_to(ROOT)):sha(p)for p in sorted((ROOT/'source').rglob('*'))if p.is_file()}
 receipt['input_unchanged']=sha(src)==receipt['input_sha256']
 receipt['profile_unchanged']=sha(ROOT/'configs/rtx4000-ada-accelsim-v1/profile.json')==receipt['profile_sha256']
 receipt['status']='PASS_PROCESS_ONLY'if p.returncode==0 and receipt['source_unchanged'] and receipt['input_unchanged'] and receipt['profile_unchanged'] else'FAILED'
 (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps({k:receipt[k]for k in ['status','cpu_minutes','wall_minutes','returncode']}));return 0 if receipt['status']=='PASS_PROCESS_ONLY'else 1
if __name__=='__main__':raise SystemExit(main())
