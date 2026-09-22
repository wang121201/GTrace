#pragma once
#include "per_sm_l1.h"

namespace GTSim {
// Frozen r4 serial read-filter experiment. These are effective model sizes,
// not a physical Ada geometry claim. No occupancy or timing is inferred here.
// The caller must supply an observed (not merely preferred) shared carveout.
inline std::uint32_t ada_r4_nominal_l1_bytes(std::uint32_t observed_shared_bytes) {
    switch (observed_shared_bytes) {
    case 32768: return 98304;
    case 65536: return 65536;
    case 102400: return 28672;
    default: throw std::invalid_argument("r4 requires observed shared 32/64/100 KiB");
    }
}

inline const char* ada_r4_serial_model_id(bool r4) {
    return r4 ? "CLOCK_u128_s16_h2_c1062" : "LRU_u128_s4_h0_c1000";
}

inline PerSmL1Config make_ada_r4_serial_l1(std::uint32_t observed_shared_bytes, bool r4) {
    const auto nominal = ada_r4_nominal_l1_bytes(observed_shared_bytes);
    const std::uint32_t sets = r4 ? 16 : 4;
    const std::uint32_t scale = r4 ? 1062 : 1000;
    const auto ways = std::uint64_t(nominal) * scale / 1000 / (128 * sets);
    PerSmL1Config config;
    config.mode = PerSmL1Mode::MODELED_SET_ASSOCIATIVE;
    config.persistence = PerSmL1Persistence::KERNEL_FLUSH;
    config.num_sms = 1; // One serial dependent-read chain, not 48-SM scheduling.
    config.line_bytes = 128;
    config.ways = static_cast<std::uint32_t>(ways);
    config.capacity_bytes_per_sm = ways * 128 * sets;
    config.sector32 = true;
    config.write_allocate = false;
    config.dirty_protection_percent = 0;
    config.hit_latency_cycles = 0; // Functional completion only; never GPU latency.
    config.replacement = r4 ? PerSmL1ReplacementPolicy::CLOCK : PerSmL1ReplacementPolicy::LRU;
    config.hash_policy = r4 ? PerSmL1HashPolicy::ALLOCATION_RELATIVE_HASH2
                           : PerSmL1HashPolicy::LINEAR_GLOBAL;
    return config;
}
}
