#pragma once
#include "work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h"
#include "work/tilegen-full-r1/core-native-copy-r2/include/ada_r4_profile.h"
#include "legacy_per_sm_l1.h"
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <climits>
#include <nlohmann/json.hpp>

namespace llm_l1 {
using U=std::uint64_t;using J=nlohmann::json;
inline void need(bool ok,const char* message){if(!ok)throw std::invalid_argument(message);}
inline U integer(const J& v){need(v.is_number_integer()&&!v.is_boolean(),"allocation integer required");if(!v.is_number_unsigned())need(v.get<std::int64_t>()>=0,"negative allocation integer");return v.get<U>();}
inline const std::string& profile(){static const std::string value=[](){const char* p=std::getenv("TILEGEN_ADA_L1_PROFILE");std::string v=p?p:"legacy32";need(v=="legacy32"||v=="r2"||v=="r4","TILEGEN_ADA_L1_PROFILE must be legacy32/r2/r4");return v;}();return value;}
inline bool require_observed(){static const bool v=[](){const char* p=std::getenv("TILEGEN_ADA_REQUIRE_OBSERVED");if(!p)return false;need(std::string(p)=="1","TILEGEN_ADA_REQUIRE_OBSERVED must be 1 when set");need(std::getenv("TILEGEN_ADA_SHARED_BYTES")==nullptr,"observed-only mode forbids assumed shared env");return true;}();return v;}
inline std::string initial_origin(){return profile()=="legacy32"?"not_applied_legacy32":require_observed()?"bootstrap_before_first_kernel_no_shared_claim":"assumed_explicit_environment_not_observed";}
inline unsigned assumed_shared(){static const unsigned value=[](){const char* p=std::getenv("TILEGEN_ADA_SHARED_BYTES");if(!p){if(require_observed())return 32768U;need(profile()=="legacy32","r2/r4 require explicit TILEGEN_ADA_SHARED_BYTES");return 32768U;}std::string v=p;need(v=="8192"||v=="16384"||v=="32768"||v=="65536"||v=="102400","shared carveout requires 8192/16384/32768/65536/102400 bytes");return unsigned(std::stoul(v));}();return value;}
inline GTSim::PerSmL1Config config(unsigned shared){
 GTSim::PerSmL1Config c;
 if(profile()=="legacy32"){c.mode=GTSim::PerSmL1Mode::MODELED_SET_ASSOCIATIVE;c.persistence=GTSim::PerSmL1Persistence::KERNEL_FLUSH;c.capacity_bytes_per_sm=32768;c.ways=64;c.line_bytes=128;c.sector32=true;}
 else if(shared==8192||shared==16384){c=GTSim::make_ada_r4_serial_l1(32768,profile()=="r4");const unsigned sets=profile()=="r4"?16:4,scale=profile()=="r4"?1062:1000;c.ways=std::uint64_t(131072-shared)*scale/1000/(128*sets);c.capacity_bytes_per_sm=std::uint64_t(c.ways)*128*sets;}
 else c=GTSim::make_ada_r4_serial_l1(shared,profile()=="r4");
 c.num_sms=48;c.store_bypass=true;return c;
}
inline J description(const GTSim::PerSmL1Config& c,unsigned shared,const std::string& origin){return {{"profile",profile()},{"bytes_per_SM",c.capacity_bytes_per_sm},{"SMs",c.num_sms},{"sets",c.capacity_bytes_per_sm/(c.line_bytes*c.ways)},{"ways",c.ways},{"line_bytes",128},{"validity_bytes",32},{"store_bypass",true},{"write_allocate",false},{"persistence","KERNEL_FLUSH"},{"replacement",GTSim::per_sm_l1_replacement_name(c.replacement)},{"hash",GTSim::per_sm_l1_hash_name(c.hash_policy)},{"shared_carveout_bytes",shared},{"shared_carveout_origin",origin},{"shared_is_per_CTA_dynamic_bytes",false},{"shared_capacity_qualification",profile()=="legacy32"?"legacy32_not_adaptive":(shared==8192||shared==16384)?"uncalibrated_extrapolation_8_16KiB":"original_serial_read_calibrated_bin_extrapolated_to_LLM"},{"scope","functional serial caller order; CTA mod48 modeled SM; r4 read-filter model extrapolation; stores preserve original bypass"}};}
struct Ticket {bool valid=false;llm_legacy_l1::ReadFillTicket legacy;GTSim::ReadFillTicket current;};
struct Decision {bool forwarded_to_l2=true;Ticket read_ticket;};
class Adapter {
 struct Allocation {U base,bytes,generation;int id;};
 GTSim::PerSmL1Config config_;std::unique_ptr<llm_legacy_l1::PerSmL1Cache> legacy_;std::unique_ptr<GTSim::PerSmL1Cache> current_;
 std::map<U,Allocation> allocations_;int next_id_=1;
 U observations_=0,allocates_=0,frees_=0,null_frees_=0,resolved_=0,bypasses_=0,misses_=0;
 U covered_min_=UINT64_MAX,covered_max_=0;unsigned shared_=assumed_shared();std::string origin_=initial_origin();
 llm_legacy_l1::PerSmL1Statistics sectors_{};
 static unsigned pop(unsigned n){return unsigned(__builtin_popcount(n));}
 const Allocation& resolve(U line,std::uint8_t mask){auto it=allocations_.upper_bound(line);if(it==allocations_.begin()){++misses_;throw std::invalid_argument("L1 address has no observed active driver allocation");}--it;const auto& a=it->second;unsigned last=0;for(unsigned s=0;s<4;++s)if(mask&(1U<<s))last=s;const U end=line+last*32+31;need(end>=line,"L1 allocation range overflow");if(line<a.base||end-a.base>=a.bytes){++misses_;throw std::invalid_argument("L1 sector exceeds observed driver allocation");}++resolved_;covered_min_=std::min(covered_min_,line);covered_max_=std::max(covered_max_,end);return a;}
public:
 explicit Adapter(const GTSim::PerSmL1Config& c):config_(c){
  if(profile()=="legacy32"){llm_legacy_l1::PerSmL1Config l;l.mode=llm_legacy_l1::PerSmL1Mode::MODELED_SET_ASSOCIATIVE;l.persistence=llm_legacy_l1::PerSmL1Persistence::KERNEL_FLUSH;l.num_sms=c.num_sms;l.capacity_bytes_per_sm=c.capacity_bytes_per_sm;l.ways=c.ways;l.line_bytes=c.line_bytes;l.store_bypass=true;l.sector_validity=true;legacy_=std::make_unique<llm_legacy_l1::PerSmL1Cache>(l);}
  else current_=std::make_unique<GTSim::PerSmL1Cache>(c);
 }
 const auto& config()const{return config_;}
 void allocation(const J& node){
  need(node.at("kind")=="allocation_API_observation","allocation node kind");need(integer(node.at("return_event"))>integer(node.at("submission_event")),"allocation return ordering");need(node.contains("raw_return")&&node.contains("raw_before"),"allocation raw provenance required");
  const auto& o=node.at("observation");need(o.at("generation_semantics")=="host_API_return_not_device_completion","allocation semantics");need(o.at("async").is_boolean(),"allocation async boolean");const auto action=o.at("action").get<std::string>();const U base=integer(o.at("base_u64"));
  if(action=="allocate"){
   need(o.at("cuda_api")=="cuMemAlloc_v2"&&!o.at("async").get<bool>(),"only qualified synchronous cuMemAlloc_v2 driver allocations supported");
   const U bytes=integer(o.at("bytes")),gen=integer(o.at("allocation_generation"));need(base&&base%128==0&&bytes&&base<=UINT64_MAX-(bytes-1)&&gen,"allocation extent/generation");need(next_id_<INT_MAX,"allocation ID overflow");auto hi=allocations_.lower_bound(base);need(hi==allocations_.end()||base+bytes-1<hi->second.base,"overlapping observed driver allocation");if(hi!=allocations_.begin()){const auto& prev=std::prev(hi)->second;need(prev.base+prev.bytes-1<base,"overlapping observed driver allocation");}allocations_.emplace(base,Allocation{base,bytes,gen,next_id_++});++allocates_;
  }else if(action=="free"){
   need(o.at("cuda_api")=="cuMemFree_v2"&&!o.at("async").get<bool>(),"only qualified synchronous cuMemFree_v2 driver frees supported");
   if(base==0){need(o.at("allocation_generation").is_null()&&o.at("bytes").is_null(),"null free provenance");++null_frees_;}
   else{auto it=allocations_.find(base);need(it!=allocations_.end()&&o.at("free_matched_observed_generation")==true,"free lacks observed driver allocation");need(integer(o.at("allocation_generation"))==it->second.generation,"free generation differs");need(integer(o.at("bytes"))==it->second.bytes,"free extent differs");allocations_.erase(it);++frees_;}
  }else throw std::invalid_argument("unknown allocation action");++observations_;
 }
 void begin_kernel(const J& metadata){
  need(!require_observed()||metadata.contains("observed_shared_bytes"),"observed-only L1 mode requires every kernel shared witness");
  if(metadata.contains("observed_shared_bytes")){const U supplied=integer(metadata.at("observed_shared_bytes"));need(supplied==8192||supplied==16384||supplied==32768||supplied==65536||supplied==102400,"unsupported observed shared carveout");shared_=unsigned(supplied);origin_=profile()=="legacy32"?"not_applied_legacy32":"observed_shared_bytes_explicit_caller_witness";}
  else{shared_=assumed_shared();origin_=profile()=="legacy32"?"not_applied_legacy32":"assumed_explicit_environment_not_observed";}
  if(legacy_)legacy_->begin_kernel();else{auto c=llm_l1::config(shared_);current_->configure_capacity_bytes_per_sm(c.capacity_bytes_per_sm,c.ways);config_=c;current_->begin_kernel();}
 }
 Decision access(const GTSim::PerSmL1Access& input){
  if(legacy_){const auto d=legacy_->access({input.sm_id,input.allocation_id,input.canonical_line,input.is_write,input.node_id,input.bypass_l1,input.sector_mask});Decision out;out.forwarded_to_l2=d.forwarded_to_l2;out.read_ticket.valid=d.read_ticket.valid;out.read_ticket.legacy=d.read_ticket;return out;}
  const unsigned count=pop(input.sector_mask);need(count>0,"explicit L1 sector mask");auto& requested=input.is_write?sectors_.write_sector_requests:sectors_.read_sector_requests;requested+=count;
  if(input.bypass_l1||(config_.store_bypass&&input.is_write)){
   ++bypasses_;++sectors_.pre_l1_transactions;++sectors_.bypassed_transactions;++sectors_.l2_input_transactions;
   ++(input.is_write?sectors_.pre_l1_writes:sectors_.pre_l1_reads);
   (input.is_write?sectors_.bypassed_write_sector_requests:sectors_.bypassed_read_sector_requests)+=count;
   (input.is_write?sectors_.write_sector_misses:sectors_.read_sector_misses)+=count;
   (input.is_write?sectors_.forwarded_write_sector_requests:sectors_.forwarded_read_sector_requests)+=count;return {};
  }
  auto a=input;const auto& allocation=resolve(a.canonical_line,a.sector_mask);a.allocation_id=allocation.id;
  if(config_.hash_policy==GTSim::PerSmL1HashPolicy::ALLOCATION_RELATIVE_HASH2)a.allocation_relative_byte_offset=a.canonical_line-allocation.base;
  const auto d=current_->access(a);const auto forwarded=pop(d.forwarded_sector_mask);
  sectors_.forwarded_read_sector_requests+=forwarded;sectors_.read_sector_misses+=forwarded;sectors_.read_sector_hits+=count-forwarded;
  Decision out;out.forwarded_to_l2=d.forwarded_to_l2;out.read_ticket.valid=d.read_ticket.valid;out.read_ticket.current=d.read_ticket;return out;
 }
 bool complete_read(const Ticket& t){return legacy_?legacy_->complete_read(t.legacy):current_->complete_read(t.current);}
 U live_read_tickets()const{return legacy_?legacy_->live_read_tickets():current_->live_read_tickets();}
 auto statistics()const{
  if(legacy_)return legacy_->statistics();auto out=sectors_;const auto n=current_->statistics();
#define ADD(field) out.field+=n.field
  ADD(pre_l1_transactions);ADD(pre_l1_reads);ADD(pre_l1_writes);ADD(bypassed_transactions);ADD(read_hits);ADD(read_misses);ADD(write_hits);ADD(write_misses);ADD(filtered_reads);ADD(l2_input_transactions);ADD(evictions);ADD(kernel_flushes);ADD(flushed_lines);ADD(peak_resident_lines);ADD(final_resident_lines);
#undef ADD
  out.decision_order_fnv1a64=n.decision_order_fnv1a64;return out;
 }
 J configuration()const{return description(config_,shared_,origin_);}
 J observation()const{return {{"driver_allocation_observations",observations_},{"driver_allocates",allocates_},{"driver_frees",frees_},{"driver_null_frees",null_frees_},{"active_driver_allocations",allocations_.size()},{"L1_allocation_resolved_line_accesses",resolved_},{"L1_allocation_unresolved_line_accesses",misses_},{"L1_hash_not_required_bypass_line_accesses",bypasses_},{"L1_covered_min_line",resolved_?J(covered_min_):J(nullptr)},{"L1_covered_max_sector_byte",resolved_?J(covered_max_):J(nullptr)},{"complete_suballocator_lifetime",false},{"allocation_scope","observed CUDA driver allocation generations; not tensor or suballocator roots"},{"allocation_or_free_flushes_L2",false},{"L1_adapter_statistics_scope","global_only; per_SM counters not exported"},{"shared_observed_required",require_observed()},{"configuration",configuration()}};}
};
}
