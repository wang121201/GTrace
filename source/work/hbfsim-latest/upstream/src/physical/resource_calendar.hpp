#pragma once
#include "physical/physical_types.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace hbfsim::physical {
inline double causal_finish(double start, double duration) {
    if (!std::isfinite(start) || !std::isfinite(duration) || start < 0 || duration < 0)
        throw std::runtime_error("invalid resource reservation");
    const double sum = start + duration;
    const double end = duration == 0 || sum > start ? sum : std::nextafter(start, std::numeric_limits<double>::infinity());
    if (!std::isfinite(end)) throw std::overflow_error("resource time overflow");
    return end;
}


// Values share one caller-selected time unit: nanoseconds for HBF, exact
// integer command-clock cycles for HBM. The calendar never converts units.
struct ResourceTimeline {
    struct Gap {
        double begin_ns = 0.0;
        double end_ns = 0.0;
        bool operator==(const Gap&) const = default;
    };

    static constexpr std::size_t kNoNode =
        std::numeric_limits<std::size_t>::max();

    struct GapNode {
        Gap gap;
        std::uint64_t priority = 0;
        double max_duration_ns = 0.0;
        std::size_t left = kNoNode;
        std::size_t right = kNoNode;
        std::uint64_t gap_fingerprint = 0;
    };

    ResourceTimeline() = default;
    ResourceTimeline(ResourceTimeline&&) noexcept = default;
    ResourceTimeline& operator=(ResourceTimeline&&) noexcept = default;
    ResourceTimeline(const ResourceTimeline&) = default;
    ResourceTimeline& operator=(const ResourceTimeline&) = default;

    double ready_ns = 0.0;
    // Sum of all non-overlapping reservations on this one resource.
    double reserved_work_ns = 0.0;

    [[nodiscard]] std::optional<Gap> first_fitting_gap(
        double earliest_ns,
        double duration_ns) const;
    [[nodiscard]] bool can_reserve_exact(
        double begin_ns,
        double duration_ns) const;
    [[nodiscard]] double preview_start(
        double earliest_ns,
        double duration_ns) const;
    void consume_gap(const Gap& gap, double begin_ns, double end_ns);
    void insert_gap(double begin_ns, double end_ns);
    // Records [ready_ns, start_ns) as idle. Every retained gap ends at or
    // before ready_ns, so no overlap search is needed; the tree shape and
    // priorities evolve exactly as with insert_gap.
    void insert_frontier_gap(double start_ns);
    void prune_before(double causal_watermark_ns);
    // Used by HBM's exact symmetric-channel replication. Expired reservations
    // and arena allocation history do not distinguish future schedules.
    [[nodiscard]] std::vector<Gap> gaps_after(double floor_ns) const;
    // Tree-shape-independent prefilter; equality still requires comparing gaps.
    [[nodiscard]] std::uint64_t gap_fingerprint_after(double floor_ns) const;
    [[nodiscard]] double reserve(double earliest_ns, double duration_ns);
    // Order-independent per-gap hash shared with GapCalendar.
    [[nodiscard]] static std::uint64_t fingerprint_gap(const Gap& gap);

private:
    // Timelines are hot and short-lived gaps churn heavily. Store treap
    // nodes in a compact, reusable arena instead of allocating and
    // freeing two unique_ptr subtrees for every split reservation.
    std::vector<GapNode> gap_nodes_;
    std::vector<std::size_t> free_gap_nodes_;
    std::size_t gap_root_ = kNoNode;
    std::uint64_t priority_state_ = 0x9e3779b97f4a7c15ULL;
    double pruned_through_ns_ = 0.0;
    // Upper bound on every retained gap's end. While it does not exceed
    // ready_ns, a gap appended at the frontier cannot overlap anything.
    double max_end_ns_ = 0.0;
    struct GapQuery {
        bool valid = false;
        double earliest_ns = 0.0;
        double duration_ns = 0.0;
        double frontier_ns = 0.0;
        std::optional<Gap> result;
    };
    mutable GapQuery last_gap_query_;

    [[nodiscard]] std::uint64_t fingerprint_after(std::size_t root, double floor_ns) const;

    [[nodiscard]] std::uint64_t next_priority();
    // Insertion without the neighbour overlap walks, for gaps that are
    // sub-intervals of a gap just removed or lie at the frontier.
    void insert_gap_unchecked(double begin_ns, double end_ns);
    [[nodiscard]] std::size_t allocate_node(Gap gap);
    void recycle_node(std::size_t node);
    void recycle_subtree(std::size_t root);
    void refresh(std::size_t node);
    void split(
        std::size_t root,
        double key,
        std::size_t& lower,
        std::size_t& upper);
    [[nodiscard]] std::size_t insert_node(
        std::size_t root,
        std::size_t node);
    [[nodiscard]] std::size_t erase_node(
        std::size_t root,
        double key,
        std::size_t& erased);
    [[nodiscard]] std::size_t merge(
        std::size_t lower,
        std::size_t upper);
    [[nodiscard]] const GapNode* predecessor(
        std::size_t root,
        double key) const;
    [[nodiscard]] const GapNode* successor(
        std::size_t root,
        double key) const;
    [[nodiscard]] const GapNode* first_fitting_from(
        std::size_t root,
        double minimum_begin_ns,
        double duration_ns) const;
};

}
