#!/usr/bin/env python3
"""Import pinned r2/r3 candidates without promoting experimental r3 to default."""
import argparse,copy,hashlib,json
from ada_profile_registry import ROOT,PROFILES,TRACE_SHA256,profile_dir,DEFAULT_PROFILE
from import_ada_tuner import build as build_v1, options

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def build(name):
 rev,directory,pinned=PROFILES[name]
 if name=='tuner-v1':return build_v1()
 folder=profile_dir(name);src=folder/'source/gpgpusim.config';trace=folder/'source/trace.config'
 if sha(src)!=pinned or sha(trace)!=TRACE_SHA256:raise ValueError('pinned calibration source changed: '+name)
 d=copy.deepcopy(build_v1());raw=options(src);raw_trace=options(trace)
 old=d['raw_gpgpusim_options'];changes={k:{'before':old.get(k),'after':v} for k,v in raw.items() if old.get(k)!=v}
 allowed={'-gpgpu_shmem_option','-gpgpu_l2_rop_latency'}
 if rev=='r3':allowed|={'-gpgpu_adaptive_cache_config','-gpgpu_unified_l1d_size','-gpgpu_cache:dl1'}
 if set(changes)-allowed or set(old)-set(raw):raise ValueError('unmapped calibration option changes')
 if raw_trace!=d['raw_trace_options']:raise ValueError('instruction calibration is outside this profile')
 first,policies,mshr,queues,port=raw['-gpgpu_cache:dl1'].split(',')
 kind,sets,line,ways=first.split(':');sets,line,ways=map(int,(sets,line,ways));replacement=policies.split(':')[0]
 if kind!='S' or replacement not in ['L','F']:raise ValueError('unsupported cache interpretation')
 adaptive=raw['-gpgpu_adaptive_cache_config']=='1';unified=int(raw['-gpgpu_unified_l1d_size'])*1024
 shared=[int(x)*1024 for x in raw['-gpgpu_shmem_option'].split(',')]
 effective=[unified-x for x in shared] if adaptive else [sets*line*ways]
 d.update(schema='GTSIM_ACCELSIM_ADA_CALIBRATION_PROFILE_V2',profile_id=name,calibration_revision=rev,default_profile=DEFAULT_PROFILE,experimental=rev=='r3',calibration_status='EXPERIMENTAL_NOT_PROMOTED' if rev=='r3' else 'PARTIALLY_VALIDATED_CANDIDATE_NOT_FULL_TRAFFIC_MATCH',raw_gpgpusim_options=raw,raw_trace_options=raw_trace,option_diff_from_tuner=changes)
 d['sources']=[{'path':str(p.relative_to(ROOT)),'sha256':sha(p)} for p in [src,trace]]
 d['upstream_configuration']=f'/home/xmu/nvidiagds/codex-runs/rtx4000-ada-cache-calibration-20260922-{rev}/configs/{directory}'
 d['L1'].update(sets=sets,base_ways=ways,base_bytes=sets*line*ways,unified_bytes=unified,adaptive=adaptive,shared_carveout_bytes=shared,effective_L1_bytes=effective,replacement='FIFO' if replacement=='F' else 'LRU')
 d['memory']['rop_core_cycles']=int(raw['-gpgpu_l2_rop_latency'])
 d['carveout_binding']={'requires_observed_shared_carveout_bytes':name!='r2-adaptive','expected_observed_bytes':int(name.rsplit('shared',1)[1])*1024 if 'shared' in name else None,'adaptive_without_observation':'resource rule, not a claim of observed hardware preference','adaptive_with_observation':'resolve 64/100 KiB to the corresponding pinned r2 fixed profile; reject insufficient or mismatched shared allocation'}
 d['latency_contract']={'L1_base_cycles':34,'L2_additional_cycles':239,'DRAM_additional_cycles':324,'derived_serial_L2_total_cycles':273,'derived_serial_DRAM_total_cycles':597,'traffic_runner_executes_latency':False,'not_hardware_timing_validation':True}
 d['scope']['functional']='sector/known-byte/L1 LRU or experimental FIFO/static geometry or adaptive sizing implemented'
 d['scope']['calibration']='r2/r3 microbench L1 filtering evidence only; L2 dirty-write and LLM NCU not validated by these campaigns'
 return d

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--check',action='store_true');ap.add_argument('--profile',choices=PROFILES);args=ap.parse_args()
 for name in ([args.profile] if args.profile else PROFILES):
  d=build(name);out=profile_dir(name)/'profile.json';text=json.dumps(d,ensure_ascii=False,indent=2)+'\n'
  if args.check:
   if not out.exists() or out.read_text()!=text:raise SystemExit('profile differs from pinned configuration: '+name)
  else:out.write_text(text)
 print('PASS_ADA_CALIBRATION_CONFIGS' if args.check else 'WROTE_PINNED_ADA_CALIBRATION_CONFIGS')
if __name__=='__main__':main()
