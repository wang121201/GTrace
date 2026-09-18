#pragma once
// Generic reporting extracted from frozen adapter; no workload Runtime is used.
#include "backend.h"
#include <nlohmann/json.hpp>
namespace tiny_hbf_reporting {
using J=nlohmann::json;using U=std::uint64_t;
inline J native_identity(const hbfsim::physical::hbm::HbmConfig& c,const std::string& path){
    J timing=J::object();
#define TIME(field) timing[#field]=c.timing.field
    TIME(tRCDRD_ns);TIME(tRCDWR_ns);TIME(tCL_ns);TIME(tCWL_ns);TIME(tRP_ns);TIME(tRAS_ns);TIME(tRC_ns);
    TIME(tWR_ns);TIME(tRTP_ns);TIME(tCCD_S_cycles);TIME(tCCD_L_cycles);TIME(tRRD_S_ns);TIME(tRRD_L_ns);
    TIME(tFAW_ns);TIME(tWTR_S_ns);TIME(tWTR_L_ns);TIME(tRTW_ns);TIME(tREFI_ns);TIME(tRFC_ns);TIME(tRFCsb_ns);TIME(tRREFD_ns);
#undef TIME
    return {{"schema","NATIVE_HBFSIM_GDDR6_INTENT_IDENTITY_V1"},
        {"upstream_commit","d7a2ca64614a6d9ce8d7a69beb77ce78b66df1a8"},{"core_modified",false},
        {"intended_memory","GDDR6"},{"intended_protocol","GENERIC_BANKED_SUBSET"},
        {"raw_core_standard",std::string(c.standard)},{"raw_core_type","hbfsim::physical::hbm::HbmDevice"},
        {"raw_core_tier","HBM"},{"native_GDDR6_device_type_implemented",false},
        {"JEDEC_controller_validated",false},{"hardware_timing_calibrated",false},
        {"chip_identity_verified",false},{"physical_GPU_mapping_recovered",false},
        {"native_config_file",path},{"effective_parameter_source","NATIVE_SYSTEM_CONFIG_BUILDER_RESOLVED_CFG"},
        {"historical_GDDR6_JSON_checked_equal_to_corresponding_native_fields",false},
        {"resolved_device",{{"capacity_bytes",c.device.capacity_bytes},{"stacks",c.device.stacks},
            {"channels_per_stack",c.device.channels_per_stack},{"pseudo_channels_per_channel",c.device.pseudo_channels_per_channel},
            {"bank_groups_per_pseudo_channel",c.device.bank_groups_per_pseudo_channel},{"banks_per_group",c.device.banks_per_group},
            {"channel_row_size_bytes",c.device.channel_row_size_bytes},{"channel_width_bits",c.device.channel_width_bits},
            {"burst_length",c.device.burst_length},{"pin_rate_Gbps",c.device.pin_rate_Gbps},
            {"data_rate_per_command_clock",c.device.data_rate_per_command_clock}}},
        {"resolved_timing",timing},
        {"resolved_controller",{{"address_mapping_ns",c.controller.address_mapping_ns},
            {"refresh_enabled",c.controller.refresh_enabled},{"same_bank_refresh",c.controller.same_bank_refresh},
            {"queue_depth",c.controller.queue_depth},{"frfcfs_cap_ns",c.controller.frfcfs_cap_ns},
            {"interleave_bytes",c.effective_interleave_bytes()},
            {"replicate_symmetric_pseudo_channels",c.controller.replicate_symmetric_pseudo_channels}}},
        {"derived",{{"burst_bytes",c.burst_bytes()},{"command_clock_ns",c.command_clock_period_ns()},
            {"command_clock_MHz",c.command_clock_MHz()},
            {"aggregate_peak_DQ_GBps",c.channel_bandwidth_GBps()*c.device.stacks*c.device.channels_per_stack}}},
        {"address_mapping_scheme",std::string(c.address_mapping_scheme())},
        {"address_mapping_qualification","GENERIC_SERVICE_MAPPING_NOT_GPU_PHYSICAL_BANK_CHANNEL_MAP"}};
}
inline J native_statistics(const hbfsim::physical::hbm::HbmStats& s){
    J j=J::object();
#define STAT(field) j[#field]=s.field
    STAT(read_bytes);STAT(write_bytes);STAT(controller_buffer_read_bytes);STAT(controller_buffer_write_bytes);
    STAT(controller_buffer_transfers);STAT(controller_buffer_bus_busy_ns);STAT(row_hits);STAT(row_misses);
    STAT(row_conflicts);STAT(activations);STAT(precharges);STAT(refresh_count);STAT(bus_busy_ns);
    STAT(finish_ns);STAT(pseudo_channels);STAT(active_pseudo_channels);STAT(max_pseudo_channel_accesses);
    STAT(max_queue_occupancy);STAT(max_pseudo_channel_busy_ns);STAT(avg_active_pseudo_channel_busy_ns);
    STAT(replicated_requests);STAT(replicated_bursts);
#undef STAT
    j["first_arrival_ns"]=std::isfinite(s.first_arrival_ns)?J(s.first_arrival_ns):J(nullptr);
    J work=J::object();
#define WORK(field) work[#field]=s.stage_work.field
    WORK(ingress_queue_wait_ns);WORK(scheduler_queue_wait_ns);WORK(address_mapping_ns);WORK(translation_ns);
    WORK(mapping_dram_ns);WORK(write_buffer_dram_ns);WORK(refresh_stall_ns);WORK(precharge_ns);WORK(activation_ns);
    WORK(command_ns);WORK(array_read_ns);WORK(array_program_ns);WORK(array_erase_ns);WORK(media_lane_transfer_ns);
    WORK(page_buffer_ns);WORK(sram_staging_ns);WORK(channel_transfer_ns);WORK(tsv_transfer_ns);WORK(hb_io_transfer_ns);
    WORK(transport_latency_ns);WORK(ecc_queue_wait_ns);WORK(ecc_latency_ns);WORK(maintenance_ns);
#undef WORK
    j["overlapping_stage_work_ns"]=work;return j;
}
inline J path_statistics(const sg_hbf::MemoryPathBackend& b){
    const auto& s=b.path_statistics();J j;
#define PATH(field) j[#field]=s.field
    PATH(outer_accepted);PATH(outer_delivered);PATH(outer_rejected);PATH(physical_admitted);PATH(physical_completed);
    PATH(peak_outer_live);PATH(peak_ingress);PATH(peak_responses);PATH(peak_physical_live);
    PATH(request_service_busy_ps);PATH(response_service_busy_ps);PATH(response_payload_bytes);
    PATH(outer_admission_wait_ps);PATH(request_queue_wait_ps);PATH(request_fixed_work_ps);PATH(request_service_work_ps);PATH(native_admission_wait_ps);
    PATH(physical_core_ready_latency_ps);PATH(physical_observation_lag_ps);PATH(physical_observation_segment_ps);PATH(response_queue_wait_ps);
    PATH(response_fixed_work_ps);PATH(response_service_work_ps);PATH(delivery_poll_lag_ps);PATH(end_to_end_observed_latency_ps);
    PATH(last_native_arrival_ps);PATH(last_physical_core_ready_ps);PATH(last_response_ready_ps);PATH(last_delivery_ps);
    PATH(physical_release_while_response_pending);PATH(zero_path_direct_completions);PATH(step_calls);
    PATH(outer_identity_fnv1a64);PATH(inner_binding_fnv1a64);
#undef PATH
    j["first_issue_ps"]=s.first_issue_ps==UINT64_MAX?J(nullptr):J(s.first_issue_ps);
    j["first_native_arrival_ps"]=s.first_native_arrival_ps==UINT64_MAX?J(nullptr):J(s.first_native_arrival_ps);
    j["final_outer_live"]=b.queue_depth();j["final_physical_live"]=b.physical_queue_depth();
    j["final_ingress"]=b.ingress_queue_depth();j["final_responses"]=b.response_queue_depth();
    j["qualification"]="DIAGNOSTIC_UNCALIBRATED";
    j["physical_core_ready_is_raw_per_parent_device_finish"]=false;
    j["physical_core_ready_definition"]="inner completion cycle poll boundary; raw native finish only in native statistics";
    j["response_start_definition"]="physical completion observed at current poll; no future or retroactive return reservation";
    j["sums_are_overlapping_request_work_not_kernel_critical_path"]=true;
    J segments=J::object();
    for(std::size_t i=0;i<sg_hbf::kDeliveredSegmentNames.size();++i)segments[sg_hbf::kDeliveredSegmentNames[i]]=s.delivered_segments_ps[i];
    j["additive_delivered_segments_ps"]={{"segments",segments},{"sum",s.delivered_segment_sum_ps},
        {"parent_count",s.delivered_parent_segment_closures},{"closed",s.delivered_segment_sum_ps==s.end_to_end_observed_latency_ps&&s.delivered_parent_segment_closures==s.outer_delivered},
        {"definition","sum over delivered parents of observed_delivery_ps minus original_issue_ps; per-parent closure enforced"},
        {"scope","DELIVERED_PARENTS_ONLY; closes at every poll, including while other parents remain live"},
        {"kernel_elapsed_time",false}};
    j["physical_observation_lag_ps_semantics"]="NONADDITIVE_OBSERVATION_DIAGNOSTIC; zero response already includes this lag in delivery_poll_lag";
    j["physical_observation_segment_ps_semantics"]="PHYSICAL_COMPLETED_WORK: zero in direct-return mode; observed-minus-core-ready otherwise; use delivered-only ledger for additive end-to-end";
    j["legacy_work_counter_scope"]="Mixed accepted/physical-completed/delivered prefixes; do not sum legacy fields as end-to-end segments";
    j["request_rate_unit"]="parent/ps";j["response_rate_unit"]="read_payload_byte/ps";
    j["service_rounding"]="ceil per serial service quantum to integer ps";
    j["request_admission_policy"]=b.path_config().direct_request()?"DIRECT_NATIVE_BACKPRESSURE_FOR_ZERO_REQUEST_PATH":"BOUNDED_OUTER_FIFO_THEN_NATIVE_CREDITS";
    j["native_credits_released_at_physical_completion_before_response"]=true;
    j["outer_inner_identity_spaces_separate"]=true;j["trace_saved"]=false;
    return j;
}
}
