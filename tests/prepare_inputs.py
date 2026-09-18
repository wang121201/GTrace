"""Read pinned B1 inputs, save reusable compressed validation transports locally."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, resource, sys, time
sys.dont_write_bytecode = True
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-runtime', type=Path, required=True,
                    help='Existing sealed canonical-full-runtime-r4 directory; imported preparation is CPU-only')
parser.add_argument('--output', type=Path, required=True, help='New directory for validation transports')
args = parser.parse_args()
BASE, OUT = args.source_runtime.resolve(), args.output.resolve()
sys.path.insert(0, str(BASE))
spec = importlib.util.spec_from_file_location('unified_validation_prepare', BASE/'prepare.py')
P = importlib.util.module_from_spec(spec); spec.loader.exec_module(P)
OUT.mkdir(parents=True, exist_ok=False)
start = time.monotonic(); usage_before=resource.getrusage(resource.RUSAGE_SELF)
payloads, mm, receipt = P.frames()
reg = P.ROUTE.Registry()
keys = [r['source_launch_key'] for r in reg.rows]
full_map = P.service_map(reg, keys)
families = {}
for key in keys:
    families.setdefault(reg.available[key][0], key)
print('families', families, flush=True)
cases = [('families', list(families.values()), 1),
         ('decode', ['epoch-3-launch-28','epoch-3-launch-29','epoch-3-launch-30'], 32),
         ('decode-512', ['epoch-3-launch-28','epoch-3-launch-29','epoch-3-launch-30'], 512),
         ('p28-prefix-one', ['epoch-1-launch-16'], 1)]
pins=[]
for name, selected, prefix in cases:
    control, segments = P.control(reg, selected, payloads, mm, prefix)
    control['decoded_control']['service_address_map'] = full_map
    control, segments = P.pack.transport(payloads, control['decoded_control'])
    path=OUT/(name+'.input')
    with path.open('wb') as f:
        for segment in segments: f.write(segment)
    (OUT/(name+'-control.json')).write_text(json.dumps(control, indent=2)+'\n')
    pins.append(dict(name=name, path=str(path), bytes=path.stat().st_size,
                     sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                     keys=selected, prefix=prefix))
usage_after=resource.getrusage(resource.RUSAGE_SELF)
receipt.update(validation_inputs=pins, preparation_seconds=time.monotonic()-start,
               preparation_CPU_seconds=usage_after.ru_utime+usage_after.ru_stime-usage_before.ru_utime-usage_before.ru_stime,
               new_GPU_sampling=False, batch_size=1,
               note='Only compressed sealed input templates saved; no expanded trace generated during preparation.')
(OUT/'preparation.json').write_text(json.dumps(receipt, indent=2)+'\n')
print(json.dumps(dict(status='PASS', seconds=receipt['preparation_seconds'],inputs=pins)), flush=True)
