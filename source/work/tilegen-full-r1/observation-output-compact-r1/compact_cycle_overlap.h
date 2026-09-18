#pragma once
// Output transformation only. No scheduler, memory, clock or observer mutation.
#include <nlohmann/json.hpp>
#include "../driver-pooled-hash-bulk-r1/sha256.h"
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace tilegen_observation_output {
using J = nlohmann::json;
using U = std::uint64_t;
inline void require(bool ok, const char* why) {
    if (!ok) throw std::logic_error(why);
}
inline U natural(const J& j) {
    require(j.is_number_integer(), "overlap integer required");
    require(j.is_number_unsigned() || j.get<std::int64_t>() >= 0,
            "overlap negative count");
    return j.get<U>();
}
inline U add(U a, U b) {
    require(b <= std::numeric_limits<U>::max() - a, "overlap sum overflow");
    return a + b;
}
inline J compact_cycle_overlap(const J& full) {
    require(full.is_object() && full.at("schema") == "GTSIM_CYCLE_OVERLAP_V1",
            "supported overlap schema required");
    require(full.at("enabled").is_boolean(), "overlap enabled boolean required");
    if (!full.at("enabled").get<bool>()) return full;
    require(!full.contains("per_unit_array_summary"), "already compacted overlap");
    const U window = natural(full.at("window_cycles"));
    const std::array<const char*,4> names = {
        "neither_cycles", "compute_only_cycles", "memory_only_cycles", "both_cycles"};
    J output = full, summary = J::object();
    const auto sm_count = full.at("per_sm").size();
    const auto sp_count = full.at("per_sp").size();
    require(sm_count > 0 && sm_count <= 1024 && sp_count >= sm_count &&
            sp_count % sm_count == 0 && sp_count / sm_count <= 32,
            "bounded overlap geometry required");
    const auto sps = sp_count / sm_count;
    for (bool sp : {false, true}) {
        const char* field = sp ? "per_sp" : "per_sm";
        const auto& rows = full.at(field);
        require(rows.is_array(), "ordered overlap array required");
        const auto& pooled = full.at(sp ? "pooled_sp_time" : "pooled_sm_time");
        std::array<U,4> sums{};
        tiny_sha::Sha256 digest;
        digest.add("[");
        U serialized_bytes = 2;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows.at(i);
            require(natural(row.at("sm_id")) == (sp ? i / sps : i),
                    "original ordered SM identity required");
            if (sp) require(natural(row.at("sp_id")) == i % sps,
                            "original ordered SP identity required");
            require(row.at("closed") == true && natural(row.at("window_cycles")) == window,
                    "unit overlap window closure required");
            U total = 0;
            for (std::size_t k = 0; k < names.size(); ++k) {
                const U value = natural(row.at(names[k]));
                sums[k] = add(sums[k], value); total = add(total, value);
            }
            require(total == window, "unit overlap bins must conserve window");
            require(natural(row.at("compute_inflight_union_cycles")) ==
                    add(natural(row.at(names[1])),natural(row.at(names[3]))) &&
                    natural(row.at("memory_outstanding_union_cycles")) ==
                    add(natural(row.at(names[2])),natural(row.at(names[3]))),
                    "unit overlap unions must conserve bins");
            require(natural(row.at("final_compute_inflight_subops")) == 0 &&
                    natural(row.at("final_memory_outstanding_nodes")) == 0,
                    "unit overlap must be drained");
            if (i) { digest.add(","); serialized_bytes = add(serialized_bytes,1); }
            const auto encoded = row.dump();
            digest.add(encoded); serialized_bytes = add(serialized_bytes,encoded.size());
        }
        digest.add("]");
        U total = 0;
        for (std::size_t k = 0; k < names.size(); ++k) {
            require(sums[k] == natural(pooled.at(names[k])), "pooled overlap bins differ");
            total = add(total,sums[k]);
        }
        require(pooled.at("closed") == true &&
                total == natural(pooled.at("population_cycles")),
                "pooled overlap population closure required");
        summary[field] = {{"count", rows.size()}, {"ordered_array_sha256",digest.hex()},
                          {"canonical_serialized_bytes",serialized_bytes}};
        output.erase(field);
    }
    summary["schema"] = "OVERLAP_PER_UNIT_ARRAY_DIGEST_V1";
    summary["encoding"] = "UTF-8 compact JSON; sorted object keys; original array order; SHA-256 includes brackets and commas";
    summary["scope"] = "Only per_sm/per_sp arrays omitted. All other fields, including gpu, pooled totals, coverage and limitations, unchanged.";
    summary["simulation_reexecuted"] = false;
    summary["model_or_counter_changed"] = false;
    output["per_unit_array_summary"] = std::move(summary);
    return output;
}
} // namespace tilegen_observation_output
