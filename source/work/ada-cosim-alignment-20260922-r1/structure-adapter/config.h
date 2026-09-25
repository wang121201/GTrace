#pragma once
#include "simulator.h"
#include <string>
namespace GTSim {
// External completion backend REQUIRED by this candidate's qualification.
// Does not construct/change the backend, clock wrapper, h288 policy, L1 validity
// mode, caller SM/subpartition metadata, service mapper, or any queue resource.
// Supply Clock(40000,87) to the existing HBF wrapper for both nonlegacy profiles.
// L1 hits use34/39. L1 misses immediately enqueue L2; L2 hits use272/276 total.
// The ROP residual is folded into hit only; it is NOT added before/after a miss.
// Internal604 fields remain unchanged and are unused with external completion.
inline SimulatorConfig make_ada_cosim_alignment_config(const std::string& profile) {
    auto c = make_rtx4000_ada_footprint_reference_config();
    if (profile == "legacy") return c;
    if (profile != "tuner-v1" && profile != "J-candidate")
        throw std::invalid_argument("unknown Ada structure profile");
    c.l2_geometry = L2GeometryConfig::accelsim_rtx4000_ada();
    c.core_frequency_mhz = 2175.0;
    c.per_sm_l1.hit_latency_cycles = profile == "tuner-v1" ? 34 : 39;
    c.l2_hit_latency_cycles = profile == "tuner-v1" ? 272 : 276;
    c.architecture_profile_name = "ada-structure-" + profile + "-external-only";
    c.architecture_parameter_source =
        "Pinned AccelSim software geometry and paired hit settings; fixed2175MHz. "
        "External HBFSIM exclusively owns misses; frontend miss pipeline unknown. "
        "Unchanged backend physical route is not proven equal to software20subpartitions. "
        "No hardware accuracy admission; J candidate is unpromoted.";
    return c;
}
} // namespace GTSim
