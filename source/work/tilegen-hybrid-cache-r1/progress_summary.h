#pragma once
// Bounded kernel aggregates for an interrupted long attempt. No address,
// instruction, node, lane, or request trace is emitted.
#include <iostream>
#include <nlohmann/json.hpp>

namespace hybrid_full {
inline void emit_completed_summary(const nlohmann::json& row,std::size_t completed) {
    using J=nlohmann::json;
    const auto& e=row.at("execution");
    const bool tiny=e.contains("tiny");
    J s={{"schema","COMPLETED_KERNEL_AGGREGATE_V1"},
        {"completed_calls",completed},{"source_launch_key",row.at("source_launch_key")},
        {"family",row.at("family")},{"phase",row.at("phase")},{"layer",row.at("layer")},
        {"route",tiny?"tiny":"fine"},{"CTAs",e.at("retired_ctas")},
        {"logical_nodes",e.at("retired_nodes")},{"start_cycle",e.at("start_cycle")},
        {"end_cycle",e.at("quiescent_end_cycle")},{"window_cycles",e.at("window_cycles")},
        {"DRAM_read_bytes",e.at("DRAM_read_bytes")},{"DRAM_write_bytes",e.at("DRAM_write_bytes")},
        {"requested_read_bytes",e.at("requested_read_bytes")},
        {"requested_write_bytes",e.at("requested_write_bytes")},
        {"host_engine_seconds",e.at("host_engine_seconds")},
        {"events_processed",nullptr},{"actual_dependency_counter_updates",nullptr},
        {"schedule_invocations",nullptr},{"warp_candidate_visits",nullptr},
        {"host_stage_seconds",nullptr}};
    if(tiny) {
        const auto& t=e.at("tiny");const auto& r=t.at("runtime");
        s["events_processed"]=r.at("events_processed");
        s["actual_dependency_counter_updates"]=r.at("typed_group_events_released");
        s["schedule_invocations"]=r.at("schedule_invocations");
        s["warp_candidate_visits"]=r.at("warp_candidate_visits");
        s["host_stage_seconds"]=t.at("host_stage_seconds");
    } else {
        s["schedule_invocations"]=e.at("scheduler_visits").at("schedule_invocations");
        s["warp_candidate_visits"]=e.at("scheduler_visits").at("warp_attempts");
    }
    std::cerr<<J({{"completed_kernel_summary",s}}).dump()<<std::endl;
}
} // namespace hybrid_full
