#include "direct_cache.h"
#include "ada_tuner_profile.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <chrono>
using J=nlohmann::json;using U=std::uint64_t;
namespace g=GTSim;
static U natural(const J& x,const char* field){
 const auto& v=x.at(field);if(!v.is_number_integer()||(!v.is_number_unsigned()&&v.get<std::int64_t>()<0))throw std::invalid_argument(std::string("expected natural ")+field);
 return v.get<U>();
}
static unsigned u32(const J&j,const char*k){U n=natural(j,k);if(n>UINT32_MAX)throw std::invalid_argument(k);return unsigned(n);}
int main(int argc,char**argv){try{
 if(argc<2||argc>3)throw std::invalid_argument("usage: ada_cache_replay source.jsonl [postcache.requests.jsonl]");
 std::ifstream in(argv[1]);if(!in)throw std::runtime_error("cannot read input");
 std::ofstream trace;if(argc==3){std::ifstream exists(argv[2]);if(exists.good())throw std::runtime_error("trace output already exists");trace.open(argv[2]);if(!trace)throw std::runtime_error("cannot create trace");}
 std::unique_ptr<direct_native::FunctionalCache> cache;J phases=J::array(),before;g::AdaKernelResources resources{};J current;
 U kernel_count=0,instructions=0;bool active=false;std::array<U,20> read{},write{};std::string line;U line_number=0;
 auto snapshot_phase=[&]{if(!active)return;auto after=cache->snapshot();J delta;for(const char*k:{"source_read_bytes","source_write_bytes","source_memory_instructions","DRAM_read_bytes","DRAM_write_bytes","DRAM_read_requests","DRAM_write_requests","L1_unmodeled_reservation_stalls"})delta[k]=after.at(k).get<U>()-before.at(k).get<U>();current["traffic"]=delta;current["cache_end"]=after;phases.push_back(current);};
 auto emit=[&](const native_trace::Record&r){auto part=g::AdaAddressMapping::decode(r.source_line_address);if(part.sub_partition!=r.l2_subpartition_id||r.bytes!=32)throw std::logic_error("sector/partition contract mismatch");bool wr=r.cause==native_trace::Cause::DirtyWriteback;(wr?write:read).at(part.sub_partition)+=r.bytes;
  if(trace.is_open())trace<<J({{"schema","GTSIM_ADA_POSTCACHE_SECTOR_V1"},{"sequence",r.source_sequence},{"kernel",r.call_index},{"trigger_cta",r.cta},{"source_allocation_id",r.source_matrix_id},{"source_line_address",r.source_line_address},{"address",r.service_address},{"bytes",r.bytes},{"write",wr},{"channel",part.chip},{"memory_subpartition",part.sub_partition},{"set",part.l2_set},{"bank",part.bank},{"issue_cycle",nullptr}}).dump()<<'\n';};
 while(std::getline(in,line)){
  ++line_number;if(line.empty())continue;auto j=J::parse(line);const auto type=j.at("type").get<std::string>();
  if(type=="kernel"){
   snapshot_phase();resources={u32(j,"threads_per_cta"),u32(j,"registers_per_thread"),u32(j,"shared_bytes_per_cta"),natural(j,"grid_ctas")};auto a=g::AdaTunerProfile::allocate(resources);
   if(!cache)cache=std::make_unique<direct_native::FunctionalCache>(g::AdaTunerProfile::l1(a),g::AdaTunerProfile::l2_bytes,[](int,U address){return address;},emit,g::AdaTunerProfile::l2_geometry());
   else cache->reconfigure_l1(a.l1_bytes,a.l1_ways);
   cache->begin_kernel();before=cache->snapshot();current={{"name",j.at("name")},{"index",kernel_count++},{"launch_resources",j},{"resident_ctas_per_sm",a.resident_ctas_per_sm},{"shared_carveout_bytes",a.shared_carveout_bytes},{"L1_bytes_per_sm",a.l1_bytes},{"L1_ways",a.l1_ways},{"L1_sets",4}};active=true;
  }else if(type=="memory"){
   if(!active)throw std::invalid_argument("memory before kernel");
   U cta=natural(j,"cta"),warp=natural(j,"warp"),matrix=natural(j,"allocation_id");if(cta>=resources.grid_ctas||warp>=(resources.threads_per_cta+31)/32||matrix>INT32_MAX)throw std::invalid_argument("source identity exceeds launch contract");
   U sm=j.contains("sm")?natural(j,"sm"):cta%48;if(sm>=48)throw std::invalid_argument("SM outside 48");
   auto op=j.at("op").get<std::string>();if(op!="read"&&op!="write")throw std::invalid_argument("expected read/write");
   g::ExplicitMemorySubop sub;
   for(const auto&r:j.at("ranges")){
    U addr=natural(r,"address"),bytes=natural(r,"bytes"),lane=natural(r,"lane");
    if(!bytes||lane>=32||addr>UINT64_MAX-(bytes-1)||sub.requested_bytes>UINT64_MAX-bytes)throw std::invalid_argument("invalid source range");
    sub.ranges.push_back({int(lane),addr,bytes});sub.source_member_ordinals.push_back(int(lane));sub.requested_bytes+=bytes;
   }
   if(sub.ranges.empty())throw std::invalid_argument("empty instruction");
   direct_native::Context c;c.call_index=kernel_count-1;c.cta=cta;c.warp=warp;c.sm_id=sm;c.node_id=instructions++;if(j.contains("pc"))c.pc=natural(j,"pc");
   cache->instruction(int(matrix),op=="write",j.value("bypass_l1",false),{sub},c);
  }else throw std::invalid_argument("unknown record type");
 }
 if(!cache)throw std::invalid_argument("input has no kernel");snapshot_phase();cache->verify_resident_ledger();auto totals=cache->snapshot();J partitions=J::array();U rs=0,ws=0;
 for(unsigned p=0;p<20;++p){rs+=read[p];ws+=write[p];partitions.push_back({{"channel",p/2},{"memory_subpartition",p},{"read_bytes",read[p]},{"write_bytes",write[p]}});}
 if(rs!=totals.at("DRAM_read_bytes").get<U>()||ws!=totals.at("DRAM_write_bytes").get<U>())throw std::logic_error("partition byte conservation failed");
 J report={{"schema","GTSIM_ADA_FUNCTIONAL_REPLAY_V1"},{"status","COMPLETED_FUNCTIONAL_ONLY"},{"configuration","accelsim-rtx4000-ada-v1"},{"source_input","explicit instruction/subop byte ranges; arbitrary input is not automatically native-qualified"},{"SM_count",48},{"compute_subpartitions_per_SM",4},{"memory_channels",10},{"memory_subpartitions",20},{"kernel_count",kernel_count},{"source_instructions",instructions},{"phases",phases},{"cache",totals},{"partitions",partitions},{"compute_executed",false},{"HBFSIM_executed",false},{"simulated_latency_ns",nullptr},{"bandwidth_GBps",nullptr},{"NCU_accuracy_tested",false},{"timing_equivalence_to_AccelSim",false},{"SM_assignment","explicit sm if supplied, otherwise declared CTA modulo 48; not observed hardware scheduling"},{"initial_cache","cold"},{"L2_cross_kernel",true},{"final_flush",false},{"unmodeled","inflight MSHR/merge, reservation retry timing, queues, interconnect, hardware replacement order, host memcpy/memset lifecycle and in-kernel membar invalidation"}};
 report["reservation_approximation_observed"]=totals.at("L1_unmodeled_reservation_stalls").get<U>()!=0;
 if(trace.is_open()){trace.flush();if(!trace)throw std::runtime_error("trace write failed");report["trace_format"]="GTSIM_ADA_POSTCACHE_SECTOR_V1; JSONL, not legacy TGCSIM01 read128";}
 std::cout<<report.dump(2)<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
