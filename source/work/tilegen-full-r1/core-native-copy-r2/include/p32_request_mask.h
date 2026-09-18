#pragma once
// Read-only request provenance, not a sector cache or a fill/dirty mask.
// Include outside namespace GTSim. This header deliberately does not include
// memory.h, so memory.h can include it before declaring CacheLineKey/L2Cache.
#include "dag_node.h"
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace native_p32_observability {
struct RequestMask {
    bool known = false;
    std::uint8_t bits = 0;
};

// matrix_id/line_addr are the pre-transform native key. Each bit denotes an
// intersection of this ONE explicit subop with a 32B quarter of the 128B line.
// Duplicate/broadcast ranges are unioned here, but distinct requests are not.
// Unsupported representations return UNKNOWN; malformed supported inputs fail
// closed. No pointer, allocation, counter, or cache/scheduler state is retained.
inline RequestMask request_mask(const GTSim::DAGNode& node, int matrix_id,
                                std::uint64_t line_addr, int line_bytes) {
    if (line_bytes != 128 || node.memory_coalesce_bytes != 128 ||
        node.explicit_memory_subops.size() != 1) return {};
    if (matrix_id < 0 || matrix_id != node.matrix_id || line_addr % 128 != 0)
        throw std::invalid_argument("request mask invalid native line key");
    const auto& subop = node.explicit_memory_subops.front();
    if (subop.ranges.empty())
        throw std::invalid_argument("request mask empty explicit ranges");
    // An aligned 128B line has a representable inclusive last address, even
    // for the final uint64 line. Avoid an overflowing exclusive end at 2^64.
    const std::uint64_t line_last = line_addr + 127;
    std::uint64_t requested = 0;
    std::uint8_t bits = 0;
    for (const auto& range : subop.ranges) {
        if (range.byte_count == 0)
            throw std::invalid_argument("request mask zero explicit range");
        if (range.offset_bytes > std::numeric_limits<std::uint64_t>::max() -
                                     (range.byte_count - 1) ||
            requested > std::numeric_limits<std::uint64_t>::max() - range.byte_count)
            throw std::overflow_error("request mask explicit range overflow");
        requested += range.byte_count; // Preserve lane multiplicity separately.
        const std::uint64_t range_last = range.offset_bytes + range.byte_count - 1;
        if (range.offset_bytes > line_last || range_last < line_addr) continue;
        const std::uint64_t first = range.offset_bytes > line_addr ? range.offset_bytes : line_addr;
        const std::uint64_t last = range_last < line_last ? range_last : line_last;
        const unsigned lo = static_cast<unsigned>((first - line_addr) / 32);
        const unsigned hi = static_cast<unsigned>((last - line_addr) / 32);
        for (unsigned sector = lo; sector <= hi; ++sector)
            bits = static_cast<std::uint8_t>(bits | (1U << sector));
    }
    if (requested != subop.requested_bytes)
        throw std::invalid_argument("request mask requested byte multiplicity mismatch");
    if (bits == 0)
        throw std::invalid_argument("request mask key not touched by explicit subop");
    return {true, bits};
}
} // namespace native_p32_observability
