#pragma once
#include "cache_geometry.h"
#include "per_sm_l1.h"
#include <nlohmann/json.hpp>

namespace native_cache {
// Serialize the actual configuration; geometry equality is not sector/timing equality.
template<class Config>
nlohmann::json profile(const Config& cfg) {
    using namespace GTSim;
    const auto& l1=cfg.per_sm_l1;
    const L2Geometry l2(cfg.l2_geometry,cfg.l2_cache_size_bytes,cfg.l2_line_size_bytes);
    const bool paper=cfg.l2_geometry.mode==L2GeometryMode::PAPER_ADA_SET_ASSOCIATIVE;
    return {
        {"schema","TILEGEN_CACHE_CONFIGURATION_V1"},
        {"profile",paper?"TILEGEN_PAPER_ADA_GEOMETRY_R1":"LEGACY_FULLY_ASSOCIATIVE"},
        {"reference","PAPER_ADA_L2_V1_WORKFLOW_1"},{"hardware_calibrated",false},
        {"L1",{{"mode",per_sm_l1_mode_name(l1.mode)},
            {"SMs",l1.num_sms},{"bytes_per_SM",l1.capacity_bytes_per_sm},
            {"line_bytes",l1.line_bytes},{"ways",l1.ways},
            {"sets_per_SM",l1.capacity_bytes_per_sm/l1.line_bytes/l1.ways},
            {"index","linear original VA line modulo sets"},{"replacement","LRU"},
            {"persistence",per_sm_l1_persistence_name(l1.persistence)},
            {"store_policy",l1.store_bypass?"bypass_no_L1_touch":"write_through_no_allocate"},
            {"validity_bytes",l1.sector_validity?32:l1.line_bytes},{"modeled_hit_latency_cycles",l1.hit_latency_cycles}}},
        {"L2",{{"bytes",cfg.l2_cache_size_bytes},{"line_bytes",cfg.l2_line_size_bytes},
            {"logical_slices",paper?20:1},{"sets_per_slice",paper?1024:1},
            {"ways",l2.capacity_per_group()},{"groups",l2.group_count()},
            {"index",paper?"PAPER_ADA_QUOTIENT_BIT8_XOR":"fully_associative"},
            {"replacement","group_local_LRU"},{"persistence","CROSS_KERNEL_PERSISTENT"},
            {"final_dirty_flush",false},{"dirty_sector_bytes",32},{"write_request_bytes",32},
            {"read_fill_bytes",128},{"store_miss_policy","128B_RFO"}}},
        {"remaining_memgen_behavior_differences",{
            "TileGen whole-line validity/read fill vs MemGen32B sectors and known bytes",
            "TileGen immediate128B store RFO vs MemGen lazy write allocation without RFO",
            "TileGen individual32B writes vs MemGen contiguous dirty-sector spans",
            "Source issue order and in-flight timing remain engine-specific"}}
    };
}
}
