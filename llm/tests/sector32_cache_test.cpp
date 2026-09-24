#include "runner.h"
#include "frozen_candidate_direct_cache.h"
#include <iostream>
#include <sstream>
#include <random>
using U=std::uint64_t;using J=nlohmann::json;
unsigned checks=0;void check(bool v,const char* m){++checks;if(!v)throw std::runtime_error(m);}
J allocation(){return {{"kind","allocation_API_observation"},{"submission_event",1},{"return_event",2},{"raw_before",J::object()},{"raw_return",J::object()},{"observation",{{"action","allocate"},{"cuda_api","cuMemAlloc_v2"},{"base_u64",4096},{"bytes",1<<20},{"allocation_generation",1},{"async",false},{"generation_semantics","host_API_return_not_device_completion"}}}};}
GTSim::ExplicitMemorySubop sub(U a,U n){GTSim::ExplicitMemorySubop s;s.requested_bytes=n;s.ranges.push_back({0,a,n});return s;}
struct Cache {
 std::vector<native_trace::Record> events;std::vector<std::uint32_t> masks;
 std::unique_ptr<direct_native::FunctionalCache> c;U ordinal=0;
 Cache(U capacity=1024,U age=0,direct_native::DataPolicy policy=direct_native::DataPolicy::SECTOR32){
  direct_native::DiagnosticOptions o;o.ef_hit_rate=288;o.dirty_age_accesses=age;o.data_policy=policy;
  c=std::make_unique<direct_native::FunctionalCache>(llm_l1::config(llm_l1::assumed_shared()),capacity,[](int,U a){return a;},[&](const auto& r){events.push_back(r);},GTSim::L2GeometryConfig{},o,direct_native::OwnerObserver{},[&](const auto&,const auto&,auto,std::uint32_t mask){masks.push_back(mask);});
  c->allocation_metadata(allocation());c->begin_kernel();
 }
 void access(U a,U n,bool write=false,bool atomic=false,bool bypass=true){direct_native::Context ctx;ctx.sm_id=0;ctx.call_index=++ordinal;ctx.atomic_rmw=atomic;c->instruction(0,write,bypass,{sub(a,n)},ctx);c->verify_resident_ledger();}
 void drain(){direct_native::Context ctx;ctx.sm_id=0;ctx.call_index=++ordinal;c->drain(ctx);c->verify_resident_ledger();}
 U stat(const char* key){return c->snapshot().at(key).get<U>();}
};
void sector_tests(){
 {Cache x;x.access(4096,4);check(x.stat("DRAM_read_bytes")==32,"cold sector reads32");x.access(4128,4);check(x.stat("DRAM_read_bytes")==64,"adjacent absent sector reads32");x.access(4096,64);check(x.stat("DRAM_read_bytes")==64&&x.stat("L2_sector_read_hits")==2,"read two known sectors");}
 {Cache x;x.access(4096,4,true);check(x.stat("DRAM_read_bytes")==0&&x.stat("resident_dirty_sectors")==1,"partialstore lazy noRFO");x.access(4096,4);check(x.stat("DRAM_read_merge_bytes")==32&&x.stat("resident_dirty_sectors")==1,"partialread merges preservingdirty");x.drain();check(x.stat("DRAM_write_bytes")==32&&x.stat("DRAM_read_bytes")==32,"merged sector drain no reread");check(x.masks==std::vector<std::uint32_t>{15},"read merge does not expand dirty mask");}
 {Cache x;for(U i=0;i<4;++i)x.access(4096+i*8,8,true);x.access(4096,32);check(x.stat("DRAM_read_bytes")==0,"accumulatedfullsector readable withoutfetch");x.drain();check(x.stat("DRAM_write_bytes")==32&&x.stat("writeback_enabled_byte_coverage")==32,"fullsector WB32");}
 {Cache x;for(U i=0;i<4;++i)x.access(4096+i*32,4,true);x.access(4104,4);check(x.stat("DRAM_read_bytes")==32,"allsector touched is not allbytesknown");check(x.stat("resident_dirty_sectors")==4,"readmerge preserves all dirty");}
 {Cache x(128);x.access(4096,4,true);x.access(4224,4);check(x.events.size()==2&&x.events[0].cause==native_trace::Cause::ReadFillOrRfo&&x.events[1].cause==native_trace::Cause::DirtyWriteback,"capacity read before maskedWB");check(x.stat("DRAM_read_bytes")==32&&x.stat("capacity_eviction_writeback_bytes")==32,"partialeviction noRFO");check(x.masks[0]==15&&x.stat("masked_writeback_requests")==1,"eviction preserves4byte mask");}
 {Cache x(1024,3);x.access(4096,4,true);for(unsigned i=0;i<3;++i)x.access(4224,4);check(x.stat("age_writeback_bytes")==32&&x.stat("DRAM_read_bytes")==32,"age threshold maskedWB");x.access(4096,4);check(x.stat("DRAM_read_merge_bytes")==32&&x.stat("resident_dirty_sectors")==0,"age clean incomplete known stays unreadable");}
 {Cache x(1024,3);x.access(4096,4,true);x.access(4224,4);x.access(4224,4);x.access(4097,1,true);check(x.stat("age_writeback_bytes")==0,"last store refresh wins same expiry tick");for(unsigned i=0;i<3;++i)x.access(4224,4);check(x.stat("age_writeback_bytes")==32,"refreshed expiry later");}
 {Cache x;x.access(4096,4,true,true);check(x.stat("DRAM_atomic_read_bytes")==32&&x.stat("resident_dirty_sectors")==1,"cold atomic reads beforestore");x.access(4096,4,true,true);check(x.stat("DRAM_atomic_read_bytes")==32,"residentfull atomic no duplicatefill");x.drain();check(x.stat("DRAM_write_bytes")==32&&x.masks[0]==15,"atomic dirty target bytes only");}
 {Cache x;x.access(4096,4,true);x.access(4096,4,true,true);check(x.stat("DRAM_atomic_read_bytes")==32,"atomic incomplete sector reads32");}
 {Cache x;x.access(4096,4,true);x.access(4224,32,true);check(x.stat("DRAM_write_bytes")==0,"no implicit end drain");x.drain();check(x.stat("DRAM_write_bytes")==64&&x.stat("DRAM_read_bytes")==0&&x.stat("writeback_enabled_byte_coverage")==36,"drain counts bus32 and enabledbytes separately");x.access(4096,4);check(x.stat("DRAM_read_merge_bytes")==32,"drain partial retains incompleteknowledge");x.access(4224,4);check(x.stat("DRAM_read_bytes")==32,"drain full retainsknown");auto n=x.events.size();x.drain();check(x.events.size()==n,"empty drain no traffic");x.access(4096,1,true);check(x.stat("dirty_sector_creations")==3,"redirty new sector lifetime");}
 {Cache x(128);x.access(4096,4,false,false,false);x.access(4224,4);x.access(4096,64,false,false,false);check(x.stat("DRAM_read_bytes")==96,"partial L1 hit forwards only missingsector");check(x.stat("L2_sector_read_misses")==3,"partialforward sectorcount");}
 {Cache x;x.access(4096,128,true);check(x.stat("DRAM_read_bytes")==0,"full128store noRFO");x.drain();check(x.stat("DRAM_write_bytes")==128&&x.stat("writeback_enabled_byte_coverage")==128,"full128WB four32");}
 {Cache x;x.access(4216,16,true);check(x.stat("resident_dirty_sectors")==2&&x.stat("DRAM_read_bytes")==0,"crossline partial lazy");x.access(4216,16);check(x.stat("DRAM_read_merge_bytes")==64,"crossline two32merges");x.drain();check(x.stat("writeback_enabled_byte_coverage")==16,"crossline exactdirtybytes");}
}
void old_equivalence(){
 if(llm_l1::profile()!="legacy32")return;
 direct_native::DiagnosticOptions o;o.ef_hit_rate=288;o.dirty_age_accesses=3;o.data_policy=direct_native::DataPolicy::OLD128;
 std::vector<native_trace::Record> actual,expected;
 direct_native::FunctionalCache now(llm_l1::config(32768),1024,[](int,U a){return a;},[&](auto r){actual.push_back(r);},GTSim::L2GeometryConfig{},o);
 llm_legacy_l1::PerSmL1Config cfg;cfg.mode=llm_legacy_l1::PerSmL1Mode::MODELED_SET_ASSOCIATIVE;cfg.persistence=llm_legacy_l1::PerSmL1Persistence::KERNEL_FLUSH;cfg.num_sms=48;cfg.capacity_bytes_per_sm=32768;cfg.ways=64;cfg.store_bypass=true;cfg.sector_validity=true;
 baseline_direct_native::DiagnosticOptions bo;bo.ef_hit_rate=288;bo.dirty_age_accesses=3;
 baseline_direct_native::FunctionalCache before(cfg,1024,[](int,U a){return a;},[&](auto r){expected.push_back(r);},GTSim::L2GeometryConfig{},bo);
 std::mt19937 rng(291944);for(unsigned i=0;i<5000;++i){if(i%97==0){now.begin_kernel();before.begin_kernel();}auto range=sub(4096+(rng()%256)*32,1+rng()%80);bool write=rng()%5==0,bypass=rng()%13==0;direct_native::Context c;c.sm_id=i%48;c.call_index=i;c.cta=i%48;c.warp=i%4;c.pc=32*(i%7);c.node_id=i/97;c.low_priority=i%2;baseline_direct_native::Context b;b.sm_id=c.sm_id;b.call_index=c.call_index;b.cta=c.cta;b.warp=c.warp;b.pc=c.pc;b.node_id=c.node_id;b.low_priority=c.low_priority;now.instruction(0,write,bypass,{range},c);before.instruction(0,write,bypass,{range},b);const auto a=now.snapshot(),e=before.snapshot();for(auto it=e.begin();it!=e.end();++it)check(a.at(it.key())==it.value(),"old128 originalcounter drift");}
 check(actual.size()==expected.size(),"old128 recordcount drift");for(unsigned i=0;i<actual.size();++i)check(native_trace::words(actual[i])==native_trace::words(expected[i]),"old128 ordered record drift");
}
void owner_and_cli(){
 std::ostringstream out;source_cache::Runner r(out);
 r.command({{"type","run_begin"},{"schema","TILEGEN_SOURCE_CACHE_STREAM_V1"},{"sm_policy","cta_mod_48"}});
 r.command({{"type","allocation_metadata"},{"node",allocation()}});
 auto begin=[&](U id,const char* phase){r.command({{"type","begin_kernel"},{"id",id},{"phase",phase},{"semantic",std::string("module_")+phase},{"grid",{1,1,1}},{"block",{32,1,1}},{"observed_shared_bytes",32768}});};
 auto store=[&](U a){source_cache::Effect e;e.operation="WRITE";e.width=1;e.global_mask=e.effective=1;e.addresses[0]=a;r.consume_effect(e,{false,false,"output"});};
 begin(1,"Warmup/Prefill");store(4096);r.command({{"type","end_kernel"},{"id",1}});
 begin(2,"Measured/Prefill");store(4097);r.command({{"type","end_kernel"},{"id",2}});
 r.command({{"type","snapshot"},{"label","before_drain"}});auto before=r.snapshot();check(before["DRAM_write_bytes"]==0&&before["dirty_tail_bytes"]==32,"phase boundaries do not flush");
 r.command({{"type","drain"},{"label","diagnostic"},{"phase","Diagnostic/post_Prefill"}});r.command({{"type","run_end"}});
 auto summary=r.summary();check(summary["snapshot"]["DRAM_write_bytes"]==32&&summary["snapshot"]["DRAM_read_bytes"]==0,"runner explicit masked drain");const auto rows=summary["dirty_ownership"]["writebacks_cumulative"];check(rows.size()==1&&rows[0]["first_writer_phase"]=="Warmup/Prefill"&&rows[0]["last_writer_phase"]=="Measured/Prefill"&&rows[0]["trigger_phase"]=="Diagnostic/post_Prefill","crossphase firstlasttrigger");check(rows[0]["enabled_write_byte_coverage"]==2&&rows[0]["write_bytes"]==32&&rows[0]["reason"]=="drain","sectorownership vs byte mask");check(summary["dirty_ownership"]["resident_dirty_bytes"]==0,"drain carry empty");
 bool found=false;std::istringstream lines(out.str());std::string line;while(std::getline(lines,line)){const auto v=J::parse(line);if(v["type"]=="snapshot"){found=true;check(v["dirty_ownership"]["resident_dirty_bytes"]==32,"phase carry snapshot");}}check(found,"snapshot record exists");
}
int main(){try{sector_tests();old_equivalence();owner_and_cli();std::cout<<J({{"status","PASS_SECTOR32_FUNCTIONAL_CACHE"},{"checks",checks},{"profile",llm_l1::profile()},{"full_model_or_GPU_test",false}}).dump()<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
