#include "../source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// Compile the actual frozen independent reference, not a rewritten oracle.
// SHA256 9319661e7abe613cc9bdb81ce488d1b3448aab6e87ad085e394fe92e8db042aa.
// The original main has implicit-main return semantics; it is not invoked.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
namespace FrozenR4 {
#define main unused_frozen_command_line_entry
#include "../configs/rtx4000-ada-r4-serial/source/cache_policy_replay_r4.cpp"
#undef main
}
#pragma GCC diagnostic pop

using namespace GTSim;
static std::uint64_t checks=0;
static void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F>static void rejects(F fn,const char* message){bool caught=false;try{fn();}catch(const std::exception&){caught=true;}check(caught,message);}
static PerSmL1Config config(unsigned sets=1,unsigned ways=3,bool hash=false){
    PerSmL1Config c;c.mode=PerSmL1Mode::MODELED_SET_ASSOCIATIVE;c.num_sms=1;
    c.capacity_bytes_per_sm=std::uint64_t(sets)*ways*128;c.ways=ways;
    c.sector32=true;c.write_allocate=true;c.replacement=PerSmL1ReplacementPolicy::CLOCK;
    if(hash)c.hash_policy=PerSmL1HashPolicy::ALLOCATION_RELATIVE_HASH2;
    return c;
}
static PerSmL1Access access(std::uint64_t offset,unsigned mask=1,std::uint64_t base=0,int allocation=0){
    PerSmL1Access a{0,allocation,base+offset,false,0,false};a.sector_mask=mask;
    a.allocation_relative_byte_offset=offset;return a;
}
static void fill(PerSmL1Cache& c,const PerSmL1Access& a){
    auto d=c.access(a);check(d.read_ticket.valid,"read miss issues actual fill obligation");
    check(d.read_ticket.canonical_line==a.canonical_line,"ticket retains original global tag address");
    check(c.complete_read(d.read_ticket),"fill retires current reservation");
}
static bool has(PerSmL1Cache& c,std::uint64_t offset){return c.inspect(access(offset)).occupied;}

static void manual_clock(){
    PerSmL1Cache c(config());fill(c,access(0));fill(c,access(128));fill(c,access(256));
    check(c.clock_hand(0,0)==0,"filling unused slots does not move hand");
    check(c.inspect(access(0)).way==0&&c.inspect(access(128)).way==1&&c.inspect(access(256)).way==2,
          "new lines occupy unused slots in order");
    for(unsigned i=0;i<5;++i){c.classify(access(384));c.inspect(access(384));}
    check(c.clock_hand(0,0)==0&&c.inspect(access(128)).clock_reference_bit,
          "probe/inspection never executes a speculative CLOCK sweep");
    fill(c,access(384));
    check(!has(c,0)&&has(c,128)&&has(c,256)&&c.clock_hand(0,0)==1,
          "first full sweep clears three refs and replaces slot0");
    check(!c.inspect(access(128)).clock_reference_bit&&!c.inspect(access(256)).clock_reference_bit,
          "untouched old lines retain cleared reference bits");
    auto d=c.access(access(128,2));
    check(d.outcome==PerSmL1Outcome::READ_MISS&&!d.allocated&&
          c.inspect(access(128)).clock_reference_bit,"resident tag sector miss sets ref1");
    check(c.complete_read(d.read_ticket)&&c.clock_hand(0,0)==1,
          "completion changes valid sectors but does not move hand");
    fill(c,access(512));
    check(has(c,128)&&!has(c,256)&&c.clock_hand(0,0)==0,
          "sector-miss second chance preserves B and evicts C");
    check(c.access(access(128,1)).outcome==PerSmL1Outcome::READ_HIT&&
          c.inspect(access(128)).clock_reference_bit,"ordinary sector hit sets ref1 too");
}

static void hash_and_identity(){
    PerSmL1Cache c(config(16,2,true));
    struct Golden{unsigned key,set;};
    const Golden golden[]={{0,0},{1,0},{2,1},{3,13},{4,1},{5,14},{7,6},{16,15},{21,0},{23,3},{32,2}};
    for(auto g:golden)check(c.inspect(access(g.key*128)).set_index==g.set,"frozen hash2 collision/index golden");
    fill(c,access(0));fill(c,access(128));fill(c,access(21*128));
    check(!has(c,0)&&has(c,128)&&has(c,21*128),"three hash2-colliding keys drive a two-way CLOCK eviction");
    PerSmL1Cache shifted(config(16,2,true));
    auto a=access(128,2,0x123400000ULL,7);fill(shifted,a);
    check(shifted.inspect(a).set_index==0&&shifted.inspect(a).canonical_line==0x123400080ULL,
          "hash uses relative offset while original global tag survives");
    auto second=access(128,2,0x456700000ULL,8);fill(shifted,second);
    check(shifted.inspect(a).occupied&&shifted.inspect(second).occupied,
          "allocation namespaces retain distinct global tags at identical relative offsets");
    auto missing=a;missing.allocation_relative_byte_offset.reset();
    rejects([&]{shifted.access(missing);},"missing offset never falls back to VA");
    missing.bypass_l1=true;rejects([&]{shifted.access(missing);},"r4 bypass still requires declared offset");
    auto wrong=a;wrong.allocation_relative_byte_offset=0;
    rejects([&]{shifted.access(wrong);},"changing relative identity for same allocation is rejected");
    wrong=a;wrong.allocation_relative_byte_offset=129;
    rejects([&]{shifted.access(wrong);},"offset must name canonical line not sector/lane byte");
    wrong=a;wrong.allocation_relative_byte_offset=std::uint64_t{1}<<32;
    rejects([&]{shifted.access(wrong);},"frozen uint32 relative address domain enforced");
    rejects([]{PerSmL1Cache bad(config(3,2,true));},"hash2 nonpoweroftwo sets rejected");
    rejects([&]{shifted.configure_capacity_bytes_per_sm(3*2*128,2);},"rehash geometry checked before resize");
    check(shifted.inspect(a).occupied,"rejected resize does not mutate cache");
    PerSmL1Cache memo(config(16,2,true));a=access(3*128);auto d=memo.access(a);
    L1ReadMissMemo saved;memo.remember_negative_read(a,saved);
    check(memo.same_negative_read(a,saved),"negative memo tracks hashed set");
    check(memo.complete_read(d.read_ticket)&&!memo.same_negative_read(a,saved),"hashed fill promotion invalidates memo");
}

static void inherited_pending_and_dirty(){
    auto cfg=config(1,1);cfg.persistence=PerSmL1Persistence::KERNEL_FLUSH;
    PerSmL1Cache c(cfg);c.begin_kernel();auto a=c.access(access(0));auto d=c.access(access(128));
    check(d.reservation_failed&&!d.allocated&&c.clock_hand(0,0)==0&&
          c.inspect(access(0)).clock_reference_bit,"all-pending set does not clear refs or force eviction");
    rejects([&]{c.configure_capacity_bytes_per_sm(256,2);},"live read tickets still block resize");
    c.begin_kernel();check(!c.complete_read(a.read_ticket),"CLOCK keeps stale fill retirement");
    check(c.clock_hand(0,0)==0,"kernel flush resets hand");
    c.configure_capacity_bytes_per_sm(384,3);
    fill(c,access(0));fill(c,access(128));fill(c,access(256));fill(c,access(384));
    check(c.clock_hand(0,0)==1,"resized CLOCK hand advances on replacement");
    c.configure_capacity_bytes_per_sm(512,4);check(c.clock_hand(0,0)==0,"resize resets hand to0");
    cfg=config(8,2);cfg.dirty_protection_percent=25;PerSmL1Cache protected_cache(cfg);
    auto w=access(0);w.is_write=true;w.known_byte_masks[0]=UINT32_MAX;protected_cache.access(w);
    w=access(1024);w.is_write=true;w.known_byte_masks[0]=UINT32_MAX;protected_cache.access(w);
    d=protected_cache.access(access(2048));
    check(d.reservation_failed&&!d.evicted&&protected_cache.clock_hand(0,0)==0,
          "CLOCK does not bypass inherited modified-line protection");
    check(protected_cache.statistics().l2_input_transactions==3,
          "WT maintenance adds no implicit duplicate writeback");
}

static void frozen_serial_differential(){
    // Selected r4 effective geometries for shared32/64/100: 100/66/28KiB.
    // No concurrent fills/writes are covered by this exact reference claim.
    for(unsigned ways:{50u,33u,14u}) {
        PerSmL1Cache actual(config(16,ways,true));
        FrozenR4::Cache expected({"frozen","CLOCK",128,16,2,1000,11},ways*16*128);
        std::uint32_t state=0x413821u;
        for(unsigned i=0;i<6000;++i) {
            state=state*1664525u+1013904223u;
            // Collision pressure plus all four sectors, and recurring reads.
            const std::uint32_t raw=((i%5)?((state>>8)%(ways*40)):(i%73))*128+((state>>28)&3)*32;
            auto request=access(raw/128*128,1u<<((raw%128)/32),0x100000000ULL,12);
            const auto oldhits=expected.hits;expected.access(raw);
            auto result=actual.access(request);
            check((result.outcome==PerSmL1Outcome::READ_HIT)==(expected.hits==oldhits+1),
                  "each serial request matches original frozen CLOCK/hash2 hit/miss");
            if(result.read_ticket.valid)check(actual.complete_read(result.read_ticket),"serial synchronous fill completes");
            auto line=actual.inspect(request);
            unsigned set=0;while(set<16&&!expected.sets[set].lookup.count(raw/128))++set;
            check(set<16&&line.set_index==set,"reference and cache hash to same set");
            const auto slot=expected.sets[set].lookup.at(raw/128);
            const auto& node=expected.sets[set].nodes[slot];
            check(line.way==unsigned(slot)&&line.readable_sector_mask==node.mask&&
                  line.clock_reference_bit==bool(node.ref)&&actual.clock_hand(0,set)==expected.sets[set].hand,
                  "reference slot/sector/ref/hand state exactly reproduced");
            if(i%113==0)for(unsigned s=0;s<16;++s)for(const auto& kv:expected.sets[s].lookup){
                const auto& n=expected.sets[s].nodes[kv.second];
                auto state_line=actual.inspect(access(kv.first*128,1,0x100000000ULL,12));
                check(state_line.occupied&&state_line.way==unsigned(kv.second)&&state_line.set_index==s&&
                      state_line.readable_sector_mask==n.mask&&state_line.clock_reference_bit==bool(n.ref),
                      "periodic full resident state matches frozen reference");
            }
        }
        auto s=actual.statistics();
        check(s.read_hits==expected.hits&&s.read_misses==expected.misses&&s.pre_l1_transactions==6000,
              "pergeometry6000-request counters match frozen reference");
        check(s.unmodeled_reservation_stalls==0&&actual.live_read_tickets()==0,
              "serial-read comparison has no pending/stall approximation");
    }
}
static void legacy_defaults(){
    PerSmL1Config c;check(c.replacement==PerSmL1ReplacementPolicy::LRU&&
                        c.hash_policy==PerSmL1HashPolicy::LINEAR_GLOBAL,"new modes remain optin");
    c.mode=PerSmL1Mode::MODELED_SET_ASSOCIATIVE;c.persistence=PerSmL1Persistence::KERNEL_FLUSH;
    c.num_sms=2;c.capacity_bytes_per_sm=1024;c.ways=2;
    PerSmL1Cache cache(c);std::vector<ReadFillTicket> pending;std::uint64_t r=23455;
    for(int i=0;i<10000;++i){r=r*6364136223846793005ULL+1442695040888963407ULL;
        if(i%51==0)cache.begin_kernel();
        PerSmL1Access a{int(r%2),int((r>>5)%3),128*((r>>9)%16),(r>>20)%3==0,i,(r>>22)%7==0};
        auto d=cache.access(a);if(d.read_ticket.valid)pending.push_back(d.read_ticket);
        if(!pending.empty()&&i%3==0){auto k=(r>>32)%pending.size();cache.complete_read(pending[k]);pending.erase(pending.begin()+k);}}
    for(auto&t:pending)cache.complete_read(t);
    check(cache.statistics().decision_order_fnv1a64==4758093236714456855ULL&&
          cache.readiness().stale_tickets==5589,"legacy10000-request ordered hash/tickets unchanged");
}
int main(){manual_clock();hash_and_identity();inherited_pending_and_dirty();frozen_serial_differential();legacy_defaults();
    std::cout<<"{\"status\":\"PASS_ADA_L1_CLOCK\",\"checks\":"<<checks
             <<",\"frozen_reference_requests\":18000,\"serial_read_only_claim\":true,\"GPU_executed\":false}\n";}
