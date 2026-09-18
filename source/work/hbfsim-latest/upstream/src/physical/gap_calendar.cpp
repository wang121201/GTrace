#include "physical/gap_calendar.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace hbfsim::physical {
namespace {
double duration_of(const GapCalendar::Gap& gap) {
    return gap.end_ns - gap.begin_ns;
}
bool reservation_outside(const GapCalendar::Gap& gap, double begin_ns, double end_ns) {
    return begin_ns < gap.begin_ns || end_ns > gap.end_ns || end_ns <= begin_ns;
}
template <class T>
void shift_up(T* values, std::uint32_t at, std::uint32_t count) {
    std::move_backward(values + at, values + count, values + count + 1);
}
template <class T>
void shift_down(T* values, std::uint32_t at, std::uint32_t count, std::uint32_t by) {
    std::move(values + at, values + count, values + at - by);
}
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

GapCalendar::Position GapCalendar::upper_bound(double key) const {
    const auto first = first_keys_.begin() + static_cast<std::ptrdiff_t>(head_);
    const auto group_after = std::upper_bound(first, first_keys_.end(), key);
    if (group_after == first) {
        return Position{head_, 0, 0};
    }
    const auto g = head_ + static_cast<std::size_t>(group_after - first) - 1;
    const Group& group = group_at(g);
    // group.first_keys[0] == first_keys_[g] <= key, so at least one leaf qualifies.
    const auto l = static_cast<std::uint32_t>(
        std::upper_bound(group.first_keys, group.first_keys + group.count, key) - group.first_keys) - 1;
    const Leaf& leaf = leaves_[group.leaves[l]];
    const Gap* const begin = leaf.gaps;
    const Gap* const end = leaf.gaps + leaf.count;
    const Gap* slot = std::upper_bound(
        begin, end, key, [](double value, const Gap& gap) { return value < gap.begin_ns; });
    if (slot != end) {
        return Position{g, l, static_cast<std::uint32_t>(slot - begin)};
    }
    if (l + 1 < group.count) {
        return Position{g, l + 1, 0};
    }
    return Position{g + 1, 0, 0};
}

GapCalendar::Position GapCalendar::lower_bound(double key) const {
    const auto first = first_keys_.begin() + static_cast<std::ptrdiff_t>(head_);
    const auto group_after = std::upper_bound(first, first_keys_.end(), key);
    if (group_after == first) {
        return Position{head_, 0, 0};
    }
    const auto g = head_ + static_cast<std::size_t>(group_after - first) - 1;
    const Group& group = group_at(g);
    const auto l = static_cast<std::uint32_t>(
        std::upper_bound(group.first_keys, group.first_keys + group.count, key) - group.first_keys) - 1;
    const Leaf& leaf = leaves_[group.leaves[l]];
    const Gap* const begin = leaf.gaps;
    const Gap* const end = leaf.gaps + leaf.count;
    const Gap* slot = std::lower_bound(
        begin, end, key, [](const Gap& gap, double value) { return gap.begin_ns < value; });
    if (slot != end) {
        return Position{g, l, static_cast<std::uint32_t>(slot - begin)};
    }
    if (l + 1 < group.count) {
        return Position{g, l + 1, 0};
    }
    return Position{g + 1, 0, 0};
}

GapCalendar::Position GapCalendar::first_after_floor(double floor_ns) const {
    // Gaps are disjoint and ordered, so the only gap that can end after the
    // floor while beginning at or before it is the last one beginning there.
    const Position after = upper_bound(floor_ns);
    Position last = after;
    if (previous(last) && gap_at(last).end_ns > floor_ns) {
        return last;
    }
    return after;
}

bool GapCalendar::previous(Position& p) const {
    if (p.slot > 0) {
        --p.slot;
        return true;
    }
    if (p.leaf > 0) {
        --p.leaf;
        p.slot = leaf_at(p).count - 1;
        return true;
    }
    if (p.group > head_) {
        --p.group;
        p.leaf = group_at(p.group).count - 1;
        p.slot = leaf_at(p).count - 1;
        return true;
    }
    return false;
}

GapCalendar::Position GapCalendar::next(Position p) const {
    const Group& group = group_at(p.group);
    if (p.slot + 1 < leaves_[group.leaves[p.leaf]].count) {
        return Position{p.group, p.leaf, p.slot + 1};
    }
    if (p.leaf + 1 < group.count) {
        return Position{p.group, p.leaf + 1, 0};
    }
    return Position{p.group + 1, 0, 0};
}

// ---------------------------------------------------------------------------
// Storage and summaries
// ---------------------------------------------------------------------------

std::uint32_t GapCalendar::allocate_leaf() {
    if (free_leaves_.empty()) {
        leaves_.emplace_back();
        return static_cast<std::uint32_t>(leaves_.size() - 1);
    }
    const auto index = free_leaves_.back();
    free_leaves_.pop_back();
    leaves_[index].count = 0;
    return index;
}

std::uint32_t GapCalendar::allocate_group() {
    if (free_groups_.empty()) {
        groups_.emplace_back();
        return static_cast<std::uint32_t>(groups_.size() - 1);
    }
    const auto index = free_groups_.back();
    free_groups_.pop_back();
    groups_[index].count = 0;
    return index;
}

bool GapCalendar::cannot_hold(double max_duration_ns, double max_end_ns, double duration_ns) {
    // A gap can satisfy causal_finish(begin, duration) <= end while end - begin
    // falls short of the duration by less than one rounding step of end. Two
    // ulps of the largest end in the range cover that and the subtraction's
    // own rounding, so no admissible gap is ever skipped. For integer cycles
    // the slack is below one and never changes the outcome.
    const double slack = 2.0 *
        (std::nextafter(max_end_ns, std::numeric_limits<double>::infinity()) - max_end_ns);
    return max_duration_ns + slack < duration_ns;
}

void GapCalendar::refresh_leaf_row(Group& group, std::uint32_t slot) {
    const Leaf& leaf = leaves_[group.leaves[slot]];
    double longest = 0.0;
    std::uint64_t fingerprint = 0;
    for (std::uint32_t i = 0; i < leaf.count; ++i) {
        longest = std::max(longest, duration_of(leaf.gaps[i]));
        fingerprint ^= ResourceTimeline::fingerprint_gap(leaf.gaps[i]);
    }
    group.first_keys[slot] = leaf.gaps[0].begin_ns;
    group.max_durations[slot] = longest;
    group.max_ends[slot] = leaf.gaps[leaf.count - 1].end_ns;
    group.fingerprints[slot] = fingerprint;
}

void GapCalendar::refresh_group_summary(std::size_t position) {
    const Group& group = group_at(position);
    double longest = 0.0;
    std::uint64_t fingerprint = 0;
    for (std::uint32_t l = 0; l < group.count; ++l) {
        longest = std::max(longest, group.max_durations[l]);
        fingerprint ^= group.fingerprints[l];
    }
    first_keys_[position] = group.first_keys[0];
    max_durations_[position] = longest;
    max_ends_[position] = group.max_ends[group.count - 1];
    fingerprints_[position] = fingerprint;
}

void GapCalendar::note_inserted(Position p, const Gap& gap) {
    Group& group = group_at(p.group);
    const Leaf& leaf = leaves_[group.leaves[p.leaf]];
    const double duration = duration_of(gap);
    const auto fingerprint = ResourceTimeline::fingerprint_gap(gap);
    group.first_keys[p.leaf] = leaf.gaps[0].begin_ns;
    group.max_durations[p.leaf] = std::max(group.max_durations[p.leaf], duration);
    group.max_ends[p.leaf] = leaf.gaps[leaf.count - 1].end_ns;
    group.fingerprints[p.leaf] ^= fingerprint;
    first_keys_[p.group] = group.first_keys[0];
    max_durations_[p.group] = std::max(max_durations_[p.group], duration);
    max_ends_[p.group] = group.max_ends[group.count - 1];
    fingerprints_[p.group] ^= fingerprint;
}

void GapCalendar::note_replaced(Position p, const Gap& old, const Gap& gap) {
    Group& group = group_at(p.group);
    const Leaf& leaf = leaves_[group.leaves[p.leaf]];
    const auto fingerprint =
        ResourceTimeline::fingerprint_gap(old) ^ ResourceTimeline::fingerprint_gap(gap);
    group.fingerprints[p.leaf] ^= fingerprint;
    fingerprints_[p.group] ^= fingerprint;
    if (p.slot == 0) {
        group.first_keys[p.leaf] = gap.begin_ns;
        if (p.leaf == 0) {
            first_keys_[p.group] = gap.begin_ns;
        }
    }
    group.max_ends[p.leaf] = leaf.gaps[leaf.count - 1].end_ns;
    max_ends_[p.group] = group.max_ends[group.count - 1];
    const double old_leaf_max = group.max_durations[p.leaf];
    double leaf_max = old_leaf_max;
    if (duration_of(old) == old_leaf_max) {
        leaf_max = 0.0;
        for (std::uint32_t i = 0; i < leaf.count; ++i) {
            leaf_max = std::max(leaf_max, duration_of(leaf.gaps[i]));
        }
    } else {
        leaf_max = std::max(old_leaf_max, duration_of(gap));
    }
    group.max_durations[p.leaf] = leaf_max;
    if (leaf_max > old_leaf_max) {
        max_durations_[p.group] = std::max(max_durations_[p.group], leaf_max);
    } else if (leaf_max < old_leaf_max && old_leaf_max == max_durations_[p.group]) {
        double longest = 0.0;
        for (std::uint32_t l = 0; l < group.count; ++l) {
            longest = std::max(longest, group.max_durations[l]);
        }
        max_durations_[p.group] = longest;
    }
}

void GapCalendar::note_erased(Position p, const Gap& old) {
    Group& group = group_at(p.group);
    const Leaf& leaf = leaves_[group.leaves[p.leaf]];
    const auto fingerprint = ResourceTimeline::fingerprint_gap(old);
    group.fingerprints[p.leaf] ^= fingerprint;
    fingerprints_[p.group] ^= fingerprint;
    if (p.slot == 0) {
        group.first_keys[p.leaf] = leaf.gaps[0].begin_ns;
        if (p.leaf == 0) {
            first_keys_[p.group] = leaf.gaps[0].begin_ns;
        }
    }
    group.max_ends[p.leaf] = leaf.gaps[leaf.count - 1].end_ns;
    max_ends_[p.group] = group.max_ends[group.count - 1];
    const double old_leaf_max = group.max_durations[p.leaf];
    if (duration_of(old) != old_leaf_max) {
        return;
    }
    double leaf_max = 0.0;
    for (std::uint32_t i = 0; i < leaf.count; ++i) {
        leaf_max = std::max(leaf_max, duration_of(leaf.gaps[i]));
    }
    group.max_durations[p.leaf] = leaf_max;
    if (leaf_max < old_leaf_max && old_leaf_max == max_durations_[p.group]) {
        double longest = 0.0;
        for (std::uint32_t l = 0; l < group.count; ++l) {
            longest = std::max(longest, group.max_durations[l]);
        }
        max_durations_[p.group] = longest;
    }
}

void GapCalendar::compact() {
    if (head_ == 0) {
        return;
    }
    const auto dead = static_cast<std::ptrdiff_t>(head_);
    order_.erase(order_.begin(), order_.begin() + dead);
    first_keys_.erase(first_keys_.begin(), first_keys_.begin() + dead);
    max_durations_.erase(max_durations_.begin(), max_durations_.begin() + dead);
    max_ends_.erase(max_ends_.begin(), max_ends_.begin() + dead);
    fingerprints_.erase(fingerprints_.begin(), fingerprints_.begin() + dead);
    head_ = 0;
}

void GapCalendar::split_group(Position& p) {
    constexpr std::uint32_t half = kGroupLeaves / 2;
    const auto index = allocate_group();
    Group& lower = group_at(p.group);
    Group& upper = groups_[index];
    std::copy(lower.leaves + half, lower.leaves + kGroupLeaves, upper.leaves);
    std::copy(lower.first_keys + half, lower.first_keys + kGroupLeaves, upper.first_keys);
    std::copy(lower.max_durations + half, lower.max_durations + kGroupLeaves, upper.max_durations);
    std::copy(lower.max_ends + half, lower.max_ends + kGroupLeaves, upper.max_ends);
    std::copy(lower.fingerprints + half, lower.fingerprints + kGroupLeaves, upper.fingerprints);
    lower.count = half;
    upper.count = half;
    const auto at = static_cast<std::ptrdiff_t>(p.group) + 1;
    order_.insert(order_.begin() + at, index);
    first_keys_.insert(first_keys_.begin() + at, 0.0);
    max_durations_.insert(max_durations_.begin() + at, 0.0);
    max_ends_.insert(max_ends_.begin() + at, 0.0);
    fingerprints_.insert(fingerprints_.begin() + at, 0);
    refresh_group_summary(p.group);
    refresh_group_summary(p.group + 1);
    if (p.leaf >= half) {
        p.group += 1;
        p.leaf -= half;
    }
}

void GapCalendar::split_leaf(Position& p) {
    if (group_at(p.group).count == kGroupLeaves) {
        split_group(p);
    }
    constexpr std::uint32_t half = kLeafGaps / 2;
    const auto index = allocate_leaf();
    Group& group = group_at(p.group);
    Leaf& lower = leaves_[group.leaves[p.leaf]];
    Leaf& upper = leaves_[index];
    std::copy(lower.gaps + half, lower.gaps + kLeafGaps, upper.gaps);
    lower.count = half;
    upper.count = half;
    const std::uint32_t at = p.leaf + 1;
    shift_up(group.leaves, at, group.count);
    shift_up(group.first_keys, at, group.count);
    shift_up(group.max_durations, at, group.count);
    shift_up(group.max_ends, at, group.count);
    shift_up(group.fingerprints, at, group.count);
    group.leaves[at] = index;
    ++group.count;
    // The group holds the same gaps as before; only its two rows change.
    refresh_leaf_row(group, p.leaf);
    refresh_leaf_row(group, at);
    if (p.slot >= half) {
        p.leaf = at;
        p.slot -= half;
    }
}

void GapCalendar::insert_at(Position p, const Gap& gap) {
    if (head_ == order_.size()) {
        compact();
        const auto group_index = allocate_group();
        const auto leaf_index = allocate_leaf();
        Leaf& leaf = leaves_[leaf_index];
        leaf.gaps[0] = gap;
        leaf.count = 1;
        Group& group = groups_[group_index];
        group.leaves[0] = leaf_index;
        group.count = 1;
        refresh_leaf_row(group, 0);
        order_.push_back(group_index);
        first_keys_.push_back(gap.begin_ns);
        max_durations_.push_back(duration_of(gap));
        max_ends_.push_back(gap.end_ns);
        fingerprints_.push_back(ResourceTimeline::fingerprint_gap(gap));
        return;
    }
    if (at_end(p)) {
        p.group = order_.size() - 1;
        p.leaf = group_at(p.group).count - 1;
        p.slot = leaf_at(p).count;
    }
    if (leaf_at(p).count == kLeafGaps) {
        split_leaf(p);
    }
    Leaf& leaf = leaf_at(p);
    shift_up(leaf.gaps, p.slot, leaf.count);
    leaf.gaps[p.slot] = gap;
    ++leaf.count;
    note_inserted(p, gap);
}

void GapCalendar::remove_leaf(Position p) {
    Group& group = group_at(p.group);
    free_leaves_.push_back(group.leaves[p.leaf]);
    if (group.count == 1) {
        free_groups_.push_back(order_[p.group]);
        const auto at = static_cast<std::ptrdiff_t>(p.group);
        order_.erase(order_.begin() + at);
        first_keys_.erase(first_keys_.begin() + at);
        max_durations_.erase(max_durations_.begin() + at);
        max_ends_.erase(max_ends_.begin() + at);
        fingerprints_.erase(fingerprints_.begin() + at);
        if (head_ == order_.size()) {
            compact();
        }
        return;
    }
    shift_down(group.leaves, p.leaf + 1, group.count, 1);
    shift_down(group.first_keys, p.leaf + 1, group.count, 1);
    shift_down(group.max_durations, p.leaf + 1, group.count, 1);
    shift_down(group.max_ends, p.leaf + 1, group.count, 1);
    shift_down(group.fingerprints, p.leaf + 1, group.count, 1);
    --group.count;
    refresh_group_summary(p.group);
}

void GapCalendar::erase_at(Position p) {
    Leaf& leaf = leaf_at(p);
    const Gap old = leaf.gaps[p.slot];
    shift_down(leaf.gaps, p.slot + 1, leaf.count, 1);
    --leaf.count;
    if (leaf.count == 0) {
        remove_leaf(p);
        return;
    }
    note_erased(p, old);
}

void GapCalendar::replace_at(Position p, const Gap& gap) {
    Leaf& leaf = leaf_at(p);
    const Gap old = leaf.gaps[p.slot];
    leaf.gaps[p.slot] = gap;
    note_replaced(p, old, gap);
}

void GapCalendar::drop_before(Position p) {
    for (std::size_t position = head_; position < p.group; ++position) {
        const Group& group = group_at(position);
        for (std::uint32_t l = 0; l < group.count; ++l) {
            free_leaves_.push_back(group.leaves[l]);
        }
        free_groups_.push_back(order_[position]);
    }
    head_ = p.group;
    if (at_end(p)) {
        compact();
        return;
    }
    Group& group = group_at(p.group);
    if (p.leaf > 0) {
        for (std::uint32_t l = 0; l < p.leaf; ++l) {
            free_leaves_.push_back(group.leaves[l]);
        }
        shift_down(group.leaves, p.leaf, group.count, p.leaf);
        shift_down(group.first_keys, p.leaf, group.count, p.leaf);
        shift_down(group.max_durations, p.leaf, group.count, p.leaf);
        shift_down(group.max_ends, p.leaf, group.count, p.leaf);
        shift_down(group.fingerprints, p.leaf, group.count, p.leaf);
        group.count -= p.leaf;
    }
    if (p.slot > 0) {
        Leaf& leaf = leaves_[group.leaves[0]];
        shift_down(leaf.gaps, p.slot, leaf.count, p.slot);
        leaf.count -= p.slot;
        refresh_leaf_row(group, 0);
    }
    refresh_group_summary(p.group);
    if (head_ > order_.size() / 2) {
        compact();
    }
}

std::size_t GapCalendar::gap_count() const {
    std::size_t count = 0;
    for (std::size_t position = head_; position < order_.size(); ++position) {
        const Group& group = group_at(position);
        for (std::uint32_t l = 0; l < group.count; ++l) {
            count += leaves_[group.leaves[l]].count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<GapCalendar::Found> GapCalendar::search(double earliest_ns, double duration_ns) const {
    const Position after = upper_bound(earliest_ns);
    Position containing = after;
    if (previous(containing)) {
        const Gap& gap = gap_at(containing);
        if (causal_finish(earliest_ns, duration_ns) <= gap.end_ns) {
            return Found{gap, containing};
        }
    }
    if (at_end(after)) {
        return std::nullopt;
    }
    // A gap beginning exactly at earliest_ns was just rejected by the same
    // test, so the scan starts at the first gap beginning after it. Leaves
    // and groups whose longest gap cannot hold the request are skipped.
    const auto scan_leaf = [&](std::size_t g, std::uint32_t l, std::uint32_t from)
        -> std::optional<Found> {
        const Leaf& leaf = leaves_[group_at(g).leaves[l]];
        for (std::uint32_t slot = from; slot < leaf.count; ++slot) {
            const Gap& gap = leaf.gaps[slot];
            if (causal_finish(gap.begin_ns, duration_ns) <= gap.end_ns) {
                return Found{gap, Position{g, l, slot}};
            }
        }
        return std::nullopt;
    };
    {
        const Group& group = group_at(after.group);
        if (!cannot_hold(group.max_durations[after.leaf], group.max_ends[after.leaf], duration_ns)) {
            if (const auto found = scan_leaf(after.group, after.leaf, after.slot)) {
                return found;
            }
        }
        for (std::uint32_t l = after.leaf + 1; l < group.count; ++l) {
            if (cannot_hold(group.max_durations[l], group.max_ends[l], duration_ns)) {
                continue;
            }
            if (const auto found = scan_leaf(after.group, l, 0)) {
                return found;
            }
        }
    }
    for (std::size_t g = after.group + 1; g < order_.size(); ++g) {
        if (cannot_hold(max_durations_[g], max_ends_[g], duration_ns)) {
            continue;
        }
        const Group& group = group_at(g);
        for (std::uint32_t l = 0; l < group.count; ++l) {
            if (cannot_hold(group.max_durations[l], group.max_ends[l], duration_ns)) {
                continue;
            }
            if (const auto found = scan_leaf(g, l, 0)) {
                return found;
            }
        }
    }
    return std::nullopt;
}

std::optional<GapCalendar::Gap> GapCalendar::first_fitting_gap(
    double earliest_ns,
    double duration_ns) const {
    // These checks stay in Release builds: a reservation placed behind the
    // pruned watermark is a causality violation that must fail closed
    // rather than silently corrupt the schedule.
    if (!std::isfinite(earliest_ns) || !std::isfinite(duration_ns) ||
        earliest_ns < pruned_through_ns_ || duration_ns <= 0.0) {
        throw std::runtime_error(
            "physical resource calendar received an invalid reservation");
    }
    if (earliest_ns >= ready_ns) {
        return std::nullopt;
    }
    if (last_gap_query_.valid && last_gap_query_.earliest_ns == earliest_ns &&
        last_gap_query_.duration_ns == duration_ns && last_gap_query_.frontier_ns == ready_ns) {
        return last_gap_query_.result;
    }
    const auto found = search(earliest_ns, duration_ns);
    std::optional<Gap> result;
    if (found) {
        result = found->gap;
    }
    last_gap_query_ = {true, earliest_ns, duration_ns, ready_ns, result};
    return result;
}

bool GapCalendar::can_reserve_exact(double begin_ns, double duration_ns) const {
    if (!std::isfinite(begin_ns) || !std::isfinite(duration_ns) ||
        begin_ns < pruned_through_ns_ || duration_ns <= 0.0 ||
        !std::isfinite(causal_finish(begin_ns, duration_ns))) {
        throw std::runtime_error(
            "physical resource calendar received an invalid exact-reservation query");
    }
    if (begin_ns >= ready_ns) {
        return true;
    }
    Position containing = upper_bound(begin_ns);
    if (!previous(containing)) {
        return false;
    }
    const Gap& gap = gap_at(containing);
    return begin_ns >= gap.begin_ns && causal_finish(begin_ns, duration_ns) <= gap.end_ns;
}

double GapCalendar::preview_start(double earliest_ns, double duration_ns) const {
    if (!std::isfinite(earliest_ns) || !std::isfinite(duration_ns) ||
        earliest_ns < pruned_through_ns_ || duration_ns <= 0.0) {
        throw std::runtime_error(
            "physical resource calendar received an invalid reservation preview");
    }
    if (const auto gap = first_fitting_gap(earliest_ns, duration_ns)) {
        return std::max(gap->begin_ns, earliest_ns);
    }
    const double start = std::max(earliest_ns, ready_ns);
    if (!std::isfinite(start) ||
        !std::isfinite(causal_finish(start, duration_ns))) {
        throw std::runtime_error("physical resource calendar preview time overflowed");
    }
    return start;
}

std::vector<GapCalendar::Gap> GapCalendar::gaps_after(double floor_ns) const {
    std::vector<Gap> result;
    for (Position p = first_after_floor(floor_ns); !at_end(p); p = next(p)) {
        const Gap& gap = gap_at(p);
        result.push_back({std::max(floor_ns, gap.begin_ns), gap.end_ns});
    }
    return result;
}

bool GapCalendar::same_gaps_after(const GapCalendar& other, double floor_ns) const {
    Position mine = first_after_floor(floor_ns);
    Position theirs = other.first_after_floor(floor_ns);
    while (!at_end(mine) && !other.at_end(theirs)) {
        const Gap& lhs = gap_at(mine);
        const Gap& rhs = other.gap_at(theirs);
        if (std::max(floor_ns, lhs.begin_ns) != std::max(floor_ns, rhs.begin_ns) ||
            lhs.end_ns != rhs.end_ns) {
            return false;
        }
        mine = next(mine);
        theirs = other.next(theirs);
    }
    return at_end(mine) && other.at_end(theirs);
}

std::uint64_t GapCalendar::gap_fingerprint_after(double floor_ns) const {
    const Position p = first_after_floor(floor_ns);
    if (at_end(p)) {
        return 0;
    }
    std::uint64_t fingerprint = 0;
    const Group& group = group_at(p.group);
    const Leaf& leaf = leaves_[group.leaves[p.leaf]];
    const Gap& first = leaf.gaps[p.slot];
    if (p.slot == 0 && first.begin_ns >= floor_ns) {
        fingerprint = group.fingerprints[p.leaf];
    } else {
        fingerprint = ResourceTimeline::fingerprint_gap(
            first.begin_ns < floor_ns ? Gap{floor_ns, first.end_ns} : first);
        for (std::uint32_t slot = p.slot + 1; slot < leaf.count; ++slot) {
            fingerprint ^= ResourceTimeline::fingerprint_gap(leaf.gaps[slot]);
        }
    }
    for (std::uint32_t l = p.leaf + 1; l < group.count; ++l) {
        fingerprint ^= group.fingerprints[l];
    }
    for (std::size_t g = p.group + 1; g < order_.size(); ++g) {
        fingerprint ^= fingerprints_[g];
    }
    return fingerprint;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

void GapCalendar::insert_gap(double begin_ns, double end_ns) {
    last_gap_query_.valid = false;
    if (!std::isfinite(begin_ns) || !std::isfinite(end_ns) ||
        begin_ns < pruned_through_ns_ || end_ns <= begin_ns) {
        throw std::runtime_error("physical resource calendar received an invalid idle gap");
    }
    const Position after = lower_bound(begin_ns);
    Position before = after;
    if (previous(before) && gap_at(before).end_ns > begin_ns) {
        throw std::runtime_error("physical resource calendar inserted overlapping idle gaps");
    }
    if (!at_end(after) && gap_at(after).begin_ns < end_ns) {
        throw std::runtime_error("physical resource calendar inserted overlapping idle gaps");
    }
    insert_at(after, Gap{begin_ns, end_ns});
}

void GapCalendar::insert_frontier_gap(double start_ns) {
    insert_gap(ready_ns, start_ns);
}

void GapCalendar::consume_gap(const Gap& gap, double begin_ns, double end_ns) {
    last_gap_query_.valid = false;
    if (reservation_outside(gap, begin_ns, end_ns)) {
        throw std::runtime_error(
            "physical resource calendar consumed outside an idle gap (gap=[" +
            std::to_string(gap.begin_ns) + "," +
            std::to_string(gap.end_ns) + "), request=[" +
            std::to_string(begin_ns) + "," +
            std::to_string(end_ns) + "))");
    }
    const Position held = lower_bound(gap.begin_ns);
    if (at_end(held) || gap_at(held).begin_ns != gap.begin_ns) {
        throw std::runtime_error(
            "physical resource calendar erased an idle gap it does not hold");
    }
    consume_at(held, gap, begin_ns, end_ns);
}

void GapCalendar::consume_at(Position held, const Gap& gap, double begin_ns, double end_ns) {
    const bool keep_head = gap.begin_ns < begin_ns;
    const bool keep_tail = end_ns < gap.end_ns;
    // The remainders are sub-intervals of the caller's gap. They can only
    // collide with the following gap when the caller's bounds exceed the
    // held gap; the treap reported that as an overlapping insertion.
    if (keep_head || keep_tail) {
        const Position following = next(held);
        if (!at_end(following) &&
            gap_at(following).begin_ns < (keep_tail ? gap.end_ns : begin_ns)) {
            throw std::runtime_error("physical resource calendar inserted overlapping idle gaps");
        }
    }
    if (keep_head && keep_tail) {
        replace_at(held, Gap{gap.begin_ns, begin_ns});
        insert_at(next(held), Gap{end_ns, gap.end_ns});
    } else if (keep_head) {
        replace_at(held, Gap{gap.begin_ns, begin_ns});
    } else if (keep_tail) {
        replace_at(held, Gap{end_ns, gap.end_ns});
    } else {
        erase_at(held);
    }
}

void GapCalendar::prune_before(double causal_watermark_ns) {
    if (!std::isfinite(causal_watermark_ns) || causal_watermark_ns < 0.0) {
        throw std::runtime_error("physical resource calendar received an invalid causal watermark");
    }
    if (causal_watermark_ns <= pruned_through_ns_) {
        return;
    }
    last_gap_query_.valid = false;
    // Gaps beginning before the watermark expire. Only the last of them can
    // cross the watermark; its usable suffix is kept exactly.
    const Position future = lower_bound(causal_watermark_ns);
    Position last_expired = future;
    std::optional<double> crossing_end_ns;
    if (previous(last_expired) && gap_at(last_expired).end_ns > causal_watermark_ns) {
        crossing_end_ns = gap_at(last_expired).end_ns;
    }
    drop_before(crossing_end_ns ? last_expired : future);
    pruned_through_ns_ = causal_watermark_ns;
    // A frontier behind the causal watermark represents expired idle time,
    // not future capacity. Advancing it is schedule-equivalent for all legal
    // future reservations and prevents reintroducing an expired prefix.
    ready_ns = std::max(ready_ns, causal_watermark_ns);
    if (crossing_end_ns) {
        replace_at(Position{head_, 0, 0}, Gap{causal_watermark_ns, *crossing_end_ns});
    }
}

double GapCalendar::reserve(double earliest_ns, double duration_ns) {
    if (!std::isfinite(earliest_ns) || !std::isfinite(duration_ns) ||
        earliest_ns < pruned_through_ns_ || duration_ns <= 0.0) {
        throw std::runtime_error(
            "physical resource calendar received an invalid reservation preview");
    }
    // Same lookup as first_fitting_gap, keeping the located position so the
    // consumption below does not search for the gap a second time.
    std::optional<Gap> gap;
    std::optional<Position> held;
    if (earliest_ns < ready_ns) {
        if (last_gap_query_.valid && last_gap_query_.earliest_ns == earliest_ns &&
            last_gap_query_.duration_ns == duration_ns &&
            last_gap_query_.frontier_ns == ready_ns) {
            gap = last_gap_query_.result;
        } else if (const auto found = search(earliest_ns, duration_ns)) {
            gap = found->gap;
            held = found->at;
        }
    }
    double start = 0.0;
    if (gap) {
        start = std::max(gap->begin_ns, earliest_ns);
    } else {
        start = std::max(earliest_ns, ready_ns);
        if (!std::isfinite(start) || !std::isfinite(causal_finish(start, duration_ns))) {
            throw std::runtime_error("physical resource calendar preview time overflowed");
        }
    }
    const double finish = causal_finish(start, duration_ns);
    if (gap) {
        last_gap_query_.valid = false;
        if (held && !reservation_outside(*gap, start, finish)) {
            consume_at(*held, *gap, start, finish);
        } else {
            consume_gap(*gap, start, finish);
        }
    } else {
        if (start > ready_ns) {
            insert_gap(ready_ns, start);
        }
        ready_ns = finish;
    }
    reserved_work_ns += duration_ns;
    return start;
}

}
