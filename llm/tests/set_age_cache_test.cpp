#include "runner.h"
#include "frozen_sector32_cache.h"
#include <iostream>
#include <random>
#include <sstream>
using U=std::uint64_t;using J=nlohmann::json;using Clock=direct_native::DirtyAgeClock;
unsigned checks=0;void check(bool v,const char* m){++checks;if(!v)throw std::runtime_error(m);}
GTSim::ExplicitMemorySubop sub(U a,U n){GTSim::ExplicitMemorySubop s;s.requested_bytes=n;s.ranges.push_back({0,a,n});return s;}
J allocation(){return {{"kind","allocation_API_observation"},{"submission_event",1},{"return_event",2},{"raw_before",J::object()},{"raw_return",J::object()},{"observation",{{"action","allocate"},{"cuda_api","cuMemAlloc_v2"},{"base_u64",4096},{"bytes",U{1}<<40},{"allocation_generation",1},{"async",false},{"generation_semantics","host_API_return_not_device_completion"}}}};}
const auto geometry=GTSim::L2GeometryConfig::paper_ada_l2_v1();
const GTSim::L2Geometry mapping(geometry,41943040,128);
std::vector<U> colliding(U start,unsigned count){std::vector<U> result;for(U a=start;result.size()<count;a+=128)if(mapping.group(a)==mapping.group(start))result.push_back(a);return result;}
struct Cache {
 std::vector<native_trace::Record> events;std::vector<std::uint32_t> masks;U ordinal=0,owner_bytes=0;bool ef=false,same_sm=false;std::unique_ptr<direct_native::FunctionalCache> c;
 Cache(Clock clock,U budget){direct_native::DiagnosticOptions o;o.ef_hit_rate=288;o.dirty_age_accesses=budget;o.dirty_age_clock=clock;o.data_policy=direct_native::DataPolicy::SECTOR32;
  c=std::make_unique<direct_native::FunctionalCache>(llm_l1::config(32768),41943040,[](int,U a){return a;},[&](auto r){events.push_back(r);},geometry,o,direct_native::OwnerObserver{},[&](const auto&,const auto&,auto,std::uint32_t mask){masks.push_back(mask);owner_bytes+=32;});c->allocation_metadata(allocation());c->begin_kernel();}
 void access(U a,U n=4,bool write=false,bool atomic=false,bool bypass=true){direct_native::Context ctx;ctx.sm_id=same_sm?0:ordinal%48;ctx.call_index=++ordinal;ctx.cta=ordinal%17;ctx.warp=ordinal%4;ctx.pc=ordinal%11*32;ctx.node_id=ordinal/97;ctx.atomic_rmw=atomic;ctx.low_priority=ef&&ordinal%2;c->instruction(0,write,bypass,{sub(a,n)},ctx);}
 void audit(){c->verify_resident_ledger();auto s=c->snapshot();check(s["DRAM_write_bytes"]==owner_bytes,"observer conservation");check(s["dirty_sector_creations"].get<U>()*32==s["DRAM_write_bytes"].get<U>()+s["resident_dirty_sectors"].get<U>()*32,"independent scalar ledger");}
 void drain(){direct_native::Context ctx;ctx.sm_id=0;ctx.call_index=++ordinal;c->drain(ctx);audit();}
 U stat(const char* key){return c->snapshot().at(key).get<U>();}
 J obs(){return c->dirty_age_observation();}
 std::vector<std::array<U,18>> read_projection(){std::vector<std::array<U,18>> result;for(auto r:events)if(r.cause==native_trace::Cause::ReadFillOrRfo){r.request_id=0;r.source_sequence=0;result.push_back(native_trace::words(r));}return result;}
};
void directed(){
 const auto a=colliding(4096,20);const U b=4224;check(mapping.group(a[0])!=mapping.group(b),"fixture separate groups");
 {for(auto clock:{Clock::GLOBAL_FORWARDED_LINE,Clock::SET_FORWARDED_LINE}){Cache x(clock,3);x.same_sm=true;x.access(a[0],4,false,false,false);const auto before=x.obs();x.access(a[0],4,false,false,false);x.access(a[0],4,false,false,false);const auto after=x.obs();check(x.stat("L1_read_hits")>=2&&x.stat("L2_forwarded_access_sequence")==1,"actual L1 hits do not tick global");if(clock==Clock::SET_FORWARDED_LINE)check(before["group_ticks"]==after["group_ticks"],"actual L1 hits do not tick set");x.audit();}}
 {Cache x(Clock::SET_FORWARDED_LINE,3),g(Clock::GLOBAL_FORWARDED_LINE,3);for(auto* c:{&x,&g}){c->access(a[0],4,true);for(int i=0;i<3;++i)c->access(b);c->audit();}check(x.stat("age_writeback_bytes")==0&&g.stat("age_writeback_bytes")==32,"cross group isolation");x.access(a[1]);x.access(a[1]);check(x.stat("age_writeback_bytes")==0,"budget minus one");x.access(a[1]);check(x.stat("age_writeback_bytes")==32,"exact selected threshold");x.audit();check(x.obs()["selected_clock"]=="set","explicit selected clock");}
 {Cache x(Clock::SET_FORWARDED_LINE,3);x.access(a[0],4,true);x.access(a[1]);x.access(a[1]);x.access(a[0]+32,1,true);check(x.stat("age_writeback_bytes")==0,"store wins expiry tick whole line");x.access(a[1]);x.access(a[1]);check(x.stat("age_writeback_bytes")==0,"whole line timer refresh retained sector0");x.access(a[1]);check(x.stat("age_writeback_bytes")==64&&x.masks==std::vector<std::uint32_t>({15,1}),"whole line age two partial masks");x.access(a[0]+1,1,true);check(x.stat("age_redirty_retained_lines")==1,"masked redirty");x.access(a[0],4);check(x.stat("DRAM_read_merge_bytes")==32,"partial knowledge persists until read merge");check(x.stat("DRAM_store_RFO_bytes")==0,"no store RFO");x.audit();}
 {Cache x(Clock::SET_FORWARDED_LINE,16);x.access(a[0],4,true);for(int i=1;i<=16;++i)x.access(a[i]);check(x.stat("capacity_eviction_writeback_bytes")==32&&x.stat("age_writeback_bytes")==0,"capacity victim before same-tick age");for(int i=0;i<20;++i)x.access(a[19]);check(x.stat("DRAM_write_bytes")==32,"capacity removed queue entry no duplicate");x.audit();}
 {Cache x(Clock::SET_FORWARDED_LINE,100);x.access(a[0],4,true);x.access(b,32,true);auto before=x.obs();const auto ticks=x.stat("L2_forwarded_access_sequence"),r=x.stat("DRAM_read_bytes"),created=x.stat("dirty_sector_creations");x.drain();auto after=x.obs();check(before["group_ticks"]==after["group_ticks"]&&x.stat("L2_forwarded_access_sequence")==ticks,"drain does not advance either clock");check(x.stat("DRAM_read_bytes")==r&&x.stat("dirty_sector_creations")==created&&x.stat("drain_writeback_bytes")==64&&x.stat("resident_dirty_sectors")==0,"drain independent ledger");x.access(a[0],4);check(x.stat("DRAM_read_merge_bytes")==32,"drain retains partial known mask");x.access(b);check(x.stat("DRAM_read_bytes")==32,"drain retains full known sector");}
 {Cache x(Clock::SET_FORWARDED_LINE,0),g(Clock::GLOBAL_FORWARDED_LINE,0);for(auto* c:{&x,&g}){c->access(a[0],4,true);for(int i=0;i<12;++i)c->access(b);c->audit();}check(x.stat("age_writeback_bytes")==0&&x.obs()["dirty_queue_entries"]==0,"age0 stays disabled");check(x.c->snapshot()==g.c->snapshot(),"age0 exact scalar equality");check(x.events.size()==g.events.size(),"age0 events same count");for(unsigned i=0;i<x.events.size();++i)check(native_trace::words(x.events[i])==native_trace::words(g.events[i]),"age0 ordered events exact");}
}
void random_projection(){
 const auto a=colliding(4096,24),b=colliding(4224,24);std::mt19937 rng(174129);Cache global(Clock::GLOBAL_FORWARDED_LINE,31),local(Clock::SET_FORWARDED_LINE,13),off(Clock::SET_FORWARDED_LINE,0);
 global.ef=local.ef=off.ef=true;
 for(unsigned i=0;i<2500;++i){const U addr=(rng()%2?a:b)[rng()%24]+rng()%96,n=1+rng()%32;const bool atomic=rng()%31==0,write=atomic||rng()%3==0,bypass=rng()%7!=0;
  for(auto* c:{&global,&local,&off}){if(i%97==0)c->c->begin_kernel();c->access(addr,n,write,atomic,bypass);}
  if(i%13==0)for(auto* c:{&global,&local,&off})c->audit();
  if(i%199==0){const auto reference=off.c->snapshot();for(auto* c:{&global,&local}){const auto s=c->c->snapshot();for(auto it=reference.begin();it!=reference.end();++it){const auto& k=it.key();if(k=="DRAM_read_bytes"||k=="DRAM_read_requests"||k=="source_projection_fnv1a64"||k=="L2_hits"||k=="L2_sector_read_hits"||k=="L2_sector_read_misses"||k.rfind("L1_",0)==0||(k.rfind("DRAM_",0)==0&&k!="DRAM_write_bytes"&&k!="DRAM_write_requests"))check(s.at(k)==it.value(),"read counters invariant under age");}}}
 }
 check(global.read_projection()==local.read_projection()&&global.read_projection()==off.read_projection(),"ordered read address and identity projection exact across clocks");
 for(auto* c:{&global,&local,&off}){c->audit();auto before=c->obs();const auto dirty=c->stat("resident_dirty_sectors")*32,w=c->stat("DRAM_write_bytes"),r=c->stat("DRAM_read_bytes");c->drain();check(c->stat("DRAM_write_bytes")==w+dirty&&c->stat("DRAM_read_bytes")==r,"random final drain exact");if(c!=&global)check(before["group_ticks"]==c->obs()["group_ticks"],"random drain no group ticks");}
}
void frozen_sector32_global_equivalence(){
 for(U budget:{U{0},U{31}}){
  direct_native::DiagnosticOptions current_options;current_options.ef_hit_rate=288;current_options.dirty_age_accesses=budget;current_options.data_policy=direct_native::DataPolicy::SECTOR32;
  frozen_sector32::DiagnosticOptions frozen_options;frozen_options.ef_hit_rate=288;frozen_options.dirty_age_accesses=budget;frozen_options.data_policy=frozen_sector32::DataPolicy::SECTOR32;
  std::vector<native_trace::Record> actual,expected;
  direct_native::FunctionalCache current(llm_l1::config(32768),41943040,[](int,U a){return a;},[&](auto r){actual.push_back(r);},geometry,current_options);
  frozen_sector32::FunctionalCache frozen(llm_l1::config(32768),41943040,[](int,U a){return a;},[&](auto r){expected.push_back(r);},geometry,frozen_options);
  current.allocation_metadata(allocation());frozen.allocation_metadata(allocation());
  const auto a=colliding(4096,24),b=colliding(4224,24);std::mt19937 rng(925023);
  for(unsigned i=0;i<800;++i){if(i%97==0){current.begin_kernel();frozen.begin_kernel();}const U addr=(rng()%2?a:b)[rng()%24]+rng()%96,n=1+rng()%32;const bool atomic=rng()%31==0,write=atomic||rng()%3==0,bypass=rng()%7!=0;
   direct_native::Context c;c.sm_id=i%48;c.call_index=i;c.cta=i%17;c.warp=i%4;c.pc=i%11*32;c.node_id=i/97;c.atomic_rmw=atomic;c.low_priority=i%2;
   frozen_sector32::Context f;f.sm_id=c.sm_id;f.call_index=c.call_index;f.cta=c.cta;f.warp=c.warp;f.pc=c.pc;f.node_id=c.node_id;f.atomic_rmw=c.atomic_rmw;f.low_priority=c.low_priority;
   current.instruction(0,write,bypass,{sub(addr,n)},c);frozen.instruction(0,write,bypass,{sub(addr,n)},f);
   check(current.snapshot()==frozen.snapshot(),"frozen sector32 global complete scalar regression");
  }
  const auto current_age=current.dirty_age_observation(),frozen_age=frozen.dirty_age_observation();for(auto it=frozen_age.begin();it!=frozen_age.end();++it)check(current_age.at(it.key())==it.value(),"frozen global legacy age observation exact");
  check(actual.size()==expected.size(),"frozen sector32 global record count");for(unsigned i=0;i<actual.size();++i)check(native_trace::words(actual[i])==native_trace::words(expected[i]),"frozen sector32 exact ordered mixed records");
 }
}
int main(){try{directed();random_projection();frozen_sector32_global_equivalence();std::cout<<J({{"status","PASS_SET_AGE_COMPONENTS"},{"checks",checks},{"profile",llm_l1::profile()},{"full_model_or_GPU_test",false},{"clock_queue_group_count",20480},{"sizeof_list",sizeof(std::list<direct_native::CacheKey>)},{"set_vector_payload_bytes",20480*(2*sizeof(U)+sizeof(std::list<direct_native::CacheKey>))}}).dump()<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
