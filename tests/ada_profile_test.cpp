#include "../source/ada_gtsim_structure.h"
#include <iostream>
#include <limits>

using namespace GTSim;
static unsigned checks=0;
static void check(bool ok,const char* why) {
    ++checks;
    if(!ok)throw std::runtime_error(why);
}
template<class F> static void rejects(F fn,const char* why) {
    bool caught=false;
    try{fn();}catch(const std::exception&){caught=true;}
    check(caught,why);
}

// Independent numerical oracle derived from pinned shader.cc:4407-4517 and
// tuner gpgpusim.config (SHA e09511b9d168632e975422b19a9557ff5ed4482ace160fa428f9c70455f5a891).
// Deliberately uses fixed expected values, not a copy of allocate()'s min/ceil
// code. Each discontinuity can distinguish an incorrect rounding/limit order.
struct Case {
    AdaKernelResources input;
    unsigned threads,regs,ctas,shared,l1,ways;
    const char* label;
};
static void allocations() {
    const Case cases[]={
        {{1,0,0,4096},32,0,24,0,131072,256,"CTA cap dominates one-thread blocks"},
        {{31,0,0,4096},32,0,24,0,131072,256,"31 threads padded to32"},
        {{32,0,0,4096},32,0,24,0,131072,256,"32 threads exact"},
        {{33,0,0,4096},64,0,24,0,131072,256,"33 threads padded to64"},
        {{64,0,0,4096},64,0,24,0,131072,256,"24 full two-warp CTAs"},
        {{65,0,0,4096},96,0,16,0,131072,256,"65-thread boundary reduces residency"},
        {{128,0,0,4096},128,0,12,0,131072,256,"thread-limited12"},
        {{129,0,0,4096},160,0,9,0,131072,256,"129-thread boundary reduces residency"},
        {{512,0,0,4096},512,0,3,0,131072,256,"512-thread boundary"},
        {{513,0,0,4096},544,0,2,0,131072,256,"513 threads padded to544"},
        {{1023,0,0,4096},1024,0,1,0,131072,256,"maximum CTA padded"},
        {{1024,0,0,4096},1024,0,1,0,131072,256,"maximum legal CTA"},
        {{64,40,0,4096},64,40,24,0,131072,256,"40regs keeps24CTAs"},
        {{64,41,0,4096},64,44,23,0,131072,256,"41regs rounded44 limits23CTAs"},
        {{128,84,0,4096},128,84,6,0,131072,256,"84regs permits6CTAs"},
        {{128,85,0,4096},128,88,5,0,131072,256,"85regs rounded88 permits5CTAs"},
        {{256,255,0,4096},256,256,1,0,131072,256,"255regs rounded256 exactlyfillsregisterfile"},
        {{256,256,0,4096},256,256,1,0,131072,256,"256regs exact capacity"},
        {{128,0,0,1},128,0,1,0,131072,256,"one-grid CTA caps perSM budget"},
        {{128,0,0,48},128,0,1,0,131072,256,"grid48 gives1perSM"},
        {{128,0,0,49},128,0,2,0,131072,256,"grid49 ceil gives2perSM"},
        {{128,0,0,96},128,0,2,0,131072,256,"grid96 exact"},
        {{128,0,0,97},128,0,3,0,131072,256,"grid97 ceil gives3perSM"},
        {{128,0,1,48},128,0,1,8192,122880,240,"small shared allocation selects8KiB"},
        {{128,0,8192,48},128,0,1,8192,122880,240,"8KiB endpoint"},
        {{128,0,8193,48},128,0,1,16384,114688,224,"above8KiB selects16KiB"},
        {{128,0,16384,48},128,0,1,16384,114688,224,"16KiB endpoint"},
        {{128,0,16385,48},128,0,1,32768,98304,192,"above16KiB selects32KiB"},
        {{128,0,32768,48},128,0,1,32768,98304,192,"32KiB endpoint"},
        {{128,0,32769,48},128,0,1,65536,65536,128,"above32KiB selects64KiB"},
        {{128,0,65536,48},128,0,1,65536,65536,128,"64KiB endpoint"},
        {{128,0,65537,48},128,0,1,102400,28672,56,"above64KiB selects100KiB"},
        {{128,0,102400,48},128,0,1,102400,28672,56,"100KiB shared leaves28KiB L1"},
        {{128,0,8192,49},128,0,2,16384,114688,224,"grid budget participates before carveout"},
        {{128,0,8192,97},128,0,3,32768,98304,192,"3CTAs round24KiB total to32KiB"},
        {{128,0,8192,4096},128,0,12,102400,28672,56,"12CTAs occupy96KiB rounded100KiB"},
        {{128,0,51200,4096},128,0,2,102400,28672,56,"shared-limit two CTAs"},
        {{128,0,51201,4096},128,0,1,65536,65536,128,"one-byte shared boundary changes residency/carveout"},
        {{512,64,40000,4096},512,64,2,102400,28672,56,"register and shared simultaneous constraints"},
        {{32,1,0,UINT64_MAX},32,4,24,0,131072,256,"huge grid does not overflow ceil or exceedCTA cap"},
    };
    for(const auto& c:cases){
        const auto a=AdaTunerProfile::allocate(c.input);
        check(a.padded_threads==c.threads && a.rounded_registers==c.regs &&
              a.resident_ctas_per_sm==c.ctas && a.shared_carveout_bytes==c.shared &&
              a.l1_bytes==c.l1 && a.l1_ways==c.ways,c.label);
        const auto l1=AdaTunerProfile::l1(a);
        check(l1.num_sms==48 && l1.line_bytes==128 &&
              l1.capacity_bytes_per_sm==c.l1 && l1.ways==c.ways &&
              l1.sector32 && l1.write_allocate && !l1.store_bypass &&
              l1.dirty_protection_percent==25 && l1.hit_latency_cycles==34 &&
              l1.persistence==PerSmL1Persistence::KERNEL_FLUSH,"functional L1 opt-ins and dynamic geometry");
        PerSmL1Cache actual(l1);
        check(actual.config().capacity_bytes_per_sm/(128*actual.config().ways)==4,
              "all adaptive L1 geometries actually construct with four sets");
    }
}
static void invalid_inputs() {
    for(const auto bad:std::array<AdaKernelResources,8>{{
        {0,32,0,48},{1025,32,0,48},{128,32,0,0},{128,32,102401,48},
        {128,UINT32_MAX,0,48},{UINT32_MAX,0,0,48},
        {256,257,0,48},{1024,65,0,48}
    }}) rejects([&]{AdaTunerProfile::allocate(bad);},"invalid launch or zero legal residency rejected");
    // regs=0/shared=0 are explicit valid source quantities. This typed API has
    // no null/unknown representation; capture admission must reject missing
    // fields, rather than replacing unknowns with these valid zeros.
    check(AdaTunerProfile::allocate({128,0,0,48}).resident_ctas_per_sm==1,
          "explicit zero registers/shared is not an unknown sentinel");
}
static void structure_and_actual_sm() {
    auto c=make_rtx4000_ada_accelsim_structure_config({128,85,8192,4096});
    check(c.architecture_profile_name=="gtsim-ada-accelsim-structure-only-v1" &&
          c.architecture_parameter_source.find("NOT equivalent")!=std::string::npos,
          "structure adapter remains separately named and qualified");
    check(c.num_sms==48 && c.max_concurrent_blocks_per_sm==5 &&
          c.schedule_policy==SchedulePolicy::GTO && c.core_frequency_mhz==2175.0 &&
          c.dram_frequency_mhz==4500.5,"actual simulator config resources/clocks/GTO");
    check(c.per_sm_l1.capacity_bytes_per_sm==65536 && c.per_sm_l1.ways==128 &&
          c.per_sm_l1.hit_latency_cycles==34 && c.sram_latency_cycles==30,
          "structural adapter applies selected shared carveout");
    check(!c.per_sm_l1.sector32 && !c.per_sm_l1.write_allocate && c.per_sm_l1.store_bypass,
          "fine structure path deliberately retains legacy memory service semantics");
    L2Geometry g(c.l2_geometry,c.l2_cache_size_bytes,c.l2_line_size_bytes);
    check(c.l2_geometry.mode==L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE &&
          g.total_lines()==327680 && g.group_count()==20480 && g.capacity_per_group()==16 &&
          g.line_bytes()==128,"actual config builds40MiB 20x1024x16 geometry");
    check(AdaTunerProfile::channels==10 && AdaTunerProfile::subpartitions_per_channel==2 &&
          g.partition(0)==0 && g.partition(128)==1 && g.partition(2304)==18 &&
          g.partition(2432)==19 && g.partition(2560)==0 && g.set(2560)==1,
          "ten channels times two memory subpartitions are distinct from four SM schedulers");
    // Construct the actual header-defined SM; no simulator/GPU stepping or
    // mocked scheduler is used. These four sps are instruction schedulers,
    // not the twenty L2 memory subpartitions above.
    SM sm(3,c.warp_count,c.tensor_core_throughput,c.simd_throughput,
          c.sfu_throughput,c.ls_throughput_bytes,c.tensor_core_latency_cycles,
          c.simd_latency_cycles,c.sfu_latency_cycles,c.shfl_latency_cycles,c.ls_latency_cycles,
          c.tensor_core_width,c.simd_width,c.sfu_width,c.ls_width_bytes,
          c.sram_latency_cycles,c.sram_bandwidth_bytes_per_cycle,c.sram_queue_depth,
          c.O2_dynamic_allocation,c.schedule_policy,c.max_concurrent_blocks_per_sm,
          c.l2_max_transactions_per_cycle_per_sp,c.make_bulk_copy_engine_config(),
          c.tmem_cols_per_cta,c.tmem_issue_interval_cycles,c.tmem_read_latency_cycles,
          c.tmem_read_bandwidth_bytes_per_cycle,c.tmem_write_latency_cycles,
          c.tmem_write_bandwidth_bytes_per_cycle,c.tmem_queue_depth,
          c.startup_delay_cycles,c.block_schedule_latency_cycles);
    check(sm.sps.size()==4 && sm.sp_schedulers.size()==4 &&
          sm.max_resident_tbs==5 && sm.max_concurrent_blocks==5,"actual SM builds four schedulers and derivedCTA admission budget");
    unsigned register_bytes=0;
    for(unsigned i=0;i<sm.sps.size();++i) {
        const auto* sp=sm.sps[i];
        check(sp->subpartition_id==int(i) && sp->owning_sm_id==3 &&
              sp->sp_scheduler==sm.sp_schedulers[i] &&
              sp->sp_scheduler->schedule_policy==SchedulePolicy::GTO,"actual perSM scheduler ownership andGTO");
        register_bytes+=sp->sp_register->register_size;
    }
    check(register_bytes==65536*4,"actual four64KiB register objects match65536 32bitregister capacity");
}
int main() {
    allocations();invalid_inputs();structure_and_actual_sm();
    std::cout<<"{\"status\":\"PASS_ADA_PROFILE\",\"checks\":"<<checks
             <<",\"oracle_launch_cases\":40,\"actual_SM_constructed\":true,"
             <<"\"GPU_simulated\":false,\"timing_accuracy_claim\":false}\n";
}
