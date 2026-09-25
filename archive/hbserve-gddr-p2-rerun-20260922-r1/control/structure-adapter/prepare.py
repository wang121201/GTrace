from pathlib import Path
import hashlib,json,difflib
D=Path(__file__).resolve().parent; R=D.parents[2]
S=R/'work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot'
I=S/'source/work/tilegen-full-r1/core-native-copy-r2/include'
Z=Path('/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo')
L=Path('/Users/wgs/Documents/Codex/2026-09-21/lian')
def pin(p):
 p=Path(p).resolve();b=p.read_bytes();return dict(path=str(p),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
def put(n,x): (D/n).write_text(json.dumps(x,indent=2)+'\n')
shared=Z/'source/work/tilegen-full-r1/core-native-copy-r2/include/ada_address_mapping.h'
assert pin(shared)['sha256']=='1dedfe56368be08dd3ba3bdc13690c11c383a8ebb2e25aa60c1d1f4c433d6068'
(D/'ada_address_mapping.h').write_bytes(shared.read_bytes())
old=(I/'cache_geometry.h').read_text(); s=old
pairs=[('#include <vector>','#include <vector>\n#include "ada_address_mapping.h"'),('    PAPER_ADA_SET_ASSOCIATIVE\n','    PAPER_ADA_SET_ASSOCIATIVE,\n    ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE\n'),('    static L2GeometryConfig paper_ada_l2_v1() {','    static L2GeometryConfig accelsim_rtx4000_ada() {\n        return {L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE};\n    }\n    static L2GeometryConfig paper_ada_l2_v1() {'),('        default:\n            throw std::invalid_argument("unknown L2 geometry mode");','        case L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE:\n            if (line_bytes != AdaAddressMapping::line_bytes || cache_bytes != AdaAddressMapping::cache_bytes)\n                throw std::invalid_argument("AccelSim Ada software geometry requires20x1024x16x128 bytes");\n            groups_ = AdaAddressMapping::subpartitions * AdaAddressMapping::sets_per_subpartition;\n            group_capacity_ = AdaAddressMapping::ways;\n            break;\n        default:\n            throw std::invalid_argument("unknown L2 geometry mode");'),('    U partition(U address) const {\n','    U partition(U address) const {\n        if (config_.mode == L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE)\n            return AdaAddressMapping::sub_partition(address);\n'),('    U set(U address) const {\n','    U set(U address) const {\n        if (config_.mode == L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE)\n            return AdaAddressMapping::set(address);\n')]
for a,b in pairs: assert s.count(a)==1;s=s.replace(a,b)
t=s
for a,b in reversed(pairs): assert t.count(b)==1;t=t.replace(b,a)
assert t==old
assert s.split('template<class Key>')[1]==old.split('template<class Key>')[1]
(D/'cache_geometry.h').write_text(s)
(D/'source.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile=str(I/'cache_geometry.h'),tofile=str(D/'cache_geometry.h'))))
put('overlay.json',{'version':0,'case-sensitive':True,'use-external-names':False,'roots':[{'type':'file','name':str(I/n),'external-contents':str(D/n)} for n in ('cache_geometry.h','ada_address_mapping.h')]})
tuner=L/'outputs/RTX4000_Ada_targeted_config/T_user_tuner/gpgpusim.config'; j=L/'outputs/RTX4000_Ada_tuner_refine_continue_20260922/candidate_configs/J_refine26_l2delay/gpgpusim.config'
assert pin(tuner)['sha256']=='e09511b9d168632e975422b19a9557ff5ed4482ace160fa428f9c70455f5a891'
assert pin(j)['sha256']=='08985c9342affa46bfccad7f330857317aec85f768d56c260a046669934c6221'
def cfg(p):
 out={}
 for raw in p.read_text().splitlines():
  line=raw.split('#')[0].strip()
  if line.startswith('-'):
   a=line.split(None,1)
   if len(a)==2:out[a[0]]=a[1]
 return out
profiles=[]
for name,p,l1,rop in [('tuner_v1',tuner,34,238),('J_unpromoted',j,39,237)]:
 c=cfg(p);assert int(c['-gpgpu_l1_latency'])==l1 and int(c['-gpgpu_l2_rop_latency'])==rop
 assert c['-gpgpu_n_mem']=='10' and c['-gpgpu_n_sub_partition_per_mchannel']=='2'
 assert c['-gpgpu_cache:dl2'].split(',')[0]=='S:1024:128:16'
 assert float(c['-gpgpu_clock_domains'].split(':')[0])==2175.0
 profiles.append(dict(name=name,source=pin(p),L1_hit_cycles=l1,ROP_residual_cycles=rop,folded_L2_hit_candidate_cycles=l1+rop,core_MHz=2175,admission='reference software candidate; not native hardware latency calibration' if name=='tuner_v1' else 'no promotion; training clock validity failures',raw_fields=c))
profile=json.loads((R/'work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-execution-r1/prepared-r1/input.json').read_text())['profile'];profile['clock']={'period_ps_numerator':40000,'period_ps_denominator':87,'source':'pinned tuner-v1 fixed2175MHz software reference; not current GPU measurement'};profile['core_profile']='ada_structure_tuner_v1_external_only';profile['internal_miss_latency_usage']='unused: finite external completion backend owns each miss; no additive 324/604/post-hit latency'
put('profile.json',profile)
put('source-proof.json',dict(status='PASS_LITERAL_INVERSE_SOURCE_ONLY',inverse_byte_exact=True,original_geometry=pin(I/'cache_geometry.h'),candidate_geometry=pin(D/'cache_geometry.h'),mapping_source=pin(shared),mapping_copy=pin(D/'ada_address_mapping.h'),literal_edits=pairs,grouped_LRU_byte_exact=True,core_memory_byte_exact=pin(I/'memory.h'),HBF_backend_byte_exact=pin(S/'source/work/tilegen-hbf-drain-native-r1/native_backend.h'),profile_original=pin(R/'work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-execution-r1/prepared-r1/input.json')))
put('provenance.json',dict(schema='ADA_STRUCTURE_CANDIDATE_PROVENANCE_V1',profiles=profiles,fields=[dict(field='L2 capacity/groups/ways/line',value='40MiB/20*1024/16/128B',source='gpgpu_cache:dl2, n_mem10 * subpartition2',qualification='software structure only; actual L2 keeps128B valid/fill and32B dirty masks; not a new sector-valid L2'),dict(field='address grouping',value='pinned AdaAddressMapping on raw cache-key address',source=pin(shared),qualification='AccelSim software mapping; NVIDIA physical address hash unproved'),dict(field='backend route',value='unchanged HbmDevice.decode(mapped request.address)',qualification='20 L2 groups are not20 queues and not a proved mapping to10 HBF service channels; metadata l2_subpartition_id unchanged'),dict(field='latency miss',value='external backend only, zero extra frontend pipeline',qualification='frontend/network miss latency unmeasured; old604 fields retained unused'),dict(field='unchanged',value='L1 capacity32KiB/64way sector32 current setting;128B L2 RFO;32B WB;shared h288 selection;current HBF selected.cfg, credits and path',qualification='not adopting dirty-age, new calibrated policy, queue model, occupancy or compute timings')],source_pins=[pin(x) for x in [tuner,j,shared,D.parent/'accelsim-evidence/evidence.json',D.parent/'accelsim-evidence/report.md',Path(profile['native_hbfsim_config_file'])]]))
print('PASS isolated source preparation')
