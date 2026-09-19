#!/usr/bin/env python3
"""Build/run the bounded GEMV typed adapter check; one CPU, no simulation."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def pin(path):
    raw = path.read_bytes()
    return dict(path=str(path.resolve()), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--receipt', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = json.loads((ROOT/'build-config.json').read_text())
    compiler = config['compiler']
    flags = ['-std='+config['standard'], '-O1', '-pthread']
    flags += ['-D'+d for d in config['definitions']]
    flags += ['-I'+str(ROOT/d) for d in config['include_directories']]
    test = ROOT/'tests/gemv_typed_binding_test.cpp'
    units = [test] + [ROOT/path for path in config['translation_units'][1:]]
    pins = [pin(path) for path in sorted((ROOT/'source').rglob('*')) if path.is_file()]
    pins += [pin(test),pin(Path(__file__)),pin(ROOT/'build-config.json'),
             pin(ROOT/'native_transfer/regression_inputs.json')]
    began = time.monotonic()
    receipt = dict(schema='GEMV_TYPED_BINDING_CPU_VALIDATION_V1', status='RUNNING',
                   jobs=1, GPU_executed=False, HBFSIM_executed=False,
                   source_pins=pins, compiler=compiler, flags=flags, steps=[])
    try:
        commands = [('compile-%02d'%i, [compiler,*flags,'-MMD','-MF',str(out/('%02d.d'%i)),
                                     '-c',str(unit),'-o',str(out/('%02d.o'%i))]) for i,unit in enumerate(units)]
        binary=out/'gemv-typed-binding-test'
        commands += [('link',[compiler,*flags,*[str(out/('%02d.o'%i)) for i in range(len(units))],
                              *['-l'+x for x in config['libraries']],'-o',str(binary)]),
                     ('run',[str(binary),str(ROOT/'native_transfer/regression_inputs.json')])]
        for name,command in commands:
            start=time.monotonic();before=resource.getrusage(resource.RUSAGE_CHILDREN)
            with (out/(name+'.stdout')).open('wb') as stdout,(out/(name+'.stderr')).open('wb') as stderr:
                result=subprocess.run(command,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr)
            after=resource.getrusage(resource.RUSAGE_CHILDREN)
            receipt['steps'].append(dict(name=name,command=command,exit_code=result.returncode,
                elapsed_seconds=time.monotonic()-start,CPU_user_seconds=after.ru_utime-before.ru_utime,
                CPU_system_seconds=after.ru_stime-before.ru_stime))
            if result.returncode: raise RuntimeError(name+' failed; inspect '+str(out/(name+'.stderr')))
        result=json.loads((out/'run.stdout').read_text())
        if result['status']!='PASS_11_SEALED_TEMPLATES_TYPED_ADDRESSES_MATCH_OLD_BINDING':
            raise RuntimeError('typed address validation did not pass')
        for before in pins:
            if pin(Path(before['path']))!=before:raise RuntimeError('source changed during validation: '+before['path'])
        receipt.update(status=result['status'],sources_unchanged=True,result=result,
                       binary=pin(binary),result_pin=pin(out/'run.stdout'))
    except Exception as error:
        receipt.update(status='FAIL',error=str(error))
    receipt['elapsed_seconds']=time.monotonic()-began
    receipt['compile_CPU_user_system_seconds']=sum(s['CPU_user_seconds']+s['CPU_system_seconds'] for s in receipt['steps'] if s['name']!='run')
    receipt['test_CPU_user_system_seconds']=sum(s['CPU_user_seconds']+s['CPU_system_seconds'] for s in receipt['steps'] if s['name']=='run')
    a.receipt.write_text(json.dumps(receipt,indent=2)+'\n')
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:receipt[k] for k in ('status','elapsed_seconds','test_CPU_user_system_seconds','error') if k in receipt}))
    return 0 if receipt['status'].startswith('PASS') else 1


if __name__=='__main__':
    raise SystemExit(main())
