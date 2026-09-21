#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Import the pinned XMU tuner file, retaining every option and its units.
No NCU fitting, no simulation; output is an auditable software configuration.
"""
import pathlib,json,hashlib,argparse
ROOT=pathlib.Path(__file__).resolve().parents[1]
CFG=ROOT/'configs/rtx4000-ada-accelsim-v1'
def options(p):
 out={}
 for i,line in enumerate(p.read_text().splitlines(),1):
  line=line.split('#',1)[0].strip()
  if not line:continue
  key,value=line.split(None,1)
  if not key.startswith('-')or key in out:raise ValueError(f'{p}:{i}: invalid/duplicate option')
  out[key]=value
 return out

def build():
 p=CFG/'source/gpgpusim.config';t=CFG/'source/trace.config';x=options(p);y=options(t)
 for f,h in [(p,'e09511b9d168632e975422b19a9557ff5ed4482ace160fa428f9c70455f5a891'),(t,'c4e4d8e85e9049af5694afca10b031fae4eafcdd06f0dbd3d8042d7195db890b')]:
  if hashlib.sha256(f.read_bytes()).hexdigest()!=h:raise ValueError('profile v1 rejects modified pinned tuner source: '+str(f))
 expected={'-gpgpu_n_clusters':'48','-gpgpu_n_cores_per_cluster':'1','-gpgpu_n_mem':'10','-gpgpu_n_sub_partition_per_mchannel':'2','-gpgpu_cache:dl1':'S:4:128:64,L:T:m:L:L,A:384:48,16:0,32','-gpgpu_cache:dl2':'S:1024:128:16,L:B:m:L:X,A:192:4,32:0,32','-gpgpu_memory_partition_indexing':'0','-gpgpu_shader_core_pipeline':'1536:32','-gpgpu_shmem_option':'0,8,16,32,64,100','-gpgpu_clock_domains':'2175:2175:2175:4500.5'}
 for k,v in expected.items():
  if x.get(k)!=v:raise ValueError(f'profile v1 rejects unsupported {k}: {x.get(k)!r}, expected {v!r}')
 timing=dict(kv.split('=')for kv in x['-gpgpu_dram_timing_opt'].split(':'))
 return {'schema':'GTSIM_ACCELSIM_ADA_PROFILE_V1','profile_id':'accelsim-rtx4000-ada-v1','hardware_calibrated':False,'framework_commit':'d930ad6d02c09bb56867132583735aba0389cff4','gpgpusim_commit':'91880c53383d5a6a6742bfb1be2c5f34e39c7871','sources':[{'path':str(f.relative_to(ROOT)),'sha256':hashlib.sha256(f.read_bytes()).hexdigest()}for f in [p,t]],
 'sm':{'count':48,'compute_subpartitions_per_sm':4,'scheduler':'GTO','max_resident_ctas':24,'max_threads':1536,'warp_threads':32,'registers_per_sm':65536,'registers_per_cta':65536,'register_allocation_granularity_per_thread':4,'shared_capacity_bytes':102400,'shared_per_block_option_bytes':49152,'sub_core_model':True,'register_banks':16,'register_file_port_throughput':2},
 'clocks_mhz':dict(zip(['core','interconnect','l2','dram'],map(float,x['-gpgpu_clock_domains'].split(':')))),
 'L1':{'line_bytes':128,'sector_bytes':32,'sets':4,'base_ways':64,'base_bytes':32768,'unified_bytes':131072,'adaptive':True,'shared_carveout_bytes':[i*1024 for i in [0,8,16,32,64,100]],'effective_L1_bytes':[i*1024 for i in [128,120,112,96,64,28]],'write_policy':'WRITE_THROUGH','write_allocate':'LAZY_FETCH_ON_READ','allocation':'ON_MISS','replacement':'LRU','set_index':'LINEAR','dirty_protection_percent':25,'banks':4,'hit_latency_core_cycles':34,'MSHR_entries':384,'MSHR_merge_limit':48,'miss_queue':16,'data_port_bytes':32,'kernel_boundary':'invalidate'},
 'L2':{'bytes':41943040,'channels':10,'subpartitions_per_channel':2,'subpartitions':20,'sets_per_subpartition':1024,'ways':16,'line_bytes':128,'sector_bytes':32,'write_policy':'WRITE_BACK','write_allocate':'LAZY_FETCH_ON_READ','allocation':'ON_MISS','replacement':'LRU','set_index':'XOR_ON_PARTITION_LOCAL_ADDRESS','dirty_protection_percent':0,'MSHR_entries_per_subpartition':192,'MSHR_merge_limit':4,'miss_queue_per_subpartition':32,'data_port_bytes':32,'final_flush':False},
 'memory':{'channel_bus_bytes':2,'burst_length':16,'bytes_per_burst':32,'data_command_freq_ratio':4,'nominal_peak_GBps':360.04,'address_map':x['-gpgpu_mem_addr_mapping'],'partition_queues':[64]*4,'dram_scheduler':'FR_FCFS','dram_scheduler_queue':64,'dram_return_queue':192,'rop_core_cycles':238,'dram_latency':324,'timing':{k:int(v)for k,v in timing.items()},'bank_indexing':0,'bank_group_indexing':1},
 'scope':{'functional':'sector/known-byte/LRU/cache geometry and adaptive sizing implemented','sm':'occupancy resource caps and 48x4 GTSim structural adapter','timing':'not an Accel-Sim timing emulator; partition queue/MSHR, register-file arbitration and interconnect retain implementation gaps','no_claim':'not a measured hardware cache design or a <20% NCU result'},'raw_gpgpusim_options':x,'raw_trace_options':y}
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--check',action='store_true');args=ap.parse_args();d=build();s=json.dumps(d,ensure_ascii=False,indent=2)+'\n';p=CFG/'profile.json'
 if args.check:
  if not p.exists()or p.read_text()!=s:raise SystemExit('profile.json differs from pinned inputs')
  print('PASS_PINNED_ADA_CONFIG')
 else:p.write_text(s);print(p)
