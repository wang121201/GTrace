#pragma once
#include "physical/resource_calendar.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hbfsim::physical {

// Idle-gap calendar for one resource whose times are exact integers (HBM
// command-clock cycles). It keeps the same idle-gap set and answers the same
// queries as ResourceTimeline, so both calendars produce identical schedules;
// only the storage differs. Gaps live in 64-entry leaves, leaves in 64-entry
// groups, and groups in one ordered vector; every level carries a summary
// row (first key, longest gap, fingerprint) so a reservation touches a few
// contiguous cache lines instead of walking an augmented treap whose nodes
// are scattered across a large arena.
//
// The first-fit answer is the earliest gap (by begin) satisfying
// causal_finish(begin, duration) <= end. ResourceTimeline skips treap
// subtrees by (end - begin) < duration before applying that test; for
// fractional nanosecond times the two can disagree by one rounding step and
// its answer then depends on the tree shape. For integer cycles the tests
// agree, which is why only integer-cycle calendars use this class today;
// nanosecond calendars keep the treap so their results stay bit-identical.
class GapCalendar {
public:
    using Gap = ResourceTimeline::Gap;

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
    // Records [ready_ns, start_ns) as idle, like ResourceTimeline::insert_frontier_gap.
    void insert_frontier_gap(double start_ns);
    void prune_before(double causal_watermark_ns);
    [[nodiscard]] std::vector<Gap> gaps_after(double floor_ns) const;
    // gaps_after(floor) == other.gaps_after(floor) without building either list.
    [[nodiscard]] bool same_gaps_after(const GapCalendar& other, double floor_ns) const;
    // Order-independent XOR of ResourceTimeline::fingerprint_gap over
    // gaps_after(floor); equal to ResourceTimeline::gap_fingerprint_after.
    [[nodiscard]] std::uint64_t gap_fingerprint_after(double floor_ns) const;
    [[nodiscard]] double reserve(double earliest_ns, double duration_ns);
    [[nodiscard]] std::size_t gap_count() const;

private:
    static constexpr std::uint32_t kLeafGaps = 64;
    static constexpr std::uint32_t kGroupLeaves = 64;
    struct Leaf {
        std::uint32_t count = 0;
        Gap gaps[kLeafGaps];
    };
    // Leaves in key order with one summary row per leaf.
    struct Group {
        std::uint32_t count = 0;
        std::uint32_t leaves[kGroupLeaves];
        double first_keys[kGroupLeaves];
        double max_durations[kGroupLeaves];
        double max_ends[kGroupLeaves];
        std::uint64_t fingerprints[kGroupLeaves];
    };
    // (index into order_, leaf slot in that group, gap slot in that leaf);
    // group == order_.size() is the end position.
    struct Position {
        std::size_t group = 0;
        std::uint32_t leaf = 0;
        std::uint32_t slot = 0;
    };
    struct Found {
        Gap gap;
        Position at;
    };
    struct GapQuery {
        bool valid = false;
        double earliest_ns = 0.0;
        double duration_ns = 0.0;
        double frontier_ns = 0.0;
        std::optional<Gap> result;
    };

    std::vector<Leaf> leaves_;
    std::vector<std::uint32_t> free_leaves_;
    std::vector<Group> groups_;
    std::vector<std::uint32_t> free_groups_;
    // Live groups are order_[head_..); the summaries are parallel to order_.
    std::vector<std::uint32_t> order_;
    std::vector<double> first_keys_;
    std::vector<double> max_durations_;
    std::vector<double> max_ends_;
    std::vector<std::uint64_t> fingerprints_;
    std::size_t head_ = 0;
    double pruned_through_ns_ = 0.0;
    mutable GapQuery last_gap_query_;

    [[nodiscard]] bool at_end(Position p) const { return p.group == order_.size(); }
    [[nodiscard]] Group& group_at(std::size_t position) { return groups_[order_[position]]; }
    [[nodiscard]] const Group& group_at(std::size_t position) const {
        return groups_[order_[position]];
    }
    [[nodiscard]] Leaf& leaf_at(Position p) { return leaves_[group_at(p.group).leaves[p.leaf]]; }
    [[nodiscard]] const Leaf& leaf_at(Position p) const {
        return leaves_[group_at(p.group).leaves[p.leaf]];
    }
    [[nodiscard]] const Gap& gap_at(Position p) const { return leaf_at(p).gaps[p.slot]; }
    [[nodiscard]] Position lower_bound(double key) const;  // first begin >= key
    [[nodiscard]] Position upper_bound(double key) const;  // first begin > key
    [[nodiscard]] Position first_after_floor(double floor_ns) const;  // first end > floor
    [[nodiscard]] bool previous(Position& p) const;
    [[nodiscard]] Position next(Position p) const;
    [[nodiscard]] static bool cannot_hold(double max_duration_ns, double max_end_ns, double duration_ns);
    [[nodiscard]] std::uint32_t allocate_leaf();
    [[nodiscard]] std::uint32_t allocate_group();
    void refresh_leaf_row(Group& group, std::uint32_t leaf);
    void refresh_group_summary(std::size_t position);
    void note_inserted(Position p, const Gap& gap);
    void note_replaced(Position p, const Gap& old, const Gap& gap);
    void note_erased(Position p, const Gap& old);
    void split_leaf(Position& p);
    void split_group(Position& p);
    void remove_leaf(Position p);
    void insert_at(Position p, const Gap& gap);
    void erase_at(Position p);
    void replace_at(Position p, const Gap& gap);
    void drop_before(Position p);
    void compact();
    [[nodiscard]] std::optional<Found> search(double earliest_ns, double duration_ns) const;
    void consume_at(Position held, const Gap& gap, double begin_ns, double end_ns);
};

}
