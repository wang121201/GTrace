#pragma once
// Observation only: no cache lookup, allocation, replacement or completion
// authority. Bounded live 128B write obligations; never a historical trace.
#include "p32_request_mask.h"
#include "per_sm_l1.h"
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace native_p32_observability {
using U = std::uint64_t;
inline void ensure(bool ok, const char* why) {
    if (!ok) throw std::logic_error(why);
}
struct Counter {
    U records=0, sectors=0, unknown=0;
    void add(RequestMask mask) {
        ++records;
        if (!mask.known) { ++unknown; return; }
        ensure(mask.bits && mask.bits < 16, "invalid known request mask");
        for (unsigned bit=0;bit<4;++bit) sectors += (mask.bits>>bit)&1;
    }
};
struct Weights {
    // L1: hit/miss/bypass. L2: hit/miss-allocate/pending-fill-merge.
    std::array<std::array<Counter,3>,2> bins{};
};
struct KernelWeights {
    int kernel_id=-1;
    Weights l1,l2;
    std::array<U,2> source_requests{};
    Counter pending_read_tag, pending_write_tag;
};
struct Ledger {
    U created_pending=0,created_resident=0,pending=0,resident=0,inflight=0,completed=0;
    void verify() const {
        ensure(created_pending+created_resident==pending+resident+inflight+completed,
               "128B model write obligation conservation");
    }
};
struct Key {
    int root=-1; U line=0;
    bool operator==(const Key& k)const{return root==k.root&&line==k.line;}
};
struct KeyHash {
    std::size_t operator()(const Key& k)const {
        return std::hash<U>{}(k.line)^(std::hash<int>{}(k.root)<<1);
    }
};
struct Obligation { Key key; int first_dirty_kernel=-1; bool pending=false; };

class Observation {
public:
    // Upper bound includes dirty residents, pending stores and in-flight WBs.
    // No map for clean lines, read requests or completed historical events.
    static constexpr std::size_t LIVE_CAP=524288, KERNEL_CAP=1647, ROOT_CAP=256;
    KernelWeights total;
    std::map<int,KernelWeights> kernels;
    std::array<Weights,48> sm_l1{},sm_l2{};
    Ledger ledger;
    std::map<int,Ledger> by_root,by_first_dirty_kernel;
    std::map<int,U> writeback_trigger_kernel;
    U peak_live=0,peak_pending=0,peak_resident=0,peak_inflight=0;
    U fill_requests=0,fill_completions=0;
    int phase=-1;

    void begin_kernel(int id) {
        ensure(id>=0&&kernels.size()<KERNEL_CAP&&!kernels.count(id),
               "bounded unique observation kernel");
        ensure(ledger.pending==0&&ledger.inflight==0,"quiescent observation boundary");
        phase=id;kernels[id].kernel_id=id;
    }
    void accepted(RequestMask mask,bool first_request,bool write,int sm,
                  GTSim::PerSmL1Outcome outcome,bool tag_pending) {
        ensure(phase>=0&&sm>=0&&sm<48,"observed real SM/kernel");
        unsigned bin=0;
        using O=GTSim::PerSmL1Outcome;
        if(outcome==O::BYPASS)bin=2;
        else if(outcome==O::READ_MISS||outcome==O::WRITE_MISS)bin=1;
        else ensure((write&&outcome==O::WRITE_HIT)||(!write&&outcome==O::READ_HIT),
                    "L1 observation direction");
        auto& k=kernels.at(phase);
        total.l1.bins[write][bin].add(mask);k.l1.bins[write][bin].add(mask);
        sm_l1[sm].bins[write][bin].add(mask);
        if(first_request){ensure(mask.known,"unknown source request not inferred");
            ++total.source_requests[write];++k.source_requests[write];}
        if(tag_pending) {
            if(write){total.pending_write_tag.add(mask);k.pending_write_tag.add(mask);}
            else {total.pending_read_tag.add(mask);k.pending_read_tag.add(mask);}
        }
    }
    void decided(RequestMask mask,bool write,int sm,unsigned outcome) {
        ensure(phase>=0&&sm>=0&&sm<48&&outcome>=1&&outcome<=3,
               "supported native L2 decision");
        const auto bin=outcome-1;
        total.l2.bins[write][bin].add(mask);kernels.at(phase).l2.bins[write][bin].add(mask);
        sm_l2[sm].bins[write][bin].add(mask);
    }
    void created(int root,U line,bool pending) {
        ensure(phase>=0&&line%128==0,"dirty creation identity");
        const Key key{root,line};
        ensure(!live.count(key)&&live.size()+inflight.size()<LIVE_CAP,"bounded unique dirty obligation");
        ensure(by_root.count(root)||by_root.size()<ROOT_CAP,"bounded root ledger");
        Obligation o{key,phase,pending};live.emplace(key,o);
        change(o,[&](Ledger& l){if(pending){++l.created_pending;++l.pending;}
            else{++l.created_resident;++l.resident;}});
        peak();
    }
    void filled(int root,U line) {
        const Key key{root,line};auto it=live.find(key);
        ensure(it!=live.end()&&it->second.pending,"fill transfers a pending write obligation");
        change(it->second,[](Ledger& l){ensure(l.pending>0,"pending underflow");--l.pending;++l.resident;});
        it->second.pending=false;peak();
    }
    void writeback_issue(int root,U line,U request_id) {
        const Key key{root,line};auto it=live.find(key);
        ensure(it!=live.end()&&!it->second.pending&&!inflight.count(request_id),
               "WB transfers actual resident dirty obligation");
        const auto obligation=it->second;
        inflight.emplace(request_id,obligation);live.erase(it);
        change(obligation,[](Ledger& l){ensure(l.resident>0,"resident underflow");--l.resident;++l.inflight;});
        ++writeback_trigger_kernel[phase];peak();
    }
    void writeback_complete(int root,U line,U request_id) {
        auto it=inflight.find(request_id);
        ensure(it!=inflight.end()&&it->second.key==Key{root,line},"matching unique WB completion");
        const auto obligation=it->second;inflight.erase(it);
        change(obligation,[](Ledger& l){ensure(l.inflight>0,"inflight underflow");--l.inflight;++l.completed;});
    }
    void audit(U actual_pending,U actual_resident,U actual_inflight)const {
        ledger.verify();
        ensure(ledger.pending==actual_pending&&ledger.resident==actual_resident&&
               ledger.inflight==actual_inflight&&live.size()==ledger.pending+ledger.resident&&
               inflight.size()==ledger.inflight,"observer versus actual model states");
        for(const auto& x:by_root)x.second.verify();
        for(const auto& x:by_first_dirty_kernel)x.second.verify();
    }
private:
    std::unordered_map<Key,Obligation,KeyHash> live;
    std::unordered_map<U,Obligation> inflight;
    template<class F>void change(const Obligation& o,F fn) {
        fn(ledger);fn(by_root[o.key.root]);fn(by_first_dirty_kernel[o.first_dirty_kernel]);
        ledger.verify();
    }
    void peak() {
        peak_live=std::max<U>(peak_live,live.size()+inflight.size());
        peak_pending=std::max(peak_pending,ledger.pending);
        peak_resident=std::max(peak_resident,ledger.resident);
        peak_inflight=std::max(peak_inflight,ledger.inflight);
    }
};
}
