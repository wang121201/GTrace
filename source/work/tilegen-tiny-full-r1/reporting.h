#pragma once
#include "source.h"
#include <chrono>

namespace tiny_full {
using HostClock=std::chrono::steady_clock;
double elapsed(HostClock::time_point t) {
    return std::chrono::duration<double>(HostClock::now()-t).count();
}

// Same constructor arguments as ContinuousL2Session. Only the GPU scheduling
// core is replaced: the existing cache, dirty sectors and native backend remain.
std::unique_ptr<g::L2Cache> make_l2(const g::SimulatorConfig& cfg,coupling::Runtime& memory) {
    p::need(cfg.memory_model_semantics.dram_bandwidth!=
        g::DramBandwidthSemantics::LEGACY_INTEGER_DRAM_CYCLE_WITH_FREQUENCY_RATIO,
        "packet pilot requires the frozen exact-rational external memory profile");
    return std::make_unique<g::L2Cache>(cfg.l2_cache_size_bytes,cfg.l2_line_size_bytes,
        cfg.l2_hit_latency_cycles,cfg.l2_bandwidth_bytes_per_cycle,
        cfg.l2_write_bandwidth_bytes_per_cycle,cfg.l2_queue_depth,cfg.l2_bypass_cache,
        0,cfg.l2_miss_penalty_cycles,cfg.core_frequency_mhz,cfg.dram_frequency_mhz,
        cfg.memory_model_semantics,cfg.per_sm_l1,memory.backend.get(),memory.mapper.get(),
        cfg.l2_geometry);
}
J runtime_stats(const rt::Stats& s) {
    J out;
#define FIELD(x) out[#x]=s.x
    FIELD(ctas_added);FIELD(ctas_completed);FIELD(groups_completed);
    FIELD(logical_members_completed);FIELD(compute_elements_completed);FIELD(tensor_fma_completed);
    FIELD(logical_external_edges_released);FIELD(typed_group_events_released);
    FIELD(compute_subops_issued);FIELD(external_subops_accepted);
    FIELD(external_attempts);FIELD(external_retries);FIELD(events_processed);
    FIELD(schedule_invocations);FIELD(warp_candidate_visits);
#undef FIELD
    return out;
}
void require_closed(g::L2Cache& l2,coupling::Runtime& memory) {
    const auto s=l2.runtime_statistics();const auto& a=memory.backend->admission_statistics();
    p::need(l2.is_quiescent()&&a.accepted==a.completed&&a.reserved_bursts==0&&
        memory.backend->queue_depth()==0,"packet native finite-credit closure");
    p::need(s.accepted_transactions==s.processed_transactions&&
        s.dram_fill_requests==s.dram_fill_completions&&
        s.dram_writeback_requests==s.dram_writeback_completions&&
        s.dram_fill_bytes==s.dram_fill_completed_bytes&&
        s.dram_writeback_bytes==s.dram_writeback_completed_bytes,
        "packet L2 request and byte ledger closure");
    const auto dirty=l2.dirty_sector_snapshot();
    p::need(g::kTilegenDirtySectorMode==2&&dirty.dirty_sector_ledger_closed&&
        dirty.writeback_byte_ledger_closed&&dirty.unknown_store_rejections==0&&
        dirty.pending_dirty_lines==0&&dirty.pending_dirty_sectors==0&&
        dirty.outstanding_writeback_bytes==0&&dirty.unadmitted_writeback_bytes==0,
        "packet 32B dirty-sector ledger closure");
    U run_sectors=0;
    for(unsigned n=1;n<=4;++n)run_sectors+=n*dirty.writeback_run_lengths[n];
    p::need(s.dram_writeback_bytes==32*dirty.evicted_dirty_sectors&&
        s.dram_writeback_bytes==32*run_sectors,"packet 32B dirty-sector byte accounting");
}
J dirty_stats(const g::L2Cache& l2) {
    const auto ds=l2.dirty_sector_snapshot();J out;
#define FIELD(x) out[#x]=ds.x
    FIELD(store_mask_requests);FIELD(range_visits);FIELD(range_intersections);
    FIELD(unknown_store_rejections);FIELD(store_mask_popcounts);FIELD(eviction_popcounts);
    FIELD(writeback_run_lengths);FIELD(eviction_masks);FIELD(eviction_run_counts);
    FIELD(dirty_sector_creations);FIELD(evicted_dirty_sectors);FIELD(resident_dirty_lines);
    FIELD(resident_dirty_sectors);FIELD(pending_dirty_lines);FIELD(pending_dirty_sectors);
    FIELD(outstanding_writeback_bytes);FIELD(unadmitted_writeback_bytes);
    FIELD(dirty_sector_ledger_closed);FIELD(writeback_byte_ledger_closed);
#undef FIELD
    out["mode"]=g::kTilegenDirtySectorMode;return out;
}
J native_admission(const coupling::Runtime& memory) {
    const auto& a=memory.backend->admission_statistics();J out;
#define FIELD(x) out[#x]=a.x
    FIELD(accepted);FIELD(completed);FIELD(blocked);FIELD(physical_calls);
    FIELD(peak_live);FIELD(peak_pseudo_channel_credits);FIELD(reserved_bursts);
    FIELD(peak_reserved_bursts);FIELD(native_service_before_calls);FIELD(native_service_steps);
    FIELD(native_enqueue_execution_checks);FIELD(native_enqueue_service_violations);
    FIELD(native_completions_posted);FIELD(future_completions_held);FIELD(peak_due_completions);
#undef FIELD
    return out;
}

} // namespace tiny_full
