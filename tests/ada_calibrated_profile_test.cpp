#include "ada_calibrated_profile.h"
#include <iostream>
#include <tuple>

using namespace GTSim;
using P = AdaCacheProfile;
using C = AdaCalibratedProfile;
static unsigned checks=0;
static void check(bool ok,const char* text) { ++checks; if(!ok)throw std::runtime_error(text); }
template<class F> static void rejects(F f,const char* text) {
    bool rejected=false;
    try { f(); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,text);
}
static auto fields(const AdaKernelAllocation& a) {
    return std::tie(a.padded_threads,a.rounded_registers,a.resident_ctas_per_sm,
                    a.shared_carveout_bytes,a.l1_bytes,a.l1_ways);
}
static void geometry(const AdaKernelAllocation& a,P p,unsigned shared,unsigned bytes,unsigned sets,unsigned ways) {
    check(a.shared_carveout_bytes==shared && a.l1_bytes==bytes && a.l1_ways==ways,"allocation exact shared/L1 geometry");
    const auto c=C::l1(a,p);
    check(c.capacity_bytes_per_sm==bytes && c.ways==ways && c.line_bytes==128 &&
          bytes/(ways*128)==sets,"L1 configured exact set/way capacity");
    check(c.mode==PerSmL1Mode::MODELED_SET_ASSOCIATIVE &&
          c.persistence==PerSmL1Persistence::KERNEL_FLUSH && c.num_sms==48 &&
          c.sector32 && c.write_allocate && !c.store_bypass && c.hit_latency_cycles==34 &&
          c.dirty_protection_percent==25,"unchanged sector/WT/lazy/flush contract");
    check(c.replacement==C::policy(p),"explicit replacement policy");
    PerSmL1Cache cache(c); // Validate actual cache accepts each non-power-of-two way count.
    check(cache.config().capacity_bytes_per_sm==bytes,"actual cache config accepted");
}
static void names_and_qualification() {
    check(C::default_profile==P::R2_ADAPTIVE,"r2 remains default candidate");
    for(const auto p:{P::TUNER_V1,P::R2_ADAPTIVE,P::R2_SHARED64,P::R2_SHARED100,
                      P::R3_FIFO_SHARED32,P::R3_FIFO_SHARED64,P::R3_FIFO_SHARED100}) {
        check(parse_ada_cache_profile(ada_cache_profile_name(p))==p,"profile names roundtrip");
        check(std::string(C::settings(p).name)==C::name(p),"settings identify profile");
    }
    rejects([]{parse_ada_cache_profile("r3");},"no ambiguous profile aliases");
    rejects([]{C::settings(static_cast<P>(255));},"unknown enum rejected");
    check(std::string(C::settings(P::R2_ADAPTIVE).evidence_status)=="default_candidate_not_full_hardware_match","no hardware-match promotion");
    check(C::settings(P::R3_FIFO_SHARED100).experimental && !C::settings(P::R3_FIFO_SHARED100).adaptive,"r3 static experimental only");
    check(C::settings(P::R2_SHARED64).adaptive,"r2 fixed option still uses upstream adaptive mechanism");
    check(resolve_ada_cache_profile(P::R2_ADAPTIVE,32768)==P::R2_ADAPTIVE,"observed32 retains r2 adaptive source");
    check(resolve_ada_cache_profile(P::R2_ADAPTIVE,65536)==P::R2_SHARED64,"observed64 resolves to correct frozen source");
    check(resolve_ada_cache_profile(P::R2_ADAPTIVE,102400)==P::R2_SHARED100,"observed100 resolves to correct frozen source");
    check(std::string(C::settings(resolve_ada_cache_profile(P::R2_ADAPTIVE,65536)).source_config)=="configs/rtx4000-ada-calibrated/r2-shared64/source/gpgpusim.config","resolved source provenance");
}
static void tuner_regression() {
    for(unsigned shared:{0U,1U,8192U,8193U,16384U,16385U,32768U,32769U,65536U,65537U,102400U}) {
        const AdaKernelResources r{128,16,shared,48};
        const auto old=AdaTunerProfile::allocate(r),same=C::allocate(r,P::TUNER_V1);
        check(fields(old)==fields(same),"v1 allocation unchanged across all carveout boundaries");
        const auto a=AdaTunerProfile::l1(old),b=C::l1(same,P::TUNER_V1);
        check(a.capacity_bytes_per_sm==b.capacity_bytes_per_sm && a.ways==b.ways &&
              a.replacement==b.replacement && a.sector32==b.sector32 &&
              a.store_bypass==b.store_bypass && a.hit_latency_cycles==b.hit_latency_cycles,"v1 L1 semantics retained");
    }
    geometry(C::allocate({128,0,0,48},P::TUNER_V1),P::TUNER_V1,0,131072,4,256);
    rejects([]{C::allocate({128,0,0,48},P::TUNER_V1,32768);},"stale observed v1 metadata rejected");
}
static void calibrated_allocation() {
    for(const auto [demand,carveout]:{std::pair{0U,32768U},{32768U,32768U},
                                    {32769U,65536U},{65536U,65536U},
                                    {65537U,102400U},{102400U,102400U}}) {
        const auto a=C::allocate({128,16,demand,48},P::R2_ADAPTIVE);
        geometry(a,P::R2_ADAPTIVE,carveout,131072-carveout,4,(131072-carveout)/512);
    }
    const auto rounded=C::allocate({33,5,1000,200},P::R2_ADAPTIVE);
    check(rounded.padded_threads==64 && rounded.rounded_registers==8 && rounded.resident_ctas_per_sm==5,"manual warp/reg rounding and grid cap");
    const auto registers=C::allocate({256,65,0,100000},P::R2_ADAPTIVE);
    check(registers.rounded_registers==68 && registers.resident_ctas_per_sm==3,"manual register-limited occupancy");
    const auto shared=C::allocate({128,0,24576,100000},P::R2_ADAPTIVE);
    check(shared.resident_ctas_per_sm==4 && shared.shared_carveout_bytes==102400,"shared demand is aggregate over four resident CTAs");
    const AdaKernelResources small{128,0,16384,48};
    geometry(C::allocate(small,P::R2_ADAPTIVE,65536),P::R2_SHARED64,65536,65536,4,128);
    geometry(C::allocate(small,P::R2_SHARED100,102400),P::R2_SHARED100,102400,28672,4,56);
    geometry(C::allocate(small,P::R3_FIFO_SHARED32,32768),P::R3_FIFO_SHARED32,32768,98304,64,12);
    geometry(C::allocate(small,P::R3_FIFO_SHARED64,65536),P::R3_FIFO_SHARED64,65536,65536,64,8);
    geometry(C::allocate(small,P::R3_FIFO_SHARED100,102400),P::R3_FIFO_SHARED100,102400,24576,64,3);
    check(C::policy(P::R2_ADAPTIVE)==PerSmL1ReplacementPolicy::LRU &&
          C::policy(P::R3_FIFO_SHARED100)==PerSmL1ReplacementPolicy::FIFO,"r3 policy opt-in only");
    for(auto p:{P::R2_SHARED64,P::R2_SHARED100,P::R3_FIFO_SHARED32,P::R3_FIFO_SHARED64,P::R3_FIFO_SHARED100}) {
        rejects([&]{C::allocate(small,p);},"fixed profile never invents observed value");
        const unsigned wrong=C::settings(p).fixed_shared_carveout_bytes==32768?65536:32768;
        rejects([&]{C::allocate(small,p,wrong);},"mismatched observed profile rejected");
    }
    for(unsigned invalid:{0U,32U,64U,100U,8192U,16384U,98304U,131072U})
        rejects([&]{C::allocate(small,P::R2_ADAPTIVE,invalid);},"invalid observed units/value rejected");
    rejects([]{C::allocate({128,0,24576,100000},P::R2_ADAPTIVE,65536);},"observed too small for aggregate occupancy rejected");
    rejects([]{C::allocate({128,0,24576,100000},P::R3_FIFO_SHARED64,65536);},"static too small never silently reduces occupancy");
    for(const auto r:{AdaKernelResources{0,0,0,48},{1025,0,0,48},{128,0,0,0},
                      {128,0,102401,48},{1024,65536,0,48},{128,65537,0,48}})
        rejects([&]{C::allocate(r,P::R2_ADAPTIVE);},"invalid resources and zero occupancy rejected");
}
static void stale_allocations() {
    auto a=C::allocate({128,0,0,48},P::R2_SHARED100,102400);
    rejects([&]{C::l1(a,P::R3_FIFO_SHARED100);},"r2 28KiB cannot masquerade as r3 24KiB");
    a=C::allocate({128,0,0,48},P::R3_FIFO_SHARED100,102400);
    a.l1_bytes=32768;a.l1_ways=4;
    rejects([&]{C::l1(a,P::R3_FIFO_SHARED100);},"upstream adaptive rounding to32KiB forbidden for static r3");
    a=C::allocate({128,0,0,48},P::R2_ADAPTIVE);
    rejects([&]{C::l1(a,P::R2_SHARED64);},"another kernel carveout rejected");
    ++a.l1_ways;
    rejects([&]{C::l1(a,P::R2_ADAPTIVE);},"stale ways rejected");
    a=C::allocate({128,0,0,48},P::R2_ADAPTIVE);a.resident_ctas_per_sm=24;
    rejects([&]{C::l1(a,P::R2_ADAPTIVE);},"stale occupancy rejected");
}
int main() {
    try {
        names_and_qualification();tuner_regression();calibrated_allocation();stale_allocations();
        std::cout<<"{\"status\":\"PASS_ADA_CALIBRATED_PROFILE\",\"checks\":"<<checks
                 <<",\"default_candidate\":\"r2-adaptive\",\"r3_experimental\":true,\"hardware_match_claim\":false}\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
