#include "runner.h"
#include "frozen_candidate_direct_cache.h"
#include <iostream>
#include <sstream>
#include <random>
using U=std::uint64_t;using J=nlohmann::json;
unsigned checks=0;
void check(bool v,const char* m){++checks;if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f,const char*m){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,m);}
J allocation(U base,U bytes,U generation,bool freeing=false){return {{"kind","allocation_API_observation"},{"submission_event",1},{"return_event",2},{"raw_before",J::object()},{"raw_return",J::object()},{"observation",{{"action",freeing?"free":"allocate"},{"cuda_api",freeing?"cuMemFree_v2":"cuMemAlloc_v2"},{"base_u64",base},{"bytes",bytes},{"allocation_generation",generation},{"async",false},{"free_matched_observed_generation",freeing},{"generation_semantics","host_API_return_not_device_completion"}}}};}
GTSim::ExplicitMemorySubop sub(U address,U bytes){GTSim::ExplicitMemorySubop s;s.requested_bytes=bytes;s.ranges.push_back({0,address,bytes});return s;}
int main(){try{
 const auto name=llm_l1::profile();auto cfg=llm_l1::config(llm_l1::assumed_shared());
 std::vector<std::array<U,24>> unused;std::vector<native_trace::Record> events;
 direct_native::DiagnosticOptions opts;opts.ef_hit_rate=288;opts.dirty_age_accesses=3;
 direct_native::FunctionalCache cache(cfg,1024,[](int,U a){return a;},[&](auto r){events.push_back(r);},GTSim::L2GeometryConfig{},opts);
 if(name=="legacy32"){
  llm_legacy_l1::PerSmL1Config old;old.mode=llm_legacy_l1::PerSmL1Mode::MODELED_SET_ASSOCIATIVE;old.persistence=llm_legacy_l1::PerSmL1Persistence::KERNEL_FLUSH;old.num_sms=48;old.capacity_bytes_per_sm=32768;old.ways=64;old.store_bypass=true;old.sector_validity=true;
  baseline_direct_native::DiagnosticOptions oldopts;oldopts.ef_hit_rate=288;oldopts.dirty_age_accesses=3;std::vector<native_trace::Record> expected;
  baseline_direct_native::FunctionalCache baseline(old,1024,[](int,U a){return a;},[&](auto r){expected.push_back(r);},GTSim::L2GeometryConfig{},oldopts);
  std::mt19937 random(291944);for(unsigned i=0;i<5000;++i){if(i%97==0){cache.begin_kernel();baseline.begin_kernel();}U addr=4096+(random()%256)*32;bool write=random()%5==0,bypass=random()%13==0;auto s=sub(addr,1+random()%80);std::vector<GTSim::ExplicitMemorySubop> ss{s};direct_native::Context c;c.sm_id=i%48;c.call_index=i;c.cta=i%48;c.warp=i%4;c.pc=32*(i%7);c.node_id=i/97;c.low_priority=i%2;baseline_direct_native::Context b;b.sm_id=c.sm_id;b.call_index=c.call_index;b.cta=c.cta;b.warp=c.warp;b.pc=c.pc;b.node_id=c.node_id;b.low_priority=c.low_priority;cache.instruction(0,write,bypass,ss,c);baseline.instruction(0,write,bypass,ss,b);check(cache.snapshot()==baseline.snapshot(),"legacy per-instruction snapshot differs");}
  check(events.size()==expected.size(),"legacy event count");for(unsigned i=0;i<events.size();++i)check(native_trace::words(events[i])==native_trace::words(expected[i]),"legacy DRAM sequence differs");
 }else{
  cache.allocation_metadata(allocation(4096,1024*1024,1));cache.begin_kernel();direct_native::Context c;c.sm_id=0;c.call_index=1;
  cache.instruction(0,false,false,{sub(4096,4)},c);check(cache.snapshot()["DRAM_read_bytes"]==128,"initial read fill");
  cache.instruction(0,false,false,{sub(4096,4)},c);check(cache.snapshot()["L1_read_hits"]==1,"sector hit");
  cache.instruction(0,false,false,{sub(4128,4)},c);check(cache.snapshot()["L1_read_misses"]==2,"different sector remains miss");check(cache.snapshot()["DRAM_read_bytes"]==128,"sector L2 retained hit");
  cache.instruction(0,true,false,{sub(4160,1)},c);check(cache.snapshot()["resident_dirty_sectors"]==1,"one byte dirties32B");check(cache.snapshot()["DRAM_write_bytes"]==0,"no eager store flush");
  auto prior=cache.snapshot();cache.begin_kernel({{"observed_shared_bytes",65536}});check(cache.l1_configuration()["shared_carveout_origin"]=="observed_shared_bytes_explicit_caller_witness","observed source");check(cache.snapshot()["DRAM_write_bytes"]==prior["DRAM_write_bytes"],"kernel reconfigure not L2flush");check(cache.snapshot()["resident_dirty_sectors"]==1,"dirty survives L1 reconfigure");
  check(cache.l1_configuration()["bytes_per_SM"]==(name=="r4"?67584:65536),"observed capacity");
  cache.begin_kernel({{"observed_shared_bytes",8192}});check(cache.l1_configuration()["bytes_per_SM"]==(name=="r4"?129024:122880),"8KiB extrapolated capacity");check(cache.l1_configuration()["shared_capacity_qualification"]=="uncalibrated_extrapolation_8_16KiB","8KiB extrapolation label");
  cache.begin_kernel({{"observed_shared_bytes",16384}});check(cache.l1_configuration()["bytes_per_SM"]==(name=="r4"?120832:114688),"16KiB extrapolated capacity");
  for(U a:{U(4224),U(4352),U(4480)})cache.instruction(0,false,false,{sub(a,4)},c);
  check(cache.snapshot()["age_writeback_bytes"]==32,"age sourceaccess tick expiry32");check(cache.snapshot()["resident_dirty_sectors"]==0,"age conservation");
  cache.instruction(0,true,false,{sub(4608,1)},c);auto count=events.size();cache.allocation_metadata(allocation(4096,1024*1024,1,true));check(events.size()==count,"free no cache effects");
  check(cache.snapshot()["dirty_sector_creations"].get<U>()*32==cache.snapshot()["DRAM_write_bytes"].get<U>()+cache.snapshot()["resident_dirty_sectors"].get<U>()*32,"dirty conservation");
  cache.instruction(0,false,true,{sub(0x10000000,4)},c);check(cache.l1_observation()["L1_hash_not_required_bypass_line_accesses"].get<U>()>=2,"bypass needs no invented allocation");
  rejects([&]{cache.instruction(0,false,false,{sub(0x10000000,4)},c);},"unknown L1 address should reject");
  rejects([&]{cache.begin_kernel({{"observed_shared_bytes",0}});},"unknown shared should reject");
  direct_native::FunctionalCache other(cfg,1024,[](int,U a){return a;},[](auto){},GTSim::L2GeometryConfig{},opts);
  other.allocation_metadata(allocation(4096,1024,1));rejects([&]{other.allocation_metadata(allocation(4224,512,2));},"overlap must reject");
  rejects([&]{other.allocation_metadata(allocation(4096,1024,2,true));},"free generation must reject");
  auto nullfree=allocation(0,1,1,true);nullfree["observation"]["bytes"]=nullptr;nullfree["observation"]["allocation_generation"]=nullptr;nullfree["observation"]["free_matched_observed_generation"]=false;other.allocation_metadata(nullfree);check(other.l1_observation()["driver_null_frees"]==1,"actual null free supported");
 }
 std::cout<<J({{"status","PASS_LLM_L1_ADAPTER"},{"profile",name},{"checks",checks},{"DRAM_records",events.size()}}).dump()<<'\n';return 0;
}catch(std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
