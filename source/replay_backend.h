#pragma once
#include "trace_replay.h"
#include <memory>

namespace replay_backend {
using U=std::uint64_t;using J=nlohmann::json;
namespace g=GTSim;namespace p=hbfsim::physical;
struct Admission {
    U accepted=0,completed=0,blocked=0,native_completions_posted=0,peak_live=0,last_completion_ps=0;
    U actual_request_shape_fnv1a64=14695981039346656037ULL;
};
struct Snapshot {
    Admission admission;
    g::L2DramRuntimeStatistics traffic;
    J physical,diagnostics;
    double first_arrival_ns=std::numeric_limits<double>::infinity(),finish_ns=0;
    U physical_read_bytes=0,physical_write_bytes=0;
    bool source_bytes_equal_physical=false;
    std::string kind;
};
class Backend {
public:
    virtual ~Backend()=default;
    virtual sg_hbf::Clock clock()const=0;
    virtual bool try_enqueue(const g::L2DramRequest&,U)=0;
    virtual std::vector<g::L2DramCompletion> step(U)=0;
    virtual U queue_depth()const=0;
    virtual U next_completion_cycle()const=0;
    virtual bool all_live_schedules_known()const=0;
    virtual U max_live()const=0;
    // Called after all source requests AND stage compute have completed.
    // Device maintenance is not a source parent and never changes its FNV.
    virtual U drain_cycle(U current_cycle)=0;
    virtual void finalize()const=0;
    virtual Snapshot snapshot()const=0;
};
class HbmBackend final:public Backend {
    sg_hbf::Clock clock_;
    sg_hbf::HbfCreditBackend backend_;
public:
    HbmBackend(sg_hbf::Clock clock,const p::hbm::HbmConfig& config,U max_live=4096,U credits=32,
               sg_hbf::DrainMode drain=sg_hbf::DrainMode::Global)
        :clock_(clock),backend_(clock,config,max_live,credits,1,drain){}
    sg_hbf::Clock clock()const override{return clock_;}
    bool try_enqueue(const g::L2DramRequest& r,U cycle)override{return backend_.try_enqueue(r,cycle);}
    std::vector<g::L2DramCompletion> step(U cycle)override{return backend_.step(cycle);}
    U queue_depth()const override{return backend_.queue_depth();}
    U next_completion_cycle()const override{return backend_.next_completion_cycle();}
    bool all_live_schedules_known()const override {
        const auto&a=backend_.admission_statistics();return a.accepted==a.native_completions_posted;
    }
    U max_live()const override{return backend_.max_live();}
    U drain_cycle(U cycle)override {
        native_trace::need(backend_.queue_depth()==0,"HBM final drain requires completed source parents");return cycle;
    }
    void finalize()const override{backend_.finalize();}
    Snapshot snapshot()const override {
        const auto&a=backend_.admission_statistics();const auto physical=backend_.physical_statistics();
        const auto service=backend_.service_diagnostics();Snapshot result;
        result.admission={a.accepted,a.completed,a.blocked,a.native_completions_posted,a.peak_live,
            a.last_completion_ps,a.actual_request_shape_fnv1a64};
        result.traffic=backend_.statistics();result.physical=trace_replay::physical_json(physical);
        result.first_arrival_ns=physical.first_arrival_ns;result.finish_ns=physical.finish_ns;
        result.physical_read_bytes=physical.read_bytes;result.physical_write_bytes=physical.write_bytes;
        result.source_bytes_equal_physical=true;result.kind="HBM_COMMAND_DEVICE";
        result.diagnostics={{"native_enqueue_execution_checks",a.native_enqueue_execution_checks},
            {"native_enqueue_service_violations",a.native_enqueue_service_violations},
            {"credits_per_channel_bursts",backend_.credits_per_pc()},
            {"peak_reserved_bursts",a.peak_reserved_bursts},{"peak_channel_burst_credits",a.peak_pseudo_channel_credits},
            {"service_quantum",1},{"drain_mode",service.drain==sg_hbf::DrainMode::Global?"global":"independent"},
            {"independent_fast_drains",service.independent_fast_drains},{"fallback_global_drains",service.fallback_global_drains},
            {"final_reserved_bursts",a.reserved_bursts},{"parent_credit_unit","source request; per-channel native 32B bursts"}};
        return result;
    }
};
} // namespace replay_backend
