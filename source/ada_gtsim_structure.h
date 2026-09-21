#pragma once
#include "simulator.h"
#include "ada_tuner_profile.h"
#include "ada_calibrated_profile.h"
namespace GTSim {
// Explicit STRUCTURAL adapter, not the sector functional profile's timing twin.
// Existing fine L2 remains line-fill/RFO and globally queued; caller must keep
// this mode separately named in reports. Use ada_cache_replay for sector traffic.
inline SimulatorConfig make_rtx4000_ada_accelsim_structure_config(const AdaKernelResources& r) {
    auto c=make_rtx4000_ada_footprint_reference_config();
    const auto a=AdaTunerProfile::allocate(r);
    c.architecture_profile_name="gtsim-ada-accelsim-structure-only-v1";
    c.architecture_parameter_source="Pinned XMU tuner e09511b9: SM occupancy/clocks and L2 geometry; fine L2 sector/MSHR/timing NOT equivalent";
    c.num_sms=48;c.max_concurrent_blocks_per_sm=a.resident_ctas_per_sm;
    c.schedule_policy=SchedulePolicy::GTO;
    c.core_frequency_mhz=AdaTunerProfile::core_mhz;c.dram_frequency_mhz=AdaTunerProfile::dram_mhz;
    c.l2_geometry=AdaTunerProfile::l2_geometry();c.l2_cache_size_bytes=AdaTunerProfile::l2_bytes;
    // Fine line-granularity service cannot claim the functional sector profile.
    // Existing L1 store bypass/whole-line validity, 272/604-cycle service and
    // calibrated old 311.66 GB/s rates remain legacy assumptions in this mode.
    c.per_sm_l1.capacity_bytes_per_sm=a.l1_bytes;c.per_sm_l1.ways=a.l1_ways;
    c.per_sm_l1.hit_latency_cycles=34;c.sram_latency_cycles=30;
    return c;
}

// Bounded INTERNAL-DRAM reference only: collapse the calibrated 34/239/324
// stage constants into the actual fine model's decision-to-completion fields.
// This is not an Accel-Sim timing emulator and is not an HBFSIM calibration.
// A caller supplying an external completion backend must NOT interpret these
// internal totals as an additional delay to that backend's service time.
inline SimulatorConfig make_rtx4000_ada_calibrated_internal_config(
        const AdaKernelResources& r,
        AdaCacheProfile profile = AdaCacheProfile::R2_ADAPTIVE,
        std::optional<std::uint32_t> observed_shared_carveout_bytes = std::nullopt) {
    const auto resolved = resolve_ada_cache_profile(profile,observed_shared_carveout_bytes);
    const auto allocation = AdaCalibratedProfile::allocate(r,profile,observed_shared_carveout_bytes);
    auto c = make_rtx4000_ada_accelsim_structure_config(r);
    if (resolved == AdaCacheProfile::TUNER_V1) return c; // exact old factory semantics
    c.architecture_profile_name = std::string("gtsim-ada-calibrated-internal-reference-")+
                                  ada_cache_profile_name(resolved);
    c.architecture_parameter_source =
        "r2 calibrated stage constants 34+239+324 collapsed into core-cycle internal reference; "
        "legacy bandwidth/queues and fine whole-line/RFO retained; NOT Accel-Sim or HBFSIM timing equivalent";
    c.max_concurrent_blocks_per_sm = allocation.resident_ctas_per_sm;
    c.l2_geometry = AdaTunerProfile::l2_geometry();
    // Fine has no byte/sector coverage delivery into L1: retain its legacy
    // whole-line/store-bypass behavior, only select capacity/ways/replacement.
    c.per_sm_l1.capacity_bytes_per_sm = allocation.l1_bytes;
    c.per_sm_l1.ways = allocation.l1_ways;
    c.per_sm_l1.replacement = AdaCalibratedProfile::policy(resolved);
    c.per_sm_l1.hit_latency_cycles = 34;
    c.l2_hit_latency_cycles = 34+239;
    c.l2_miss_penalty_cycles = 34+239+324;
    c.memory_model_semantics.l2_miss_latency = L2MissLatencySemantics::END_TO_END_FROM_MISS_DECISION;
    c.memory_model_semantics.end_to_end_miss_latency_cycles = 34+239+324;
    // Rates, budgets, queue capacities and latency ordering are deliberately
    // inherited. A 597-cycle ready time can still wait for service credit.
    return c;
}
}
