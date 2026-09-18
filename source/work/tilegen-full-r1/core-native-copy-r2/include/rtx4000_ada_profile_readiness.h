#ifndef RTX4000_ADA_PROFILE_READINESS_H
#define RTX4000_ADA_PROFILE_READINESS_H

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace GTSim {

enum class AdaProfileFieldState {
    ADMITTED,
    BLOCKED_MISSING_EVIDENCE,
    BLOCKED_SEMANTIC_MISMATCH,
    BLOCKED_UNAVAILABLE_OBSERVER,
};

inline const char* ada_profile_field_state_name(AdaProfileFieldState state) {
    switch (state) {
        case AdaProfileFieldState::ADMITTED:
            return "ADMITTED";
        case AdaProfileFieldState::BLOCKED_MISSING_EVIDENCE:
            return "BLOCKED_MISSING_EVIDENCE";
        case AdaProfileFieldState::BLOCKED_SEMANTIC_MISMATCH:
            return "BLOCKED_SEMANTIC_MISMATCH";
        case AdaProfileFieldState::BLOCKED_UNAVAILABLE_OBSERVER:
            return "BLOCKED_UNAVAILABLE_OBSERVER";
    }
    return "UNKNOWN";
}

struct AdaProfileFieldReadiness {
    const char* field;
    AdaProfileFieldState state;
    bool required_for_execution;
    bool required_for_hardware_matched_claim;
    const char* evidence;
    const char* consumer;
};

inline constexpr std::array<AdaProfileFieldReadiness, 25>
kRtx4000AdaProfileReadinessFieldsA3B3{{
    {"num_sms", AdaProfileFieldState::ADMITTED, true, true,
     "R73_M7_EXACT_HARDWARE_DESCRIPTOR", "SimulatorConfig.num_sms"},
    {"l2_cache_size_bytes", AdaProfileFieldState::ADMITTED, true, true,
     "R73_M7_EXACT_HARDWARE_DESCRIPTOR", "L2Cache.max_lines"},
    {"core_frequency_mhz", AdaProfileFieldState::ADMITTED, true, true,
     "R77_DIRECT_ORACLE_CLOCK_AT_SAMPLE", "DRAMModel.freq_ratio"},
    {"l2_hit_latency_cycles", AdaProfileFieldState::ADMITTED, true, true,
     "R77_DIRECT_ORACLE_WARM_DEPENDENT_LOAD", "L2Cache.completion_queue"},
    {"sram_latency_cycles", AdaProfileFieldState::ADMITTED, true, true,
     "R77_DIRECT_ORACLE_SHARED_DEPENDENT_LOAD", "Memory.latency"},
    {"sram_bandwidth_bytes_per_cycle", AdaProfileFieldState::ADMITTED, true, true,
     "R77_DIRECT_ORACLE_SHARED_ADMITTED_SERVICE", "Memory.bandwidth_bytes_per_cycle"},
    {"startup_delay_cycles", AdaProfileFieldState::ADMITTED, true, true,
     "R77_DIRECT_ORACLE_SCHEDULER_WAVES", "Simulator.run"},
    {"block_schedule_latency_cycles", AdaProfileFieldState::ADMITTED, true, true,
     "R77_DIRECT_ORACLE_SCHEDULER_WAVES", "ThreadBlockScheduler"},
    {"ada_ldgsts_per_source_rate", AdaProfileFieldState::ADMITTED, true, true,
     "R77_RATE_PLUS_R81_EXACT_RATIONAL_LOWERING", "BulkCopyUnit.source_bucket"},
    {"ada_ldgsts_aggregate_sm_rate", AdaProfileFieldState::ADMITTED, true, true,
     "R77_RATE_PLUS_R81_EXACT_RATIONAL_LOWERING", "BulkCopyUnit.aggregate_bucket"},
    {"internal_l2_line_accounting", AdaProfileFieldState::ADMITTED, true, false,
     "R82_MODELED_128B_FULL_LINE_EQUIVALENT_ONLY", "compute_dram_subop_lines"},

    {"l2_read_service_quantization", AdaProfileFieldState::BLOCKED_SEMANTIC_MISMATCH,
     true, true, "R77_711_BPC_NOT_EXACTLY_REPRESENTABLE_AS_INTEGER_128B_TX",
     "L2Cache.step.total_budget_tx"},
    {"l2_write_service_quantization", AdaProfileFieldState::BLOCKED_SEMANTIC_MISMATCH,
     true, true, "R77_540_BPC_NOT_EXACTLY_REPRESENTABLE_AS_INTEGER_128B_TX",
     "L2Cache.step.write_budget_tx"},
    {"dram_miss_latency_decomposition", AdaProfileFieldState::BLOCKED_SEMANTIC_MISMATCH,
     true, true, "R77_COLD_LOAD_604_CYCLES_BUT_MODEL_ADDS_L2_HIT_AFTER_DRAM",
     "DRAMModel.ready_cycle_plus_L2Cache.hit_latency"},
    {"dram_bandwidth_cycle_conversion", AdaProfileFieldState::BLOCKED_SEMANTIC_MISMATCH,
     true, true, "R77_EFFECTIVE_GBPS_BUT_MODEL_ROUNDS_THROUGH_DRAM_FREQUENCY",
     "Simulator.constructor_plus_DRAMModel.freq_ratio"},
    {"scheduler_consumption_of_tensor_fma_work",
     AdaProfileFieldState::BLOCKED_SEMANTIC_MISMATCH, true, true,
     "R80_DECLARED_FMA_WORK_NOT_READ_BY_SCHEDULER",
     "Scheduler.wgmma_issue_span"},
    {"ada_tensor_instruction_lowering", AdaProfileFieldState::BLOCKED_MISSING_EVIDENCE,
     true, true, "NATIVE_WGMMA_GRAPH_HAS_NO_ADA_WMMA_MMA_LOWERING",
     "Scheduler.group_issue"},
    {"tensor_core_latency_mapping", AdaProfileFieldState::BLOCKED_SEMANTIC_MISMATCH,
     true, true, "R77_DEPENDENT_WMMA_LATENCY_NOT_A_NATIVE_WGMMA_PIPELINE_LATENCY",
     "Pipeline.Tensor.latency"},
    {"ada_ldgsts_setup_latency_cycles", AdaProfileFieldState::BLOCKED_MISSING_EVIDENCE,
     true, true, "R77_SIZE_SLOPE_DOES_NOT_IDENTIFY_SETUP_INTERCEPT",
     "BulkCopyRequest.ready_cycle"},
    {"ada_ldgsts_command_issue_interval_cycles",
     AdaProfileFieldState::BLOCKED_MISSING_EVIDENCE, true, true,
     "R77_RATE_DOES_NOT_IDENTIFY_PER_WARP_COMMAND_INTERVAL",
     "Scheduler.tma_next_issue_cycle"},
    {"ada_store_path", AdaProfileFieldState::BLOCKED_MISSING_EVIDENCE,
     true, true, "R81_RETAINS_CP_SRAM2DRAM_TMA_AS_UNRESOLVED",
     "BulkCopyUnit.enqueue"},
    {"external_sector_exact_accuracy", AdaProfileFieldState::BLOCKED_UNAVAILABLE_OBSERVER,
     false, true, "R82_ERR_NVGPUCTRPERM_NO_REQUESTED_SECTOR_COUNTER",
     "hardware_matched_memory_claim"},
    {"gtsim_l1_accuracy", AdaProfileFieldState::BLOCKED_UNAVAILABLE_OBSERVER,
     false, true, "NO_GTSIM_L1_MODEL_OR_L1_OUTCOME_EVENT",
     "hardware_matched_memory_claim"},
    {"non_tensor_pipeline_parameters", AdaProfileFieldState::BLOCKED_MISSING_EVIDENCE,
     true, true, "SIMD_SFU_SHFL_LS_FIELDS_WOULD_INHERIT_LEGACY_DEFAULTS",
     "GPU.Subpartition.Pipeline"},
    {"queue_and_residency_parameters", AdaProfileFieldState::BLOCKED_MISSING_EVIDENCE,
     true, true, "L2_SRAM_QUEUE_DEPTH_TX_PER_CYCLE_AND_BLOCK_RESIDENCY_UNMEASURED",
     "L2Cache.Memory.ThreadBlockScheduler"},
}};

// R83 is immutable historical evidence. The current matrix is derived from
// that exact ledger and changes only the five A3B4 model-semantic fields whose
// implementations and CPU-only contracts are introduced by R84.
inline const std::array<AdaProfileFieldReadiness, 25>
kRtx4000AdaProfileReadinessFields = [] {
    auto fields = kRtx4000AdaProfileReadinessFieldsA3B3;
    fields[11] = {
        "l2_read_service_quantization", AdaProfileFieldState::ADMITTED,
        true, true, "R84_EXACT_RATIONAL_TOTAL_L2_SERVICE",
        "L2Cache.exact_l2_total_budget"};
    fields[12] = {
        "l2_write_service_quantization", AdaProfileFieldState::ADMITTED,
        true, true, "R84_EXACT_RATIONAL_WRITE_L2_SERVICE",
        "L2Cache.exact_l2_write_budget"};
    fields[13] = {
        "dram_miss_latency_decomposition", AdaProfileFieldState::ADMITTED,
        true, true, "R84_END_TO_END_MISS_DECISION_LATENCY",
        "DRAMModel.ready_cycle_and_L2Cache.direct_fill_completion"};
    fields[14] = {
        "dram_bandwidth_cycle_conversion", AdaProfileFieldState::ADMITTED,
        true, true, "R84_EXACT_RATIONAL_DRAM_BYTES_PER_CORE_CYCLE",
        "DRAMModel.exact_budget"};
    fields[15] = {
        "scheduler_consumption_of_tensor_fma_work",
        AdaProfileFieldState::ADMITTED, true, true,
        "R84_DECLARED_FMA_WORK_SCHEDULER_CONSUMER",
        "Scheduler.tensor_issue_span"};
    return fields;
}();

struct Rtx4000AdaProfileReadinessDecision {
    const char* gate;
    bool materialize_runnable_profile;
    bool hardware_accuracy_claimable;
    std::size_t field_count;
    std::size_t admitted_field_count;
    std::size_t execution_blocker_count;
    std::size_t hardware_claim_blocker_count;
    const std::array<AdaProfileFieldReadiness, 25>* fields;

    std::vector<std::string> blocking_field_names(bool hardware_claim) const {
        std::vector<std::string> result;
        if (fields == nullptr) {
            throw std::logic_error("readiness decision has no field ledger");
        }
        for (const auto& field : *fields) {
            const bool required = hardware_claim
                                      ? field.required_for_hardware_matched_claim
                                      : field.required_for_execution;
            if (required && field.state != AdaProfileFieldState::ADMITTED) {
                result.emplace_back(field.field);
            }
        }
        return result;
    }

    std::string first_blocking_field(bool hardware_claim) const {
        const auto fields = blocking_field_names(hardware_claim);
        return fields.empty() ? std::string("none") : fields.front();
    }
};

inline Rtx4000AdaProfileReadinessDecision
make_rtx4000_ada_profile_readiness_decision_from(
        const std::array<AdaProfileFieldReadiness, 25>& fields,
        const char* gate) {
    std::size_t admitted = 0;
    std::size_t execution_blockers = 0;
    std::size_t claim_blockers = 0;
    for (const auto& field : fields) {
        if (field.state == AdaProfileFieldState::ADMITTED) {
            ++admitted;
        } else {
            if (field.required_for_execution) ++execution_blockers;
            if (field.required_for_hardware_matched_claim) ++claim_blockers;
        }
    }
    return {
        gate,
        execution_blockers == 0,
        claim_blockers == 0,
        fields.size(),
        admitted,
        execution_blockers,
        claim_blockers,
        &fields,
    };
}

inline Rtx4000AdaProfileReadinessDecision
make_rtx4000_ada_profile_readiness_decision_a3b3() {
    return make_rtx4000_ada_profile_readiness_decision_from(
        kRtx4000AdaProfileReadinessFieldsA3B3,
        "A3B3_RUNNABLE_ADA_PROFILE_MATERIALIZATION_FAIL_CLOSED");
}

inline Rtx4000AdaProfileReadinessDecision
make_rtx4000_ada_profile_readiness_decision() {
    return make_rtx4000_ada_profile_readiness_decision_from(
        kRtx4000AdaProfileReadinessFields,
        "A3B4_L2_DRAM_MODEL_SEMANTICS_CLOSURE");
}

inline void require_rtx4000_ada_runnable_profile() {
    const auto decision = make_rtx4000_ada_profile_readiness_decision();
    if (!decision.materialize_runnable_profile) {
        throw std::runtime_error(
            "RTX 4000 Ada profile materialization rejected by A3B4: " +
            std::to_string(decision.execution_blocker_count) +
            " execution blockers; first=" +
            decision.first_blocking_field(false));
    }
}

}  // namespace GTSim

#endif  // RTX4000_ADA_PROFILE_READINESS_H
