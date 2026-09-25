#pragma once
// Raw evidence transport only. No hint interpretation or cache policy.
#include "source_semantics.h"
#include <limits>

namespace GTSim::source_service {
inline constexpr std::uint64_t unknown_id=std::numeric_limits<std::uint64_t>::max();
enum class Stage : std::uint8_t { Accepted=1, Decision=2, FillWaiter=3 };
struct Event {
    Stage stage;
    source_memory::View source;
    std::uint64_t call_id,accept_sequence,fill_sequence;
    std::int64_t cycle;
    int node_id,matrix_id;
    std::uint64_t line_addr;
    bool is_write,forwarded;
    std::uint8_t outcome; // original L2DecisionKind, or 0 outside decision
};
class Observer {
public:
    virtual ~Observer()=default;
    // Synchronous borrowed event. Do not mutate the cache or retain an event,
    // node/span, or Record beyond its immutable SourceCatalog lifetime.
    virtual void observe(const Event&)=0;
    virtual void scope_closed(bool complete) noexcept=0;
};
struct Binding {const void* owner=nullptr;Observer* observer=nullptr;std::uint64_t call=unknown_id;};
inline thread_local Binding active;
class Scope final {
    bool bound_=false;
    const void* owner_=nullptr;
    Observer* observer_=nullptr;
    bool(*quiescent_)(const void*)=nullptr;
public:
    template<class Cache> Scope(Observer& observer,const Cache* owner,std::uint64_t call)
        :owner_(owner),observer_(&observer),quiescent_([](const void* p){return static_cast<const Cache*>(p)->is_quiescent();}) {
        if(!owner||call==unknown_id||!owner->is_quiescent())
            throw std::invalid_argument("source scope owner/call/quiescence");
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
        if(active.owner)throw std::logic_error("source scope already active");
        active={owner,&observer,call};bound_=true;
#else
        (void)observer;
#endif
    }
    Scope(const Scope&)=delete;Scope&operator=(const Scope&)=delete;
    Scope(Scope&&)=delete;Scope&operator=(Scope&&)=delete;
    void close() {
        if(!quiescent_(owner_))throw std::logic_error("source scope must drain before close");
        if(bound_){active={};bound_=false;observer_->scope_closed(true);}
    }
    ~Scope(){if(bound_){active={};observer_->scope_closed(false);}}
};
inline std::uint64_t call_id(const void* owner) noexcept {
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
    return active.owner==owner?active.call:unknown_id;
#else
    (void)owner;return unknown_id;
#endif
}
inline void emit(const void* owner,const Event& event) {
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
    if(active.owner==owner&&active.observer)active.observer->observe(event);
#else
    (void)owner;(void)event;
#endif
}
}
