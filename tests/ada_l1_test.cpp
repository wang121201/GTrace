#include "../source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h"
#include <iostream>

using namespace GTSim;
static unsigned checks = 0;
static void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> static void rejects(F f, const char* message) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}
static PerSmL1Config config(unsigned lines=8, unsigned ways=2) {
    PerSmL1Config c;
    c.mode=PerSmL1Mode::MODELED_SET_ASSOCIATIVE;
    c.num_sms=2;c.capacity_bytes_per_sm=128*lines;c.ways=ways;
    c.sector32=true;c.write_allocate=true;
    return c;
}
static PerSmL1Access request(std::uint64_t line, unsigned mask, bool write=false,
                            std::uint32_t bytes=0, unsigned sm=0) {
    PerSmL1Access a{static_cast<int>(sm),7,line,write,91,false};
    a.sector_mask=mask;
    for(unsigned i=0;i<4;++i) if(mask&(1u<<i)) a.known_byte_masks[i]=bytes;
    return a;
}
static void fill(PerSmL1Cache& cache, PerSmL1Access a) {
    auto d=cache.access(a);
    check(d.read_ticket.valid,"read must issue fill");
    check(cache.complete_read(d.read_ticket),"fill must complete resident generation");
}
static void sectors_and_tickets() {
    PerSmL1Cache c(config());
    auto s0=request(0,1),s1=request(0,2),both=request(0,3);
    auto d0=c.access(s0),d1=c.access(s1);
    check(d0.allocated && !d1.allocated,"same tag sector reservations");
    check(d0.read_ticket.sector_mask==1 && d1.read_ticket.sector_mask==2,
          "each delivery retains independent sector mask");
    check(c.complete_read(d0.read_ticket),"sector0 delivered");
    check(c.classify(s0)==PerSmL1Outcome::READ_HIT,"sector0 readable");
    check(c.classify(s1)==PerSmL1Outcome::READ_MISS,"sector0 fill must not ready sector1");
    auto dm=c.access(both);
    check(dm.forwarded_sector_mask==2 && dm.read_ticket.sector_mask==2,
          "mixed sector read forwards only missing sector");
    auto altered=dm.read_ticket;altered.sector_mask=1;
    rejects([&]{c.complete_read(altered);},"altered fill sector rejected");
    check(c.complete_read(d1.read_ticket) && c.complete_read(dm.read_ticket),
          "two delivery obligations retire separately");
    rejects([&]{c.complete_read(dm.read_ticket);},"duplicate completion rejected");
    check(c.classify(both)==PerSmL1Outcome::READ_HIT,"both delivered sectors readable");
    check(c.inspect(both).readable_sector_mask==3 && !c.inspect(both).ready,
          "partial line is not fully ready");
    auto rest=c.access(request(0,0));
    check(rest.forwarded_sector_mask==12 && rest.read_ticket.sector_mask==12,
          "legacy wholeline convention means all four sectors in sector mode");
    check(c.complete_read(rest.read_ticket) && c.inspect(both).ready,"all sectors ready");
    auto r=c.readiness();
    check(r.ready_lines==1 && r.ready_transitions==1 && r.sector_readiness_promotions==4 &&
          r.live_tickets==0,"line and sector readiness counters");
    check(c.classify(request(0,1,false,0,1))==PerSmL1Outcome::READ_MISS,"perSM isolation");
}
static void lazy_write_allocate() {
    PerSmL1Cache c(config());
    auto w=request(0,1,true,0x0000ffff);
    auto d=c.access(w);
    check(d.outcome==PerSmL1Outcome::WRITE_MISS && d.allocated && d.forwarded_to_l2 &&
          d.forwarded_sector_mask==1 && !d.read_ticket.valid,"WT lazy miss allocate without RFO");
    check(c.classify(request(0,1))==PerSmL1Outcome::READ_MISS,"partial bytes unreadable");
    check(c.inspect(w).modified && c.inspect(w).known_byte_masks[0]==0xffff,"known bytes tracked");
    w.known_byte_masks[0]=0xffff0000;
    d=c.access(w);
    check(d.outcome==PerSmL1Outcome::WRITE_HIT && d.forwarded_to_l2,"WT hit always forwarded");
    check(c.classify(request(0,1))==PerSmL1Outcome::READ_HIT,"two partial stores complete sector");
    check(c.classify(request(0,2))==PerSmL1Outcome::READ_MISS,"adjacent sector remains unreadable");
    auto sector_miss=c.access(request(0,2,true,1));
    check(sector_miss.outcome==PerSmL1Outcome::WRITE_MISS && !sector_miss.allocated &&
          c.inspect(w).resident_sector_mask==3,"absent sector store is miss within existing tag");
    auto unknown=request(128,1,true);
    check(c.access(unknown).allocated,"unknown store still reserves tag");
    check(c.inspect(unknown).readable_sector_mask==0,"unknown bytes never guessed complete");
    auto a=request(256,2,true,3);c.access(a);
    fill(c,request(256,2));
    check(c.inspect(a).known_byte_masks[1]==UINT32_MAX && c.inspect(a).modified,
          "read fill supplies rest while retaining modified status");
    auto pending=c.access(request(384,4));
    c.access(request(384,4,true,UINT32_MAX));
    check(c.classify(request(384,4))==PerSmL1Outcome::READ_HIT,"full store satisfies pending sector");
    check(c.complete_read(pending.read_ticket),"pending delivery still must retire after store");
    check(c.readiness().store_readiness_promotions==2,"store sector promotions counted");
    auto no=config();no.write_allocate=false;PerSmL1Cache n(no);
    d=n.access(request(0,1,true,UINT32_MAX));
    check(!d.allocated && !n.inspect(request(0,1)).occupied,"optional write noallocate mode");
    auto bypass=config();bypass.store_bypass=true;PerSmL1Cache b(bypass);
    d=b.access(request(0,4,true,UINT32_MAX));
    check(d.outcome==PerSmL1Outcome::BYPASS && d.forwarded_sector_mask==4 &&
          !b.inspect(request(0,4)).occupied,"explicit store bypass does not touch L1");
    auto bb=config();bb.mode=PerSmL1Mode::BYPASS;PerSmL1Cache bypass_all(bb);
    check(bypass_all.access(request(0,10)).forwarded_sector_mask==10,"BYPASS preserves sector mask");
}
static void replacement_and_protection() {
    auto cfg=config(8,1);cfg.dirty_protection_percent=25;PerSmL1Cache c(cfg);
    c.access(request(0,1,true,UINT32_MAX)); // 1/8=12.5%, protected.
    auto d=c.access(request(1024,1));
    check(d.reservation_failed && !d.allocated && !d.evicted && d.forwarded_to_l2 &&
          !d.read_ticket.valid,"protected set returns explicit functional fall-through");
    check(c.classify(request(0,1))==PerSmL1Outcome::READ_HIT,"protected WT line retained");
    c.access(request(128,1,true,UINT32_MAX)); // 2/8=25%, eligible.
    d=c.access(request(1024,1));
    check(d.allocated && d.evicted && !d.reservation_failed,"threshold inclusive at25percent");
    check(c.complete_read(d.read_ticket),"replacement fill completes");
    auto stats=c.statistics();
    check(stats.pre_l1_transactions==4 && stats.l2_input_transactions==4 &&
          stats.unmodeled_reservation_stalls==1,"WT eviction generates no duplicate writeback request");
    // With two ways, clean victim wins even when protected dirty is the LRU.
    cfg=config(16,2);cfg.dirty_protection_percent=25;PerSmL1Cache clean(cfg);
    clean.access(request(0,1,true,UINT32_MAX));fill(clean,request(1024,1));
    fill(clean,request(2048,1));
    check(clean.inspect(request(0,1)).occupied && !clean.inspect(request(1024,1)).occupied,
          "clean victim chosen instead of protected older modified line");
    cfg.dirty_protection_percent=0;PerSmL1Cache off(cfg);
    off.access(request(0,1,true,UINT32_MAX));fill(off,request(1024,1));fill(off,request(2048,1));
    check(!off.inspect(request(0,1)).occupied,"off restores LRU modified victim");
    PerSmL1Cache reserved(config(1,1));auto pending=reserved.access(request(0,1));
    d=reserved.access(request(128,2));
    check(d.reservation_failed && !d.read_ticket.valid,"pending sector reservation cannot be evicted");
    check(reserved.complete_read(pending.read_ticket),"reserved original still completes");
}
static void memo_and_reconfigure() {
    auto cfg=config();cfg.persistence=PerSmL1Persistence::KERNEL_FLUSH;PerSmL1Cache c(cfg);
    c.begin_kernel();auto a=request(0,1);auto pending=c.access(a);
    L1ReadMissMemo memo;c.remember_negative_read(a,memo);
    check(c.same_negative_read(a,memo),"memo initially valid");
    check(!c.same_negative_read(request(0,2),memo),"negative memo includes requested sector");
    c.access(request(0,1,true,1));
    check(c.same_negative_read(a,memo),"partial store does not spuriously promote readiness");
    c.access(request(0,1,true,UINT32_MAX));
    check(!c.same_negative_read(a,memo),"complete store invalidates negative memo");
    rejects([&]{c.configure_capacity_bytes_per_sm(4096,4);},"live fills prohibit layout change");
    c.begin_kernel();
    check(!c.complete_read(pending.read_ticket),"kernel invalidation makes delivery stale");
    check(c.statistics().kernel_flushes==1 && c.statistics().flushed_lines==1,"kernel flush counted");
    c.remember_negative_read(a,memo);const auto epoch=c.host_ready_epoch(0);
    c.configure_capacity_bytes_per_sm(4096,4);
    check(!c.same_negative_read(a,memo) && c.host_ready_epoch(0)>epoch,"resize invalidates all host memo identities");
    check(c.config().capacity_bytes_per_sm==4096 && c.config().ways==4,"resize config reported");
    for(auto kib:{28u,64u,96u,112u,120u,128u}) {
        c.configure_capacity_bytes_per_sm(kib*1024,2*kib);
        check(c.config().ways==2*kib,"adaptive four-set capacity geometry");
        fill(c,request(0,1));
        check(c.classify(request(0,1))==PerSmL1Outcome::READ_HIT,"adaptive geometry operational");
    }
    check(c.statistics().capacity_reconfigurations==7 &&
          c.statistics().capacity_reconfiguration_flushed_lines==5,"resize invalidation separately counted");
    rejects([&]{c.configure_capacity_bytes_per_sm(1000,4);},"bad capacity rejected");
    rejects([&]{c.access(request(0,16));},"outside sector mask rejected");
    auto wrong=request(0,1,true);wrong.known_byte_masks[1]=1;
    rejects([&]{c.access(wrong);},"bytes outside mask rejected");
}
static void legacy_behavior() {
    auto cfg=config(1,1);cfg.sector32=false;cfg.write_allocate=false;
    PerSmL1Cache c(cfg);PerSmL1Access a{0,7,0,false,91,false};
    auto d=c.access(a);
    check(d.read_ticket.valid && !d.read_ticket.sector_mask,"legacy wholeline fill ticket");
    auto w=a;w.is_write=true;w.sector_mask=1;w.known_byte_masks[0]=UINT32_MAX;
    c.access(w);
    check(c.classify(a)==PerSmL1Outcome::READ_MISS,"legacy stores do not ready data");
    check(c.complete_read(d.read_ticket) && c.classify(a)==PerSmL1Outcome::READ_HIT,"legacy fill readies whole line");
    w.canonical_line=128;
    check(!c.access(w).allocated && c.inspect(a).occupied,"legacy write miss does not allocate");
    auto r=a;r.canonical_line=128;d=c.access(r);
    auto replacement=a;replacement.canonical_line=256;auto d2=c.access(replacement);
    check(d2.evicted && !c.complete_read(d.read_ticket),"legacy pending victim/stale completion retained");
    check(c.complete_read(d2.read_ticket),"legacy replacement fill retires");
}
static void legacy_frozen_stream() {
    // Golden was independently executed against the unchanged integration
    // baseline header. Delayed deliveries, kernel flushes and pending victims
    // exercise the compatibility contract beyond a single happy-path hit.
    PerSmL1Config cfg;cfg.mode=PerSmL1Mode::MODELED_SET_ASSOCIATIVE;
    cfg.persistence=PerSmL1Persistence::KERNEL_FLUSH;cfg.num_sms=2;
    cfg.capacity_bytes_per_sm=1024;cfg.ways=2;
    PerSmL1Cache c(cfg);std::vector<ReadFillTicket> pending;std::uint64_t r=23455;
    for(int i=0;i<10000;++i) {
        r=r*6364136223846793005ULL+1442695040888963407ULL;
        if(i%51==0)c.begin_kernel();
        PerSmL1Access a{int(r%2),int((r>>5)%3),128*((r>>9)%16),(r>>20)%3==0,i,(r>>22)%7==0};
        auto d=c.access(a);if(d.read_ticket.valid)pending.push_back(d.read_ticket);
        if(!pending.empty()&&i%3==0) {
            auto k=(r>>32)%pending.size();c.complete_read(pending[k]);pending.erase(pending.begin()+k);
        }
    }
    for(auto&t:pending)c.complete_read(t);
    auto s=c.statistics();auto t=c.readiness();
    check(s.decision_order_fnv1a64==4758093236714456855ULL &&
          s.pre_l1_transactions==10000 && s.read_hits==15 && s.read_misses==5681 &&
          s.write_hits==293 && s.write_misses==2577 && s.evictions==2237 &&
          s.kernel_flushes==196 && s.flushed_lines==2929 && s.final_resident_lines==2,
          "10000-access legacy exact ordered outcome digest and counters");
    check(t.issued_tickets==5681 && t.completed_tickets==5681 && t.stale_tickets==5589 &&
          t.ready_transitions==91 && t.pending_tag_reads==513 &&
          t.write_hits_on_pending==287 && t.ready_lines==2 && t.pending_lines==0,
          "10000-access legacy exact ticket/readiness counters");
}
int main() {
    sectors_and_tickets();lazy_write_allocate();replacement_and_protection();
    memo_and_reconfigure();legacy_behavior();legacy_frozen_stream();
    auto bad=config();bad.line_bytes=64;
    rejects([&]{PerSmL1Cache c(bad);},"sector mode requires128Bline");
    bad=config();bad.dirty_protection_percent=101;
    rejects([&]{PerSmL1Cache c(bad);},"invalid protection percentage rejected");
    std::cout<<"{\"status\":\"PASS_ADA_L1\",\"checks\":"<<checks<<"}\n";
}
