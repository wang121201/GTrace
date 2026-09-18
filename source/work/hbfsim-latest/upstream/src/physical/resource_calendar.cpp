#include "physical/resource_calendar.hpp"

namespace hbfsim::physical {
std::uint64_t ResourceTimeline::fingerprint_gap(const Gap& gap) {
    const auto bits = [](double value) {
        return value == 0.0 ? std::uint64_t{0} : std::bit_cast<std::uint64_t>(value);
    };
    auto value = bits(gap.begin_ns) ^ std::rotl(bits(gap.end_ns), 29);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t ResourceTimeline::fingerprint_after(std::size_t root, double floor_ns) const {
    if (root == kNoNode) return 0;
    const auto& item = gap_nodes_[root];
    if (item.gap.end_ns <= floor_ns) return fingerprint_after(item.right, floor_ns);
    const auto right = item.right == kNoNode ? 0 : gap_nodes_[item.right].gap_fingerprint;
    if (item.gap.begin_ns < floor_ns)
        return fingerprint_gap({floor_ns, item.gap.end_ns}) ^ right;
    return fingerprint_after(item.left, floor_ns) ^ fingerprint_gap(item.gap) ^ right;
}

std::uint64_t ResourceTimeline::gap_fingerprint_after(double floor_ns) const {
    return fingerprint_after(gap_root_, floor_ns);
}

std::vector<ResourceTimeline::Gap> ResourceTimeline::gaps_after(double floor_ns) const {
    std::vector<Gap> result;
    const auto visit = [&](auto&& self, std::size_t node) -> void {
        if (node == kNoNode) return;
        const auto& item = gap_nodes_[node];
        if (item.gap.end_ns > floor_ns) {
            self(self, item.left);
            result.push_back({std::max(floor_ns, item.gap.begin_ns), item.gap.end_ns});
        }
        self(self, item.right);
    };
    visit(visit, gap_root_);
    return result;
}

double ResourceTimeline::reserve(double earliest_ns, double duration_ns) {
    const auto start = preview_start(earliest_ns, duration_ns);
    const auto finish = causal_finish(start, duration_ns);
    if (const auto gap = first_fitting_gap(earliest_ns, duration_ns)) {
        consume_gap(*gap, start, finish);
    } else {
        if (start > ready_ns) insert_frontier_gap(start);
        ready_ns = finish;
    }
    reserved_work_ns += duration_ns;
    return start;
}


std::uint64_t ResourceTimeline::next_priority() {
    // Deterministic xorshift64: priorities depend only on this calendar's
    // insertion sequence, so runs remain reproducible while the treap stays
    // balanced in expectation even for monotonically increasing gap starts.
    priority_state_ ^= priority_state_ << 13;
    priority_state_ ^= priority_state_ >> 7;
    priority_state_ ^= priority_state_ << 17;
    return priority_state_;
}

std::size_t ResourceTimeline::allocate_node(Gap gap) {
    const std::size_t node = [&] {
        if (free_gap_nodes_.empty()) {
            gap_nodes_.emplace_back();
            return gap_nodes_.size() - 1;
        }
        const auto recycled = free_gap_nodes_.back();
        free_gap_nodes_.pop_back();
        return recycled;
    }();
    gap_nodes_[node] = GapNode{
        .gap = gap,
        .priority = next_priority(),
        .max_duration_ns = gap.end_ns - gap.begin_ns,
        .gap_fingerprint = fingerprint_gap(gap),
    };
    return node;
}

void ResourceTimeline::recycle_node(std::size_t node) {
    if (node == kNoNode || node >= gap_nodes_.size()) {
        throw std::runtime_error(
            "physical resource calendar recycled a node outside its arena");
    }
    // The node is fully rewritten when reused; clearing it here only costs
    // stores.
    free_gap_nodes_.push_back(node);
}

void ResourceTimeline::recycle_subtree(std::size_t root) {
    if (root == kNoNode) {
        return;
    }
    const auto left = gap_nodes_[root].left;
    const auto right = gap_nodes_[root].right;
    recycle_subtree(left);
    recycle_subtree(right);
    recycle_node(root);
}

void ResourceTimeline::refresh(std::size_t node_index) {
    auto& node = gap_nodes_[node_index];
    node.max_duration_ns = node.gap.end_ns - node.gap.begin_ns;
    node.gap_fingerprint = fingerprint_gap(node.gap);
    if (node.left != kNoNode) {
        node.max_duration_ns = std::max(
            node.max_duration_ns, gap_nodes_[node.left].max_duration_ns);
        node.gap_fingerprint ^= gap_nodes_[node.left].gap_fingerprint;
    }
    if (node.right != kNoNode) {
        node.max_duration_ns = std::max(
            node.max_duration_ns, gap_nodes_[node.right].max_duration_ns);
        node.gap_fingerprint ^= gap_nodes_[node.right].gap_fingerprint;
    }
}

void ResourceTimeline::split(
    std::size_t root,
    double key,
    std::size_t& lower,
    std::size_t& upper) {
    if (root == kNoNode) {
        lower = kNoNode;
        upper = kNoNode;
        return;
    }
    if (gap_nodes_[root].gap.begin_ns < key) {
        std::size_t new_right = kNoNode;
        split(gap_nodes_[root].right, key, new_right, upper);
        gap_nodes_[root].right = new_right;
        refresh(root);
        lower = root;
    } else {
        std::size_t new_left = kNoNode;
        split(gap_nodes_[root].left, key, lower, new_left);
        gap_nodes_[root].left = new_left;
        refresh(root);
        upper = root;
    }
}

std::size_t ResourceTimeline::insert_node(
    std::size_t root,
    std::size_t node) {
    if (root == kNoNode) {
        return node;
    }
    if (gap_nodes_[node].gap.begin_ns == gap_nodes_[root].gap.begin_ns) {
        throw std::runtime_error(
            "physical resource calendar inserted two idle gaps at one instant");
    }
    if (gap_nodes_[node].priority > gap_nodes_[root].priority) {
        split(
            root,
            gap_nodes_[node].gap.begin_ns,
            gap_nodes_[node].left,
            gap_nodes_[node].right);
        refresh(node);
        return node;
    }
    if (gap_nodes_[node].gap.begin_ns < gap_nodes_[root].gap.begin_ns) {
        gap_nodes_[root].left = insert_node(gap_nodes_[root].left, node);
    } else {
        gap_nodes_[root].right = insert_node(gap_nodes_[root].right, node);
    }
    refresh(root);
    return root;
}

std::size_t ResourceTimeline::merge(
    std::size_t lower,
    std::size_t upper) {
    if (lower == kNoNode) {
        return upper;
    }
    if (upper == kNoNode) {
        return lower;
    }
    if (gap_nodes_[lower].priority > gap_nodes_[upper].priority) {
        gap_nodes_[lower].right = merge(gap_nodes_[lower].right, upper);
        refresh(lower);
        return lower;
    }
    gap_nodes_[upper].left = merge(lower, gap_nodes_[upper].left);
    refresh(upper);
    return upper;
}

std::size_t ResourceTimeline::erase_node(
    std::size_t root,
    double key,
    std::size_t& erased) {
    if (root == kNoNode) {
        throw std::runtime_error(
            "physical resource calendar erased an idle gap it does not hold");
    }
    if (key == gap_nodes_[root].gap.begin_ns) {
        erased = root;
        return merge(gap_nodes_[root].left, gap_nodes_[root].right);
    }
    if (key < gap_nodes_[root].gap.begin_ns) {
        gap_nodes_[root].left = erase_node(gap_nodes_[root].left, key, erased);
    } else {
        gap_nodes_[root].right = erase_node(gap_nodes_[root].right, key, erased);
    }
    refresh(root);
    return root;
}

const ResourceTimeline::GapNode*
ResourceTimeline::predecessor(std::size_t root, double key) const {
    const GapNode* result = nullptr;
    while (root != kNoNode) {
        const auto& node = gap_nodes_[root];
        if (node.gap.begin_ns <= key) {
            result = &node;
            root = node.right;
        } else {
            root = node.left;
        }
    }
    return result;
}

const ResourceTimeline::GapNode*
ResourceTimeline::successor(std::size_t root, double key) const {
    const GapNode* result = nullptr;
    while (root != kNoNode) {
        const auto& node = gap_nodes_[root];
        if (node.gap.begin_ns >= key) {
            result = &node;
            root = node.left;
        } else {
            root = node.right;
        }
    }
    return result;
}

const ResourceTimeline::GapNode*
ResourceTimeline::first_fitting_from(
    std::size_t root,
    double minimum_begin_ns,
    double duration_ns) const {
    if (root == kNoNode ||
        gap_nodes_[root].max_duration_ns < duration_ns) {
        return nullptr;
    }
    const auto& node = gap_nodes_[root];
    if (node.gap.begin_ns < minimum_begin_ns) {
        return first_fitting_from(node.right, minimum_begin_ns, duration_ns);
    }
    if (const auto* in_left = first_fitting_from(
            node.left, minimum_begin_ns, duration_ns)) {
        return in_left;
    }
    if (causal_finish(node.gap.begin_ns, duration_ns) <= node.gap.end_ns) {
        return &node;
    }
    return first_fitting_from(node.right, minimum_begin_ns, duration_ns);
}

std::optional<ResourceTimeline::Gap>
ResourceTimeline::first_fitting_gap(
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
    // Every retained gap ends at or before the reservation frontier. HBM
    // command streams often query beyond it while earlier buffer gaps must
    // remain available; those queries need no search through the history.
    if (earliest_ns >= ready_ns) {
        return std::nullopt;
    }
    if (last_gap_query_.valid && last_gap_query_.earliest_ns == earliest_ns &&
        last_gap_query_.duration_ns == duration_ns && last_gap_query_.frontier_ns == ready_ns) {
        return last_gap_query_.result;
    }
    const auto remember = [&](std::optional<Gap> result) {
        last_gap_query_ = {true, earliest_ns, duration_ns, ready_ns, result};
        return result;
    };
    if (const auto* containing = predecessor(gap_root_, earliest_ns);
        containing != nullptr &&
        causal_finish(earliest_ns, duration_ns) <=
            containing->gap.end_ns) {
        return remember(containing->gap);
    }
    if (const auto* later = first_fitting_from(
            gap_root_, earliest_ns, duration_ns)) {
        return remember(later->gap);
    }
    return remember(std::nullopt);
}

bool ResourceTimeline::can_reserve_exact(
    double begin_ns,
    double duration_ns) const {
    if (!std::isfinite(begin_ns) || !std::isfinite(duration_ns) ||
        begin_ns < pruned_through_ns_ || duration_ns <= 0.0 ||
        !std::isfinite(causal_finish(begin_ns, duration_ns))) {
        throw std::runtime_error(
            "physical resource calendar received an invalid exact-reservation query");
    }
    if (begin_ns >= ready_ns) {
        return true;
    }
    const auto* containing = predecessor(gap_root_, begin_ns);
    return containing != nullptr &&
        begin_ns >= containing->gap.begin_ns &&
        causal_finish(begin_ns, duration_ns) <= containing->gap.end_ns;
}

double ResourceTimeline::preview_start(
    double earliest_ns,
    double duration_ns) const {
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

void ResourceTimeline::insert_gap(double begin_ns, double end_ns) {
    last_gap_query_.valid = false;
    if (!std::isfinite(begin_ns) || !std::isfinite(end_ns) ||
        begin_ns < pruned_through_ns_ || end_ns <= begin_ns) {
        throw std::runtime_error("physical resource calendar received an invalid idle gap");
    }
    if (const auto* previous = predecessor(gap_root_, begin_ns);
        previous != nullptr && previous->gap.end_ns > begin_ns) {
        throw std::runtime_error("physical resource calendar inserted overlapping idle gaps");
    }
    if (const auto* next = successor(gap_root_, begin_ns);
        next != nullptr && next->gap.begin_ns < end_ns) {
        throw std::runtime_error("physical resource calendar inserted overlapping idle gaps");
    }
    insert_gap_unchecked(begin_ns, end_ns);
}

void ResourceTimeline::insert_gap_unchecked(double begin_ns, double end_ns) {
    last_gap_query_.valid = false;
    max_end_ns_ = std::max(max_end_ns_, end_ns);
    const auto node = allocate_node(Gap{
        .begin_ns = begin_ns,
        .end_ns = end_ns,
    });
    gap_root_ = insert_node(gap_root_, node);
}

void ResourceTimeline::insert_frontier_gap(double start_ns) {
    if (max_end_ns_ > ready_ns) {
        // A caller inserted idle time beyond the frontier directly; keep
        // the full overlap validation for that state.
        insert_gap(ready_ns, start_ns);
        return;
    }
    if (!std::isfinite(ready_ns) || !std::isfinite(start_ns) ||
        ready_ns < pruned_through_ns_ || start_ns <= ready_ns) {
        throw std::runtime_error("physical resource calendar received an invalid idle gap");
    }
    insert_gap_unchecked(ready_ns, start_ns);
}

void ResourceTimeline::consume_gap(
    const Gap& gap,
    double begin_ns,
    double end_ns) {
    last_gap_query_.valid = false;
    if (begin_ns < gap.begin_ns || end_ns > gap.end_ns || end_ns <= begin_ns) {
        throw std::runtime_error(
            "physical resource calendar consumed outside an idle gap (gap=[" +
            std::to_string(gap.begin_ns) + "," +
            std::to_string(gap.end_ns) + "), request=[" +
            std::to_string(begin_ns) + "," +
            std::to_string(end_ns) + "))");
    }
    std::size_t erased = kNoNode;
    gap_root_ = erase_node(gap_root_, gap.begin_ns, erased);
    if (erased == kNoNode) {
        throw std::runtime_error(
            "physical resource calendar consumed an idle gap it does not hold");
    }
    recycle_node(erased);
    // Both remainders lie inside the caller's gap. They can only collide
    // with the following gap when the caller's bounds exceed the gap that
    // was actually held; one successor lookup preserves that rejection.
    const bool keep_head = gap.begin_ns < begin_ns;
    const bool keep_tail = end_ns < gap.end_ns;
    if (keep_head || keep_tail) {
        if (const auto* following = successor(gap_root_, gap.begin_ns);
            following != nullptr &&
            following->gap.begin_ns < (keep_tail ? gap.end_ns : begin_ns)) {
            throw std::runtime_error("physical resource calendar inserted overlapping idle gaps");
        }
    }
    if (keep_head) {
        insert_gap_unchecked(gap.begin_ns, begin_ns);
    }
    if (keep_tail) {
        insert_gap_unchecked(end_ns, gap.end_ns);
    }
}

void ResourceTimeline::prune_before(double causal_watermark_ns) {
    if (!std::isfinite(causal_watermark_ns) || causal_watermark_ns < 0.0) {
        throw std::runtime_error("physical resource calendar received an invalid causal watermark");
    }
    if (causal_watermark_ns <= pruned_through_ns_) {
        return;
    }
    last_gap_query_.valid = false;

    std::size_t expired = kNoNode;
    std::size_t future = kNoNode;
    split(gap_root_, causal_watermark_ns, expired, future);

    // All intervals in `expired` begin before the watermark. Because gaps
    // are disjoint and ordered, only its rightmost interval can cross the
    // watermark; preserve that usable suffix exactly and discard the rest.
    std::optional<double> crossing_end_ns;
    std::size_t rightmost = expired;
    while (rightmost != kNoNode && gap_nodes_[rightmost].right != kNoNode) {
        rightmost = gap_nodes_[rightmost].right;
    }
    if (rightmost != kNoNode &&
        gap_nodes_[rightmost].gap.end_ns > causal_watermark_ns) {
        crossing_end_ns = gap_nodes_[rightmost].gap.end_ns;
    }

    gap_root_ = future;
    recycle_subtree(expired);
    pruned_through_ns_ = causal_watermark_ns;
    // A frontier behind the causal watermark represents expired idle time,
    // not future capacity. Advancing it is schedule-equivalent for all legal
    // future reservations and prevents reintroducing an expired prefix.
    ready_ns = std::max(ready_ns, causal_watermark_ns);
    if (crossing_end_ns) {
        // The crossing gap ended before every retained future gap began.
        insert_gap_unchecked(causal_watermark_ns, *crossing_end_ns);
    }
}

}
