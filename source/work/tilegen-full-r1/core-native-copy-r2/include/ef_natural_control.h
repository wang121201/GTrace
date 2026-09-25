#pragma once
#include "ef_insertion.h"
namespace ef_natural_control {
inline bool requested=false;
inline bool zero(const GTSim::ef_insertion::Observation&o){return !o.completed_fill_candidates&&!o.inspected_waiters&&!o.pure_observed_ef&&!o.fallback&&!o.inserted_lru&&!o.closed&&!o.abandoned;}
template<class J>J counters(const GTSim::ef_insertion::Observation&o){return {{"completed_fill_candidates",o.completed_fill_candidates},{"inspected_waiters",o.inspected_waiters},{"pure_observed_ef",o.pure_observed_ef},{"fallback",o.fallback},{"inserted_lru",o.inserted_lru},{"closed",o.closed},{"abandoned",o.abandoned}};}
template<class J>J description(GTSim::Cycle begin,GTSim::Cycle end,bool full=false){return {{"schema",full?"FULL1138_FIXED_EF_D1_D2_POLICY_V1":"NATURAL773_FIXED_EF_D1_POLICY_V1"},{"enabled",requested},{"model_behavior_changed",requested},{"first_call",408},{"last_call",full?1137:772},{"prefill_policy_active",false},{"entry_cycle",begin},{"end_cycle",end},{"rule","completed new-line fills: all actual MSHR waiters Known observed-ExactEF reads insert at existing group LRU; otherwise original MRU; hits unchanged"},{"resolved_L1_and_explicit_L2","Unknown"},{"hardware_policy_qualified",false},{"hardware_gap_fixed",false},{"cache_reset",false},{"final_dirty_flush",false}};}
}
