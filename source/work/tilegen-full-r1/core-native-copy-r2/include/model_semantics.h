#ifndef MODEL_SEMANTICS_H
#define MODEL_SEMANTICS_H

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace GTSim {

// Exact rational bytes per core cycle. The numerator is expressed in bytes
// and the denominator in core cycles. Service uses integer fixed-point tokens;
// no floating-point conversion participates in admission or arbitration.
struct ExactByteRate {
    std::uint64_t numerator_bytes = 0;
    std::uint64_t denominator_cycles = 0;

    bool valid() const {
        return numerator_bytes > 0 && denominator_cycles > 0;
    }
};

enum class L2ServiceSemantics {
    LEGACY_FLOOR_LINES_PER_CYCLE,
    EXACT_RATIONAL_BYTES_PER_CORE_CYCLE,
};

enum class DramBandwidthSemantics {
    LEGACY_INTEGER_DRAM_CYCLE_WITH_FREQUENCY_RATIO,
    EXACT_RATIONAL_BYTES_PER_CORE_CYCLE,
};

enum class L2MissLatencySemantics {
    LEGACY_ADDITIVE_DRAM_THEN_L2_HIT,
    END_TO_END_FROM_MISS_DECISION,
};

enum class TensorIssueWorkSemantics {
    LEGACY_OUTPUT_ELEMENTS,
    DECLARED_FMA_WORK,
};

inline const char* l2_service_semantics_name(L2ServiceSemantics value) {
    switch (value) {
        case L2ServiceSemantics::LEGACY_FLOOR_LINES_PER_CYCLE:
            return "LEGACY_FLOOR_LINES_PER_CYCLE";
        case L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE:
            return "EXACT_RATIONAL_BYTES_PER_CORE_CYCLE";
    }
    return "UNKNOWN";
}

inline const char* dram_bandwidth_semantics_name(DramBandwidthSemantics value) {
    switch (value) {
        case DramBandwidthSemantics::LEGACY_INTEGER_DRAM_CYCLE_WITH_FREQUENCY_RATIO:
            return "LEGACY_INTEGER_DRAM_CYCLE_WITH_FREQUENCY_RATIO";
        case DramBandwidthSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE:
            return "EXACT_RATIONAL_BYTES_PER_CORE_CYCLE";
    }
    return "UNKNOWN";
}

inline const char* l2_miss_latency_semantics_name(L2MissLatencySemantics value) {
    switch (value) {
        case L2MissLatencySemantics::LEGACY_ADDITIVE_DRAM_THEN_L2_HIT:
            return "LEGACY_ADDITIVE_DRAM_THEN_L2_HIT";
        case L2MissLatencySemantics::END_TO_END_FROM_MISS_DECISION:
            return "END_TO_END_FROM_MISS_DECISION";
    }
    return "UNKNOWN";
}

inline const char* tensor_issue_work_semantics_name(
        TensorIssueWorkSemantics value) {
    switch (value) {
        case TensorIssueWorkSemantics::LEGACY_OUTPUT_ELEMENTS:
            return "LEGACY_OUTPUT_ELEMENTS";
        case TensorIssueWorkSemantics::DECLARED_FMA_WORK:
            return "DECLARED_FMA_WORK";
    }
    return "UNKNOWN";
}

struct MemoryModelSemantics {
    L2ServiceSemantics l2_service =
        L2ServiceSemantics::LEGACY_FLOOR_LINES_PER_CYCLE;
    DramBandwidthSemantics dram_bandwidth =
        DramBandwidthSemantics::LEGACY_INTEGER_DRAM_CYCLE_WITH_FREQUENCY_RATIO;
    L2MissLatencySemantics l2_miss_latency =
        L2MissLatencySemantics::LEGACY_ADDITIVE_DRAM_THEN_L2_HIT;
    ExactByteRate l2_total_rate{};
    ExactByteRate l2_write_rate{};
    ExactByteRate dram_rate{};
    int end_to_end_miss_latency_cycles = 0;

    void validate(int line_size_bytes) const {
        if (line_size_bytes <= 0) {
            throw std::invalid_argument("memory semantics require a positive line size");
        }
        if (l2_service ==
            L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE) {
            if (!l2_total_rate.valid() || !l2_write_rate.valid()) {
                throw std::invalid_argument(
                    "exact L2 semantics require total and write byte rates");
            }
            // A shared denominator makes the write <= total arbitration
            // relation exact without a lossy floating-point cross-product.
            if (l2_total_rate.denominator_cycles !=
                    l2_write_rate.denominator_cycles ||
                l2_write_rate.numerator_bytes >
                    l2_total_rate.numerator_bytes) {
                throw std::invalid_argument(
                    "exact L2 write rate must share the total-rate denominator and not exceed total service");
            }
        }
        if (dram_bandwidth ==
                DramBandwidthSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE &&
            !dram_rate.valid()) {
            throw std::invalid_argument(
                "exact DRAM semantics require a positive byte rate");
        }
        if (l2_miss_latency ==
                L2MissLatencySemantics::END_TO_END_FROM_MISS_DECISION &&
            end_to_end_miss_latency_cycles < 0) {
            throw std::invalid_argument(
                "end-to-end L2 miss latency cannot be negative");
        }
    }
};

// Fixed-point token bucket used only by explicit exact modes. Credit advances
// on active service cycles and is capped at one transfer quantum plus at most
// one cycle of carry, preventing idle/backpressure time from becoming an
// unbounded later burst.
class ExactRationalByteBudget {
public:
    ExactRationalByteBudget() = default;

    ExactRationalByteBudget(ExactByteRate rate, int quantum_bytes) {
        configure(rate, quantum_bytes);
    }

    void configure(ExactByteRate rate, int quantum_bytes) {
        if (!rate.valid() || quantum_bytes <= 0) {
            throw std::invalid_argument("invalid exact rational byte budget");
        }
        rate_ = rate;
        const auto quantum = static_cast<std::uint64_t>(quantum_bytes);
        if (quantum > std::numeric_limits<std::uint64_t>::max() /
                          rate_.denominator_cycles) {
            throw std::overflow_error("exact byte-budget quantum cost overflow");
        }
        quantum_cost_units_ = quantum * rate_.denominator_cycles;
        if (quantum_cost_units_ > std::numeric_limits<std::uint64_t>::max() -
                                      (rate_.numerator_bytes - 1)) {
            throw std::overflow_error("exact byte-budget token cap overflow");
        }
        token_cap_units_ = quantum_cost_units_ + rate_.numerator_bytes - 1;
        credit_units_ = 0;
        configured_ = true;
    }

    void reset() {
        credit_units_ = 0;
    }

    void accrue_active_cycle() {
        if (!configured_) {
            throw std::logic_error("exact byte budget is not configured");
        }
        if (credit_units_ >= token_cap_units_ ||
            rate_.numerator_bytes >= token_cap_units_ - credit_units_) {
            credit_units_ = token_cap_units_;
        } else {
            credit_units_ += rate_.numerator_bytes;
        }
    }

    bool can_consume_quantum() const {
        return configured_ && credit_units_ >= quantum_cost_units_;
    }

    void consume_quantum() {
        if (!can_consume_quantum()) {
            throw std::logic_error("exact byte budget quantum is unavailable");
        }
        credit_units_ -= quantum_cost_units_;
    }

    std::uint64_t credit_units() const {
        return credit_units_;
    }

private:
    ExactByteRate rate_{};
    std::uint64_t quantum_cost_units_ = 0;
    std::uint64_t token_cap_units_ = 0;
    std::uint64_t credit_units_ = 0;
    bool configured_ = false;
};

inline MemoryModelSemantics
make_rtx4000_ada_a3b4_memory_model_semantics() {
    MemoryModelSemantics result;
    result.l2_service =
        L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE;
    result.dram_bandwidth =
        DramBandwidthSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE;
    result.l2_miss_latency =
        L2MissLatencySemantics::END_TO_END_FROM_MISS_DECISION;
    result.l2_total_rate = {711ULL, 1ULL};
    result.l2_write_rate = {540ULL, 1ULL};
    result.dram_rate = {138978120924ULL, 1000000000ULL};
    result.end_to_end_miss_latency_cycles = 604;
    return result;
}

}  // namespace GTSim

#endif  // MODEL_SEMANTICS_H
