#include "../source/ada_gtsim_structure.h"
#include <iostream>
#include <map>

using namespace GTSim;
using U=std::uint64_t;
static unsigned checks=0;
static void check(bool ok,const char* why) { ++checks;if(!ok)throw std::runtime_error(why); }
template<class F> static void rejects(F fn,const char* why) {
    bool caught=false;try{fn();}catch(const std::invalid_argument&){caught=true;}check(caught,why);
}

// Mode2 requires declared finite admission, while the existing internal
// DRAMModel has an unbounded interface. This test-only wrapper adds credits,
// delegates ALL timing/bandwidth/completion work to the actual DRAMModel, and
// never substitutes a synthetic completion timestamp or hardware backend.
struct FiniteInternal final:L2DramCompletionBackend {
    DRAMModel inner;
    std::vector<L2DramRequest> issued;
    std::vector<L2DramCompletion> completed;
    explicit FiniteInternal(const SimulatorConfig& c)
        :inner(0,c.l2_miss_penalty_cycles,128,c.core_frequency_mhz,c.dram_frequency_mhz,c.memory_model_semantics){}
    U issue_cycle_to_ps(U cycle)const override{return inner.issue_cycle_to_ps(cycle);}
    std::size_t admission_capacity()const override{return 64;}
    void enqueue(const L2DramRequest& r)override{check(try_enqueue(r,r.issue_cycle),"finite wrapper admission");}
    bool try_enqueue(const L2DramRequest& r,U)override{
        if(inner.queue_depth()>=64)return false;
        issued.push_back(r);inner.enqueue(r);return true;
    }
    std::vector<L2DramCompletion> step(U cycle)override{
        auto done=inner.step(cycle);completed.insert(completed.end(),done.begin(),done.end());return done;
    }
    const L2DramRuntimeStatistics& statistics()const override{return inner.statistics();}
    std::size_t queue_depth()const override{return inner.queue_depth();}
};

static void config_contract() {
    const AdaKernelResources r{128,16,0,48};
    const auto old=make_rtx4000_ada_accelsim_structure_config(r);
    const auto v1=make_rtx4000_ada_calibrated_internal_config(r,AdaCacheProfile::TUNER_V1);
    check(v1.architecture_profile_name==old.architecture_profile_name &&
          v1.l2_hit_latency_cycles==272 && v1.l2_miss_penalty_cycles==604 &&
          v1.memory_model_semantics.end_to_end_miss_latency_cycles==604 &&
          v1.per_sm_l1.capacity_bytes_per_sm==131072,"v1 factory returned unchanged behavior");
    const auto c=make_rtx4000_ada_calibrated_internal_config(r);
    check(c.l2_hit_latency_cycles==273 && c.l2_miss_penalty_cycles==597 &&
          c.memory_model_semantics.end_to_end_miss_latency_cycles==597 &&
          c.memory_model_semantics.l2_miss_latency==L2MissLatencySemantics::END_TO_END_FROM_MISS_DECISION,
          "34/239/324 totals are consumed fields, not extra total stages");
    check(c.per_sm_l1.capacity_bytes_per_sm==98304 && c.per_sm_l1.ways==192 &&
          c.per_sm_l1.replacement==PerSmL1ReplacementPolicy::LRU &&
          !c.per_sm_l1.sector32 && !c.per_sm_l1.write_allocate && c.per_sm_l1.store_bypass,
          "fine whole-line/store-bypass compatibility explicitly retained");
    check(c.memory_model_semantics.dram_rate.numerator_bytes==old.memory_model_semantics.dram_rate.numerator_bytes &&
          c.memory_model_semantics.dram_rate.denominator_cycles==old.memory_model_semantics.dram_rate.denominator_cycles &&
          c.memory_model_semantics.l2_total_rate.numerator_bytes==old.memory_model_semantics.l2_total_rate.numerator_bytes &&
          c.l2_queue_depth==old.l2_queue_depth && c.dram_bandwidth_gbps==old.dram_bandwidth_gbps,
          "old bandwidth and queues were not recalibrated");
    const auto fifo=make_rtx4000_ada_calibrated_internal_config(r,AdaCacheProfile::R3_FIFO_SHARED100,102400);
    check(fifo.per_sm_l1.capacity_bytes_per_sm==24576 && fifo.per_sm_l1.ways==3 &&
          fifo.per_sm_l1.replacement==PerSmL1ReplacementPolicy::FIFO && !fifo.per_sm_l1.sector32,
          "r3 static geometry and FIFO reach fine structure without changing sector behavior");
    rejects([&]{make_rtx4000_ada_calibrated_internal_config(r,AdaCacheProfile::R3_FIFO_SHARED100);},"fixed observed metadata mandatory");
}

struct Result { U issue,fill,node_done,hit_decision,hit_done; };
static Result exercise(bool epoch) {
    const auto c=make_rtx4000_ada_calibrated_internal_config({128,0,0,48});
    FiniteInternal backend(c);
    L2Cache cache(c.l2_cache_size_bytes,128,c.l2_hit_latency_cycles,
        c.l2_bandwidth_bytes_per_cycle,c.l2_write_bandwidth_bytes_per_cycle,c.l2_queue_depth,
        false,0,c.l2_miss_penalty_cycles,c.core_frequency_mhz,c.dram_frequency_mhz,
        c.memory_model_semantics,c.per_sm_l1,&backend,nullptr,c.l2_geometry);
    std::map<int,U> done;
    auto tick=[&](U cycle){
        std::vector<int> nodes;
        if(epoch){auto completions=backend.step(cycle);nodes=cache.service_epoch(cycle,1,backend,completions);}
        else nodes=cache.step(cycle);
        for(int node:nodes)check(done.emplace(node,cycle).second,"node completes exactly once");
    };
    auto access=[&](int id,U cycle){
        DAGNode n(id,"timing","LS","ld.dram2reg",0,{},0,{});
        n.sm_id=0;n.matrix_id=1;n.warp_id=0;n.async_copy_bypass_l1=true;
        check(cache.enqueue_transaction_key(n,{1,0},false,cycle,0),"test request admitted");
    };
    access(1,0);
    U cycle=1; // first epoch spans(0,1], following ingress at boundary0
    for(;cycle<2000 && !done.count(1);++cycle)tick(cycle);
    check(done.count(1) && backend.issued.size()==1 && backend.completed.size()==1,"cold read closes one actual fill");
    const U issue=backend.issued[0].issue_cycle,fill=backend.completed[0].completion_cycle;
    check(fill-issue==597,"actual DRAMModel waits597 core cycles from miss issue");
    check(done.at(1)==fill,"END_TO_END must not append another273 cycles");
    const U hit_decision=cycle;
    access(2,cycle-1); // accepted at previous boundary, legal for span1 epoch
    tick(cycle++);
    check(cache.runtime_statistics().read_hits==1,"hit decision occurs at the first available service boundary");
    for(;cycle<3000 && !done.count(2);++cycle)tick(cycle);
    check(done.count(2) && backend.issued.size()==1,"resident L2 hit creates no second fill");
    check(done.at(2)-hit_decision==273,"L2 hit completes273 cycles after decision with available service");
    check(cache.is_quiescent(),"all L1/L2/backend obligations closed");
    const auto s=cache.runtime_statistics();
    check(s.read_hits==1 && s.read_miss_allocates==1 && s.dram_fill_bytes==128 && s.dram_fill_completed_bytes==128,"cold/hit traffic unchanged");
    return {issue,fill,done.at(1),hit_decision,done.at(2)};
}
int main(){try{
    config_contract();const auto step=exercise(false),epoch=exercise(true);
    check(step.fill-step.issue==epoch.fill-epoch.issue && step.node_done-step.issue==epoch.node_done-epoch.issue &&
          step.hit_done-step.hit_decision==epoch.hit_done-epoch.hit_decision,"step and span1 service_epoch relative delays agree");
    std::cout<<"{\"status\":\"PASS_ADA_CALIBRATED_TIMING\",\"checks\":"<<checks
             <<",\"cold_fill_cycles\":597,\"L2_hit_cycles\":273,\"actual_DRAMModel\":true,"
             <<"\"test_only_finite_credit_wrapper\":true,\"HBFSIM_executed\":false,\"hardware_timing_match_claim\":false}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
