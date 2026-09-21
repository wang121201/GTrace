#pragma once
#include "ada_tuner_profile.h"
#include <optional>
#include <string>

namespace GTSim {

enum class AdaCacheProfile : std::uint8_t {
    TUNER_V1,
    R2_ADAPTIVE,
    R2_SHARED64,
    R2_SHARED100,
    R3_FIFO_SHARED32,
    R3_FIFO_SHARED64,
    R3_FIFO_SHARED100,
};

inline const char* ada_cache_profile_name(AdaCacheProfile profile) {
    switch (profile) {
    case AdaCacheProfile::TUNER_V1: return "tuner-v1";
    case AdaCacheProfile::R2_ADAPTIVE: return "r2-adaptive";
    case AdaCacheProfile::R2_SHARED64: return "r2-shared64";
    case AdaCacheProfile::R2_SHARED100: return "r2-shared100";
    case AdaCacheProfile::R3_FIFO_SHARED32: return "r3-fifo-shared32";
    case AdaCacheProfile::R3_FIFO_SHARED64: return "r3-fifo-shared64";
    case AdaCacheProfile::R3_FIFO_SHARED100: return "r3-fifo-shared100";
    }
    throw std::invalid_argument("unknown Ada cache profile enum");
}

inline AdaCacheProfile parse_ada_cache_profile(const std::string& name) {
    for (const auto p : {AdaCacheProfile::TUNER_V1,AdaCacheProfile::R2_ADAPTIVE,
                        AdaCacheProfile::R2_SHARED64,AdaCacheProfile::R2_SHARED100,
                        AdaCacheProfile::R3_FIFO_SHARED32,AdaCacheProfile::R3_FIFO_SHARED64,
                        AdaCacheProfile::R3_FIFO_SHARED100})
        if (name == ada_cache_profile_name(p)) return p;
    throw std::invalid_argument("unknown Ada cache profile name: " + name);
}

struct AdaCacheProfileSettings {
    const char* name;
    // Repo-relative path to the actual frozen bytes used by this profile.
    const char* source_config;
    const char* evidence_status;
    bool experimental;
    bool adaptive; // source -gpgpu_adaptive_cache_config, even for single-option r2
    bool requires_observed_carveout;
    std::uint32_t fixed_shared_carveout_bytes; // zero means adaptive, not observed zero
    std::uint32_t l1_sets;
    std::uint32_t fixed_l1_bytes; // zero means derived from the chosen carveout
    std::uint32_t fixed_l1_ways;
    PerSmL1ReplacementPolicy replacement;
};

inline AdaCacheProfileSettings ada_cache_profile_settings(AdaCacheProfile p) {
    using P = AdaCacheProfile;
    using R = PerSmL1ReplacementPolicy;
    const auto* name = ada_cache_profile_name(p); // also rejects invalid enum
    switch (p) {
    case P::TUNER_V1:
        return {name,"configs/rtx4000-ada-accelsim-v1/source/gpgpusim.config",
                "historical_tuner_software_reference",false,true,false,0,4,0,0,R::LRU};
    case P::R2_ADAPTIVE:
        return {name,"configs/rtx4000-ada-calibrated/r2-adaptive/source/gpgpusim.config",
                "default_candidate_not_full_hardware_match",false,true,false,0,4,0,0,R::LRU};
    case P::R2_SHARED64:
        return {name,"configs/rtx4000-ada-calibrated/r2-shared64/source/gpgpusim.config",
                "observed_carveout_candidate_not_full_hardware_match",false,true,true,65536,4,65536,128,R::LRU};
    case P::R2_SHARED100:
        return {name,"configs/rtx4000-ada-calibrated/r2-shared100/source/gpgpusim.config",
                "observed_carveout_candidate_not_full_hardware_match",false,true,true,102400,4,28672,56,R::LRU};
    case P::R3_FIFO_SHARED32:
        return {name,"configs/rtx4000-ada-calibrated/r3-fifo-shared32/source/gpgpusim.config",
                "experimental_not_promoted",true,false,true,32768,64,98304,12,R::FIFO};
    case P::R3_FIFO_SHARED64:
        return {name,"configs/rtx4000-ada-calibrated/r3-fifo-shared64/source/gpgpusim.config",
                "experimental_not_promoted",true,false,true,65536,64,65536,8,R::FIFO};
    case P::R3_FIFO_SHARED100:
        return {name,"configs/rtx4000-ada-calibrated/r3-fifo-shared100/source/gpgpusim.config",
                "experimental_not_promoted",true,false,true,102400,64,24576,3,R::FIFO};
    }
    throw std::invalid_argument("unknown Ada cache profile");
}

inline AdaCacheProfile resolve_ada_cache_profile(
        AdaCacheProfile requested,
        std::optional<std::uint32_t> observed_shared_carveout_bytes = std::nullopt) {
    const auto s = ada_cache_profile_settings(requested);
    if (requested == AdaCacheProfile::TUNER_V1) {
        if (observed_shared_carveout_bytes &&
            std::find(AdaTunerProfile::shared_options.begin(),AdaTunerProfile::shared_options.end(),
                      *observed_shared_carveout_bytes) == AdaTunerProfile::shared_options.end())
            throw std::invalid_argument("observed carveout is not a tuner-v1 option");
        return requested;
    }
    if (observed_shared_carveout_bytes && *observed_shared_carveout_bytes != 32768 &&
        *observed_shared_carveout_bytes != 65536 && *observed_shared_carveout_bytes != 102400)
        throw std::invalid_argument("calibrated profile requires observed carveout in bytes:32768/65536/102400");
    if (s.requires_observed_carveout) {
        if (!observed_shared_carveout_bytes ||
            *observed_shared_carveout_bytes != s.fixed_shared_carveout_bytes)
            throw std::invalid_argument("fixed Ada profile requires a matching observed shared carveout");
        return requested;
    }
    if (observed_shared_carveout_bytes == 65536) return AdaCacheProfile::R2_SHARED64;
    if (observed_shared_carveout_bytes == 102400) return AdaCacheProfile::R2_SHARED100;
    return requested;
}

struct AdaCalibratedProfile {
    static constexpr AdaCacheProfile default_profile = AdaCacheProfile::R2_ADAPTIVE;
    static constexpr std::array<std::uint32_t,3> shared_options = {32768,65536,102400};
    static AdaCacheProfileSettings settings(AdaCacheProfile p) { return ada_cache_profile_settings(p); }
    static const char* name(AdaCacheProfile p) { return ada_cache_profile_name(p); }
    static PerSmL1ReplacementPolicy policy(AdaCacheProfile p) { return settings(p).replacement; }

    // Occupancy is unchanged from v1 (including the grid cap). An observed
    // carveout that cannot hold ALL of those resident CTAs is rejected; do not
    // silently lower occupancy or infer a different hardware observation.
    static AdaKernelAllocation allocate(
            const AdaKernelResources& resources, AdaCacheProfile profile,
            std::optional<std::uint32_t> observed_shared_carveout_bytes = std::nullopt) {
        auto allocation = AdaTunerProfile::allocate(resources);
        const auto resolved = resolve_ada_cache_profile(profile,observed_shared_carveout_bytes);
        if (resolved == AdaCacheProfile::TUNER_V1) {
            if (observed_shared_carveout_bytes &&
                *observed_shared_carveout_bytes != allocation.shared_carveout_bytes)
                throw std::invalid_argument("observed carveout contradicts tuner-v1 allocation");
            return allocation;
        }
        const auto s = settings(resolved);
        const auto needed = std::uint64_t(allocation.resident_ctas_per_sm)*resources.shared_bytes_per_cta;
        std::uint32_t selected = 0;
        if (observed_shared_carveout_bytes) selected = *observed_shared_carveout_bytes;
        else {
            const auto it = std::lower_bound(shared_options.begin(),shared_options.end(),needed);
            if (it == shared_options.end()) throw std::invalid_argument("no supported calibrated shared carveout");
            selected = *it;
        }
        if (needed > selected) throw std::invalid_argument("observed shared carveout cannot hold resident CTA demand");
        allocation.shared_carveout_bytes = selected;
        allocation.l1_bytes = s.fixed_l1_bytes ? s.fixed_l1_bytes : AdaTunerProfile::unified_bytes-selected;
        allocation.l1_ways = allocation.l1_bytes/(s.l1_sets*AdaTunerProfile::line_bytes);
        return allocation;
    }

    static PerSmL1Config l1(const AdaKernelAllocation& allocation, AdaCacheProfile profile) {
        const auto s = settings(profile);
        if (!allocation.padded_threads || allocation.padded_threads > 1024 || allocation.padded_threads%32 ||
            allocation.rounded_registers%4 || allocation.rounded_registers>65536 ||
            !allocation.resident_ctas_per_sm || allocation.resident_ctas_per_sm>AdaTunerProfile::max_ctas_per_sm ||
            std::uint64_t(allocation.padded_threads)*allocation.resident_ctas_per_sm>AdaTunerProfile::max_threads_per_sm ||
            std::uint64_t(allocation.padded_threads)*allocation.rounded_registers*allocation.resident_ctas_per_sm>AdaTunerProfile::registers_per_sm)
            throw std::invalid_argument("invalid Ada resource allocation");
        if (profile == AdaCacheProfile::TUNER_V1) {
            if (std::find(AdaTunerProfile::shared_options.begin(),AdaTunerProfile::shared_options.end(),allocation.shared_carveout_bytes)
                == AdaTunerProfile::shared_options.end()) throw std::invalid_argument("invalid tuner-v1 allocation carveout");
        } else if (std::find(shared_options.begin(),shared_options.end(),allocation.shared_carveout_bytes)==shared_options.end())
            throw std::invalid_argument("invalid calibrated allocation carveout");
        if (s.requires_observed_carveout && allocation.shared_carveout_bytes!=s.fixed_shared_carveout_bytes)
            throw std::invalid_argument("stale allocation uses another profile's carveout");
        const auto expected_bytes = s.fixed_l1_bytes ? s.fixed_l1_bytes
            : AdaTunerProfile::unified_bytes-allocation.shared_carveout_bytes;
        if (allocation.l1_bytes!=expected_bytes || allocation.l1_ways!=expected_bytes/(s.l1_sets*AdaTunerProfile::line_bytes))
            throw std::invalid_argument("stale allocation uses another profile's L1 geometry");
        auto config = AdaTunerProfile::l1(allocation);
        config.replacement = s.replacement;
        return config;
    }
};
}
