#pragma once
#include "simulator.h"
#include "ada_tuner_profile.h"
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
}
