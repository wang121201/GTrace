#!/usr/bin/env python3
"""Build/replay an explicitly selected, pinned Ada functional cache profile."""
import argparse, pathlib, json, subprocess, resource, time, hashlib, shutil, sys
from tools.ada_profile_registry import PROFILES, DEFAULT_PROFILE, profile_dir
ROOT=pathlib.Path(__file__).resolve().parent

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def source_pins():
    paths=list((ROOT/'source').rglob('*'))+[ROOT/'ada_profile.py',ROOT/'build-config.json']+list((ROOT/'tools').glob('*.py'))
    return {str(p.relative_to(ROOT)):sha(p) for p in sorted(paths) if p.is_file()}
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input',type=pathlib.Path,required=True)
    ap.add_argument('--output',type=pathlib.Path,required=True)
    ap.add_argument('--emit-trace',action='store_true')
    ap.add_argument('--binary',type=pathlib.Path)
    ap.add_argument('--compiler',help='C++20 compiler override; recorded in receipt')
    ap.add_argument('--profile',choices=PROFILES,default=DEFAULT_PROFILE)
    args=ap.parse_args();out=args.output.resolve();src=args.input.resolve()
    if not src.is_file():ap.error('input does not exist')
    if out.exists():ap.error('choose a new output directory')
    # Validate every pin: an observed per-kernel carveout can resolve an adaptive
    # r2 request to either pinned fixed variant during the run.
    subprocess.run([sys.executable,str(ROOT/'tools/import_ada_calibrations.py'),'--check'],check=True,stdout=subprocess.DEVNULL)
    config=json.loads((ROOT/'build-config.json').read_text())
    compiler=shutil.which(args.compiler or config['compiler'])
    if not args.binary and not compiler:ap.error('C++20 compiler not found')
    out.mkdir(parents=True)
    profiles={name:sha(profile_dir(name)/'profile.json') for name in PROFILES}
    before=source_pins()
    receipt={'schema':'GTSIM_ADA_RUN_RECEIPT_V2','requested_profile':args.profile,
             'input_sha256':sha(src),'profile_sha256':profiles[args.profile],
             'available_profile_sha256':profiles,'source_sha256':before,
             'CPU_units':'minutes user+system','mode':'functional cache only'}
    binary=args.binary.resolve() if args.binary else out/'ada_cache_replay'
    if not args.binary:
        argv=[compiler,'-std=c++20','-O2','-Wall','-Wextra','-DTILEGEN_DIRTY_SECTOR_MODE=2']+['-I'+str(ROOT/p) for p in ['source']+config['include_directories']]+[str(ROOT/'source/ada_cache_replay.cpp'),'-o',str(binary)]
        now=time.monotonic();b=subprocess.run(argv,capture_output=True,text=True)
        receipt['compile']={'argv':argv,'returncode':b.returncode,'wall_minutes':(time.monotonic()-now)/60}
        (out/'compile.stderr').write_text(b.stderr)
        if b.returncode:
            receipt['status']='FAILED_COMPILE';(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');return b.returncode
    if not binary.is_file():raise SystemExit('binary missing')
    argv=[str(binary),str(src),str(out/'postcache.requests.jsonl') if args.emit_trace else '-',args.profile]
    r0=resource.getrusage(resource.RUSAGE_CHILDREN);now=time.monotonic()
    with (out/'result.json').open('wb') as stdout,(out/'run.stderr').open('wb') as stderr:
        p=subprocess.run(argv,stdout=stdout,stderr=stderr)
    r1=resource.getrusage(resource.RUSAGE_CHILDREN)
    receipt.update(argv=argv,returncode=p.returncode,wall_minutes=(time.monotonic()-now)/60,cpu_minutes=(r1.ru_utime+r1.ru_stime-r0.ru_utime-r0.ru_stime)/60,binary_sha256=sha(binary))
    receipt['source_unchanged']=before==source_pins()
    receipt['input_unchanged']=sha(src)==receipt['input_sha256']
    receipt['profile_unchanged']=profiles=={name:sha(profile_dir(name)/'profile.json') for name in PROFILES}
    receipt['status']='PASS_PROCESS_ONLY' if p.returncode==0 and receipt['source_unchanged'] and receipt['input_unchanged'] and receipt['profile_unchanged'] else 'FAILED'
    if p.returncode==0:
        result=json.loads((out/'result.json').read_text())
        resolved=sorted({phase['resolved_profile'] for phase in result['phases']})
        receipt['resolved_profile_sha256']={name:profiles[name] for name in resolved}
        if result['configuration']!=args.profile:receipt['status']='FAILED_PROFILE_IDENTITY'
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:receipt[k] for k in ['status','requested_profile','cpu_minutes','wall_minutes','returncode']}))
    return 0 if receipt['status']=='PASS_PROCESS_ONLY' else 1
if __name__=='__main__':raise SystemExit(main())
