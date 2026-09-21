#include "memory.h"
#include <iostream>
#include <set>

namespace g = GTSim;
using U = std::uint64_t;
static U checks = 0;
static void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

struct Backend final : g::L2DramCompletionBackend {
    struct Pending { g::L2DramRequest request; U due; };
    std::vector<Pending> live;
    std::vector<g::L2DramRequest> accepted;
    g::L2DramRuntimeStatistics stats;
    U issue_cycle_to_ps(U c) const override { return c; }
    std::size_t admission_capacity() const override { return 64; }
    void enqueue(const g::L2DramRequest& r) override { check(try_enqueue(r,r.issue_cycle),"bounded mock admission"); }
    bool try_enqueue(const g::L2DramRequest& r, U cycle) override {
        if (live.size() == 64) return false;
        check(accepted.size() < 128,"bounded record fixture");
        check(r.request_id == accepted.size(),"request identity preserved");
        accepted.push_back(r); live.push_back({r,cycle+3});
        if (r.cause == g::L2DramRequestCause::DIRTY_WRITEBACK) {
            ++stats.writeback_requests; stats.writeback_bytes += r.bytes;
        } else { ++stats.fill_requests; stats.fill_bytes += r.bytes; }
        stats.peak_queue_depth = std::max<U>(stats.peak_queue_depth,live.size());
        return true;
    }
    std::vector<g::L2DramCompletion> step(U cycle) override {
        std::vector<g::L2DramCompletion> done;
        std::vector<Pending> remaining;
        for (const auto& p : live) {
            if (p.due > cycle) { remaining.push_back(p); continue; }
            const auto& r = p.request;
            const bool w = r.cause == g::L2DramRequestCause::DIRTY_WRITEBACK;
            if (w) { ++stats.writeback_completions; stats.writeback_completed_bytes += r.bytes; }
            else { ++stats.fill_completions; stats.fill_completed_bytes += r.bytes; }
            done.push_back({r.request_id,r.source_sequence,r.issue_cycle,r.issue_time_ps,cycle,r.key,w});
        }
        live = std::move(remaining);
        return done;
    }
    const g::L2DramRuntimeStatistics& statistics() const override { return stats; }
    std::size_t queue_depth() const override { return live.size(); }
};

// A legal, line-preserving service remap deliberately changes the decoded
// partition. Cache/partition identity must still use the original request key.
struct Mapper final : g::L2DramAddressMapper {
    U map(const g::CacheLineKey& k) const override { return k.line_addr + 256; }
};
struct Rig {
    Backend backend; Mapper mapper; g::L2Cache cache;
    g::Cycle cycle = 0; int next_id = 0;
    explicit Rig(g::L2GeometryConfig geometry, int bytes = 41943040)
        : cache(bytes,128,1,128,128,32,false,128,3,1000,1000,
                {},{},&backend,&mapper,geometry) {}
    void tick() {
        cache.step(cycle++);
        check(cycle < 10000,"bounded CPU-only test");
        if constexpr (g::kTilegenDirtySectorMode != 0) {
            const auto s = cache.dirty_sector_snapshot();
            check(s.dirty_sector_ledger_closed && s.writeback_byte_ledger_closed,"dirty and backend byte ledgers close");
        }
    }
    int access(U line, int warp, bool write = false) {
        g::DAGNode n(next_id++,"test","LS",write?"st.reg2dram":"ld.dram2reg",0,{},0,{});
        n.sm_id = 0; n.matrix_id = 3; n.warp_id = warp;
        g::ExplicitMemorySubop sub;
        sub.ranges.push_back({-1,line+5,1}); sub.requested_bytes = 1;
        if (write) { sub.ranges.push_back({-1,line+69,1}); ++sub.requested_bytes; }
        n.explicit_memory_subops.push_back(std::move(sub));
        while (!cache.enqueue_transaction_key(n,{n.matrix_id,line},write,cycle,0,true,nullptr,nullptr,0)) tick();
        tick(); while (!cache.is_quiescent()) tick();
        return n.id;
    }
    void closed() const {
        const auto s = cache.runtime_statistics();
        check(s.accepted_transactions == s.processed_transactions,"all ingress completed");
        check(s.dram_fill_bytes == s.dram_fill_completed_bytes &&
              s.dram_writeback_bytes == s.dram_writeback_completed_bytes,"all backend bytes completed");
    }
};

static U address(U partition, U set, U tag, bool ada) {
    const U local = tag*1024 + (set ^ (tag % 1024));
    if (ada) return (local*10+partition/2)*256+(partition%2)*128;
    return ((local/2)*20+partition)*256+(local%2)*128;
}
static void all_partitions() {
    Rig r(g::L2GeometryConfig::accelsim_rtx4000_ada_v1());
    std::set<int> seen;
    for (unsigned p=0;p<20;++p) {
        const U va=p*128;
        const int node=r.access(va,(p+1)%4);
        const auto& q=r.backend.accepted.back();
        check(q.node_id==node && q.sm_id==0,"origin node and SM preserved");
        check(q.key.line_addr==va && q.address==va+256 && q.bytes==128,"fill key/address/bytes preserved");
        check(q.l2_subpartition_id==static_cast<int>(p),"actual backend destination spans0..19 by address");
        check(q.l2_subpartition_id!=static_cast<int>(g::AdaAddressMapping::sub_partition(q.address)),"destination is before service remap");
        seen.insert(q.l2_subpartition_id);
    }
    check(seen.size()==20,"all twenty destinations reached"); r.closed();
}
static void eviction(g::L2GeometryConfig geometry, bool ada, bool fully_associative=false) {
    Rig r(geometry,fully_associative?128:41943040);
    const U partition=ada?19:0, set=17;
    const U victim=fully_associative?0:address(partition,set,0,ada);
    const int owner=r.access(victim,0,true);
    int trigger=-1;
    const unsigned count=fully_associative?1:16;
    for (unsigned t=1;t<=count;++t)
        trigger=r.access(fully_associative?128:address(partition,set,t,ada),7);
    unsigned writes=0;
    for (const auto& q:r.backend.accepted) {
        if (q.cause!=g::L2DramRequestCause::DIRTY_WRITEBACK) continue;
        ++writes;
        check(q.key.matrix_id==3 && q.key.line_addr==victim,"writeback retains actual victim key");
        check(q.node_id==trigger && q.node_id!=owner,"trigger context does not become dirty owner");
        check(q.l2_subpartition_id==(ada?19:3),"Ada uses victim destination; legacy keeps trigger SM-local field");
        if constexpr (g::kTilegenDirtySectorMode==2)
            check(q.bytes==32 && (q.address==victim+256 || q.address==victim+320),"dirty32 offsets preserve victim line destination");
        else check(q.bytes==128 && q.address==victim+256,"legacy dirty128 packet retained");
    }
    check(writes==(g::kTilegenDirtySectorMode==2?2U:1U),"exact dirty writeback request count");
    // Group-local victim and replacement necessarily share a destination.
    // The key assertions above test victim identity without falsely claiming
    // their partition numbers can differ inside the same set.
    r.closed();
}
int main() {
    try {
        all_partitions();
        eviction(g::L2GeometryConfig::accelsim_rtx4000_ada_v1(),true);
        eviction(g::L2GeometryConfig::paper_ada_l2_v1(),false);
        eviction({},false,true);
        std::cout << "{\"status\":\"PASS_ADA_FINE_PARTITION\",\"checks\":" << checks
                  << ",\"dirty_mode\":" << g::kTilegenDirtySectorMode
                  << ",\"memory_partitions\":20,\"timing_equivalence_claim\":false}\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
