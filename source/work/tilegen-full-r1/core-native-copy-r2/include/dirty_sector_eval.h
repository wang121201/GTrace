#pragma once
// Experimental dirty-sector representation only. It does not add read-sector
// validity, change128B fill/RFO, L1 policy, issue bandwidth or replacement.
#include "dag_node.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <span>

#ifndef TILEGEN_DIRTY_SECTOR_MODE
#define TILEGEN_DIRTY_SECTOR_MODE 0
#endif

namespace GTSim {
inline constexpr int kTilegenDirtySectorMode = TILEGEN_DIRTY_SECTOR_MODE;
static_assert(kTilegenDirtySectorMode >= 0 && kTilegenDirtySectorMode <= 2,
              "mode0 baseline128, mode1 shadow mask/write128, mode2 sector writeback");
struct DirtySectorEvalStatistics {
    std::uint64_t store_mask_requests=0, range_visits=0, range_intersections=0;
    std::uint64_t unknown_store_rejections=0;
    std::uint64_t dirty_sector_creations=0, evicted_dirty_sectors=0;
    // These four gauges are populated only by dirty_sector_snapshot(), not
    // during modeled execution. They do not alter cache or backend state.
    std::uint64_t resident_dirty_lines=0, resident_dirty_sectors=0;
    std::uint64_t pending_dirty_lines=0, pending_dirty_sectors=0;
    std::uint64_t outstanding_writeback_bytes=0, unadmitted_writeback_bytes=0;
    bool dirty_sector_ledger_closed=false, writeback_byte_ledger_closed=false;
    std::array<std::uint64_t,5> store_mask_popcounts{}, eviction_popcounts{}, writeback_run_lengths{};
    std::array<std::uint64_t,16> eviction_masks{};
    std::array<std::uint64_t,5> eviction_run_counts{};
};
inline unsigned sector_popcount(std::uint8_t mask) {
    unsigned count=0;
    for (unsigned i=0;i<4;++i) count+=(mask>>i)&1U;
    return count;
}
inline std::uint8_t explicit_store_sector_mask_program(int source_matrix,
        int coalesce_bytes, std::span<const ExplicitMemorySubop> subops, int matrix,
        std::uint64_t line, int subop_index, DirtySectorEvalStatistics& stats) {
    const auto reject=[&](const char* why)->std::uint8_t {
        ++stats.unknown_store_rejections;
        throw std::invalid_argument(why);
    };
    if (matrix<0 || matrix!=source_matrix || line%128 ||
        coalesce_bytes!=128 || subops.empty())
        return reject("sector store requires explicit128B-coalesced byte-range program");
    if (subop_index<0) {
        if (subops.size()!=1)
            return reject("sector store with multiple subops requires exact subop index");
        subop_index=0;
    }
    if (static_cast<std::size_t>(subop_index)>=subops.size())
        return reject("sector store subop index out of bounds");
    const auto& subop=subops[subop_index];
    if (subop.ranges.empty()) return reject("sector store cannot infer empty or line-span-only coverage");
    const std::uint64_t last=line+127;
    std::uint64_t payload=0,visits=0,intersections=0;
    std::uint8_t mask=0;
    for (const auto& range:subop.ranges) {
        ++visits;
        if (!range.byte_count || range.offset_bytes>UINT64_MAX-(range.byte_count-1) ||
            payload>UINT64_MAX-range.byte_count)
            return reject("sector store zero/overflowing explicit range");
        payload+=range.byte_count;
        const auto end=range.offset_bytes+range.byte_count-1;
        if (range.offset_bytes>last || end<line) continue;
        ++intersections;
        const auto first=std::max(line,range.offset_bytes), stop=std::min(last,end);
        for (unsigned sector=unsigned((first-line)/32);sector<=unsigned((stop-line)/32);++sector)
            mask=std::uint8_t(mask|(1U<<sector));
    }
    if (payload!=subop.requested_bytes || !mask)
        return reject("sector store payload multiplicity or touched-line identity differs");
    ++stats.store_mask_requests;stats.range_visits+=visits;stats.range_intersections+=intersections;
    ++stats.store_mask_popcounts.at(sector_popcount(mask));
    return mask;
}
inline std::uint8_t explicit_store_sector_mask(const DAGNode& node, int matrix,
        std::uint64_t line, int subop_index, DirtySectorEvalStatistics& stats) {
    return explicit_store_sector_mask_program(node.matrix_id,node.memory_coalesce_bytes,
        node.explicit_memory_subops,matrix,line,subop_index,stats);
}
} // namespace GTSim
