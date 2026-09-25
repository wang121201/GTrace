#pragma once
// Fixed counterfactual: pure observed-EF completed read fills insert at LRU.
// No claim that this is the hardware meaning of the raw SASS marker.
#include "source_semantics.h"
#include <limits>

#ifndef TILEGEN_EF_INSERTION_COUNTERFACTUAL
#define TILEGEN_EF_INSERTION_COUNTERFACTUAL 0
#endif
#if TILEGEN_EF_INSERTION_COUNTERFACTUAL != 0 && TILEGEN_EF_INSERTION_COUNTERFACTUAL != 1
#error TILEGEN_EF_INSERTION_COUNTERFACTUAL must be 0 or 1
#endif
#if TILEGEN_EF_INSERTION_COUNTERFACTUAL && !TILEGEN_SOURCE_MEMORY_SEMANTICS
#error EF insertion counterfactual requires qualified raw source transport
#endif

namespace GTSim::ef_insertion {
struct Observation {
    std::uint64_t completed_fill_candidates=0, inspected_waiters=0;
    std::uint64_t pure_observed_ef=0, fallback=0, inserted_lru=0;
    bool closed=false, abandoned=false;
};
struct Binding {const void* owner=nullptr;Observation* observation=nullptr;bool enabled=false;};
inline thread_local Binding active;
inline void add(std::uint64_t& value,std::uint64_t n=1) {
    if(n>std::numeric_limits<std::uint64_t>::max()-value)
        throw std::overflow_error("EF insertion observation overflow");
    value+=n;
}
class Scope final {
    const void* owner_;Observation& observation_;bool bound_=false;
    bool(*quiescent_)(const void*);
public:
    template<class Cache> Scope(Observation& observation,const Cache* owner,bool enabled=false)
        :owner_(owner),observation_(observation),
         quiescent_([](const void* p){return static_cast<const Cache*>(p)->is_quiescent();}) {
        if(!owner||!owner->is_quiescent()||observation.closed||observation.abandoned)
            throw std::invalid_argument("EF insertion scope needs live quiescent owner and fresh observation");
#if TILEGEN_EF_INSERTION_COUNTERFACTUAL
        if(active.owner)throw std::logic_error("EF insertion scope already active");
        active={owner,&observation,enabled};bound_=true;
#else
        (void)enabled;
#endif
    }
    Scope(const Scope&)=delete;Scope&operator=(const Scope&)=delete;
    Scope(Scope&&)=delete;Scope&operator=(Scope&&)=delete;
    void close() {
        if(!quiescent_(owner_))throw std::logic_error("EF insertion scope must drain before close");
        if(bound_){active={};bound_=false;}
        observation_.closed=true;
    }
    ~Scope(){if(bound_){active={};observation_.abandoned=true;}}
};
template<class Waiting> bool pure_completed_read_fill(const void* owner,const Waiting& waiting) {
#if TILEGEN_EF_INSERTION_COUNTERFACTUAL
    if(active.owner!=owner||!active.enabled)return false;
    auto& o=*active.observation;
    add(o.completed_fill_candidates);add(o.inspected_waiters,waiting.size());
    bool pure=!waiting.empty();
    for(const auto& tx:waiting) {
        const auto& view=tx.source_evidence;
        const auto* r=view.record;
        pure=pure&&!tx.is_write&&view.selection==source_memory::Selection::Known&&r&&
             r->operation=='R'&&r->observed_sass==source_memory::ObservedSass::ExactEF;
    }
    add(pure?o.pure_observed_ef:o.fallback);
    return pure;
#else
    (void)owner;(void)waiting;return false;
#endif
}
inline void inserted(const void* owner) {
#if TILEGEN_EF_INSERTION_COUNTERFACTUAL
    if(active.owner!=owner||!active.enabled)throw std::logic_error("unbound EF insertion");
    add(active.observation->inserted_lru);
#else
    (void)owner;throw std::logic_error("EF insertion disabled");
#endif
}
} // namespace GTSim::ef_insertion
