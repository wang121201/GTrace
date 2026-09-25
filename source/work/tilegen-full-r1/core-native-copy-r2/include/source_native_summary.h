#pragma once
#include "service_source.h"
#include <array>
namespace GTSim::source_service {
// Per serial, quiescent call. No event log, source strings, cache policy, or ownership inference.
struct CallSummary final:Observer {
 std::uint64_t call,closed=0,abandoned=0,forwarded=0,filtered=0;
 std::array<std::array<std::uint64_t,5>,3> selection{};
 std::array<std::uint64_t,3> exact_EF{},known_stores{};
 explicit CallSummary(std::uint64_t c):call(c){}
 void observe(const Event&e)override{
  if(e.call_id!=call)throw std::logic_error("source scope call mismatch");
  const auto stage=unsigned(e.stage)-1,kind=unsigned(e.source.selection);
  if(stage>=3||kind>=5)throw std::logic_error("source summary enum");
  ++selection[stage][kind];
  if(stage==0){forwarded+=e.forwarded;filtered+=!e.forwarded;}
  if(e.source.record){
   const auto&r=*e.source.record;
   if(r.resolved_l1!=source_memory::ResolvedL1Route::Unknown||r.explicit_l2!=source_memory::ExplicitL2Policy::Unknown)throw std::logic_error("unqualified resolved source attribute");
   exact_EF[stage]+=r.observed_sass==source_memory::ObservedSass::ExactEF;known_stores[stage]+=e.is_write;
  }
 }
 void scope_closed(bool complete)noexcept override{if(complete)++closed;else ++abandoned;}
 template<class J>J report(const J&target_key)const{
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
  if(closed!=1||abandoned)throw std::logic_error("source summary not closed");
#endif
  return {{"schema","SERIAL_CALL_RAW_SOURCE_TRANSPORT_V1"},{"call_token",call},{"actual_target_key",target_key},{"enabled",bool(TILEGEN_SOURCE_MEMORY_SEMANTICS)},{"selection_counts_by_stage",selection},{"stages",{"Accepted","Decision","FillWaiter"}},{"selections",{"Known","Missing","Ambiguous","InvalidIndex","Disabled"}},{"observed_exact_EF_by_stage",exact_EF},{"known_stores_by_stage",known_stores},{"forwarded",forwarded},{"L1_filtered",filtered},{"closed",closed},{"abandoned",abandoned},{"template_provenance_remains_in_Record",true},{"resolved_L1_and_explicit_L2","Unknown"},{"concurrent_kernel_identity_qualified",false},{"cache_policy_changed",false}};
 }
};
}
