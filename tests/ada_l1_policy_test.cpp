#include "../source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h"
#include <iostream>
#include <string>

using namespace GTSim;
static unsigned checks=0;
static void check(bool ok,const char* message) {
    ++checks;
    if(!ok)throw std::runtime_error(message);
}
template<class F> static void rejects(F fn,const char* message) {
    bool caught=false;
    try{fn();}catch(const std::exception&){caught=true;}
    check(caught,message);
}
static PerSmL1Config config(PerSmL1ReplacementPolicy policy,unsigned bytes=256,unsigned ways=2) {
    PerSmL1Config c;
    c.mode=PerSmL1Mode::MODELED_SET_ASSOCIATIVE;c.num_sms=1;
    c.capacity_bytes_per_sm=bytes;c.ways=ways;c.sector32=true;c.write_allocate=true;
    c.replacement=policy;
    return c;
}
static PerSmL1Access read(std::uint64_t addr,unsigned mask=1) {
    PerSmL1Access a{0,4,addr,false,0,false};a.sector_mask=mask;return a;
}
static void fill(PerSmL1Cache& cache,std::uint64_t addr,unsigned mask=1) {
    auto d=cache.access(read(addr,mask));
    check(d.outcome==PerSmL1Outcome::READ_MISS&&d.read_ticket.valid,"real read fill required");
    check(cache.complete_read(d.read_ticket),"fill delivers current reservation");
}
static bool present(PerSmL1Cache& c,std::uint64_t addr) {return c.inspect(read(addr)).occupied;}

static void manual_evictions() {
    for(auto policy:{PerSmL1ReplacementPolicy::LRU,PerSmL1ReplacementPolicy::FIFO}) {
        const bool fifo=policy==PerSmL1ReplacementPolicy::FIFO;
        PerSmL1Cache c(config(policy));
        fill(c,0);fill(c,128);
        check(c.access(read(0)).outcome==PerSmL1Outcome::READ_HIT,"A hit after inserting A then B");
        fill(c,256);
        check(present(c,0)==!fifo&&present(c,128)==fifo&&present(c,256),
              "A,B,A,C: FIFO evicts A while LRU evicts B");
        check(c.statistics().evictions==1&&c.statistics().l2_input_transactions==3,
              "replacement alone does not invent downstream transactions");

        PerSmL1Cache store(config(policy));fill(store,0);fill(store,128);
        auto w=read(0);w.is_write=true;w.known_byte_masks[0]=UINT32_MAX;
        check(store.access(w).outcome==PerSmL1Outcome::WRITE_HIT,"store hits existing A");
        fill(store,256);
        check(present(store,0)==!fifo&&present(store,128)==fifo,
              "WT hit refreshes LRU but never FIFO insertion age");

        PerSmL1Cache sector(config(policy));fill(sector,0);fill(sector,128);
        auto d=sector.access(read(0,2));
        check(!d.allocated&&d.read_ticket.sector_mask==2,"new sector reuses A tag");
        check(sector.complete_read(d.read_ticket),"sector1 completes without wholeline reallocation");
        fill(sector,256);
        check(present(sector,0)==!fifo&&present(sector,128)==fifo,
              "sector miss does not reset FIFO wholeline insertion age");

        PerSmL1Cache completion(config(policy));
        auto a=completion.access(read(0)),b=completion.access(read(128));
        check(completion.complete_read(b.read_ticket)&&completion.complete_read(a.read_ticket),
              "out-of-order fills retire B before A");
        fill(completion,256);
        check(!present(completion,0)&&present(completion,128),
              "fill delivery order does not refresh either replacement timestamp");
    }
}
static void pending_and_protection() {
    auto cfg=config(PerSmL1ReplacementPolicy::FIFO,128,1);
    cfg.persistence=PerSmL1Persistence::KERNEL_FLUSH;PerSmL1Cache c(cfg);
    c.begin_kernel();auto a=c.access(read(0));auto b=c.access(read(128));
    check(b.reservation_failed&&!b.allocated&&!b.evicted&&!b.read_ticket.valid,
          "FIFO does not evict a protected outstanding fill");
    rejects([&]{c.configure_capacity_bytes_per_sm(256,2);},"FIFO resize still rejects live tickets");
    c.begin_kernel();
    check(!c.complete_read(a.read_ticket)&&c.readiness().stale_tickets==1,
          "kernel flush preserves stale-ticket retirement contract");
    c.configure_capacity_bytes_per_sm(256,2);
    check(c.config().replacement==PerSmL1ReplacementPolicy::FIFO,"reconfigure preserves selected policy");
    fill(c,0);fill(c,128);c.access(read(0));fill(c,256);
    check(!present(c,0)&&present(c,128),"new geometry still executes FIFO");

    cfg=config(PerSmL1ReplacementPolicy::FIFO,2048,2);cfg.dirty_protection_percent=25;
    PerSmL1Cache protected_cache(cfg);
    auto w=read(0);w.is_write=true;w.known_byte_masks[0]=UINT32_MAX;
    protected_cache.access(w);fill(protected_cache,1024);fill(protected_cache,2048);
    check(present(protected_cache,0)&&!present(protected_cache,1024)&&present(protected_cache,2048),
          "FIFO chooses oldest eligible clean line while preserving dirty protection");
    check(protected_cache.statistics().l2_input_transactions==3,
          "protected WT data does not create an additional writeback");
}
static void layouts() {
    struct Layout{unsigned kib,sets,ways;};
    const Layout layouts[]={{24,4,48},{28,4,56},{64,4,128},{96,4,192},
                            {24,64,3},{64,64,8},{96,64,12}};
    for(const auto l:layouts)for(auto policy:{PerSmL1ReplacementPolicy::LRU,PerSmL1ReplacementPolicy::FIFO}) {
        PerSmL1Cache c(config(policy,l.kib*1024,l.ways));
        for(unsigned set=0;set<l.sets;++set)
            check(c.inspect(read(set*128)).set_index==set,"every configured linear set is reachable");
        const auto stride=std::uint64_t(l.sets)*128;
        for(unsigned i=0;i<l.ways;++i)fill(c,i*stride);
        check(c.statistics().evictions==0&&c.statistics().final_resident_lines==l.ways,
              "exact number of colliding ways fit");
        c.access(read(0));fill(c,l.ways*stride);
        const bool fifo=policy==PerSmL1ReplacementPolicy::FIFO;
        check(present(c,0)==!fifo&&present(c,stride)==fifo&&c.statistics().evictions==1,
              "same independent eviction oracle holds for all supported capacities/set layouts");
        // A boundary resize must derive the new set count instead of retaining
        // the previous 4-set index. No pending deliveries survive fill().
        c.configure_capacity_bytes_per_sm(24*1024,3);
        check(c.inspect(read(63*128)).set_index==63&&c.inspect(read(64*128)).set_index==0,
              "capacity/ways reconfiguration derives64 linear sets");
    }
    // 28KiB is not representable by64 sets of whole128B ways. Three ways with
    // that byte count is invalid; four ways would mean56 sets, NOT64. r3's
    // explicit24KiB/64x3 experiment must not be silently labeled28KiB.
    rejects([]{PerSmL1Cache c(config(PerSmL1ReplacementPolicy::FIFO,28*1024,3));},
            "28KiB cannot be rounded silently to64sets3ways");
    PerSmL1Cache c(config(PerSmL1ReplacementPolicy::FIFO,28*1024,4));
    check(c.inspect(read(55*128)).set_index==55&&c.inspect(read(56*128)).set_index==0,
          "28KiB with4ways truthfully computes56sets");
}
static void default_compatibility() {
    PerSmL1Config defaults;
    check(defaults.replacement==PerSmL1ReplacementPolicy::LRU,"existing default remains LRU");
    check(std::string(per_sm_l1_replacement_name(defaults.replacement))=="LRU"&&
          std::string(per_sm_l1_replacement_name(PerSmL1ReplacementPolicy::FIFO))=="FIFO",
          "policy names support explicit reporting");
    // Frozen default-mode oracle from the original L1 implementation. It
    // exercises pending eviction, late fill, kernel flush, stores and bypass.
    defaults.mode=PerSmL1Mode::MODELED_SET_ASSOCIATIVE;
    defaults.persistence=PerSmL1Persistence::KERNEL_FLUSH;defaults.num_sms=2;
    defaults.capacity_bytes_per_sm=1024;defaults.ways=2;
    PerSmL1Cache c(defaults);std::vector<ReadFillTicket> pending;std::uint64_t r=23455;
    for(int i=0;i<10000;++i) {
        r=r*6364136223846793005ULL+1442695040888963407ULL;
        if(i%51==0)c.begin_kernel();
        PerSmL1Access a{int(r%2),int((r>>5)%3),128*((r>>9)%16),(r>>20)%3==0,i,(r>>22)%7==0};
        auto d=c.access(a);if(d.read_ticket.valid)pending.push_back(d.read_ticket);
        if(!pending.empty()&&i%3==0){auto k=(r>>32)%pending.size();c.complete_read(pending[k]);pending.erase(pending.begin()+k);}
    }
    for(auto&t:pending)c.complete_read(t);
    auto s=c.statistics();auto t=c.readiness();
    check(s.decision_order_fnv1a64==4758093236714456855ULL&&s.read_hits==15&&s.read_misses==5681&&
          s.write_hits==293&&s.write_misses==2577&&s.evictions==2237&&s.kernel_flushes==196&&
          s.flushed_lines==2929&&s.final_resident_lines==2,"default LRU exact10000-access digest and counters");
    check(t.issued_tickets==5681&&t.completed_tickets==5681&&t.stale_tickets==5589&&
          t.ready_transitions==91&&t.pending_tag_reads==513&&t.write_hits_on_pending==287&&
          t.ready_lines==2&&t.pending_lines==0,"default LRU fill-ticket accounting unchanged");
    defaults.replacement=static_cast<PerSmL1ReplacementPolicy>(255);
    rejects([&]{PerSmL1Cache bad(defaults);},"unknown replacement policy rejected");
}
int main() {
    manual_evictions();pending_and_protection();layouts();default_compatibility();
    std::cout<<"{\"status\":\"PASS_ADA_L1_POLICY\",\"checks\":"<<checks
             <<",\"default\":\"LRU\",\"FIFO\":\"experiment_only\",\"GPU_executed\":false}\n";
}
