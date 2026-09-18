#ifndef GTSIM_AGGREGATE_SCHEDULER_OBSERVER_H
#define GTSIM_AGGREGATE_SCHEDULER_OBSERVER_H

#include "cycle.h"
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>

// Actual scheduler visits only. This is deliberately NOT a warp-cycle census.
// One simulator per host thread; enable/reset only outside a running kernel.
namespace GTSim::scheduler_observer {

enum class Reject : std::size_t {
    finished, already_issued, unresolved_dependency, future_ready_cycle,
    not_resident, tmem_unavailable, sram_issue_interval,
    memory_issue_interval, sram_read_queue_full, sram_write_queue_full,
    missing_pipeline, pipeline_issue_busy, count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(Reject::count)> reject_names = {
    "finished", "already_issued", "unresolved_dependency", "future_ready_cycle",
    "not_resident", "tmem_unavailable", "sram_issue_interval",
    "memory_issue_interval", "sram_read_queue_full", "sram_write_queue_full",
    "missing_pipeline", "pipeline_issue_busy"
};

struct Counters {
    std::uint64_t schedule_invocations = 0;
    std::uint64_t schedule_returns_with_issue = 0;
    std::uint64_t schedule_returns_without_issue = 0;
    std::uint64_t no_issue_no_residents = 0;
    std::uint64_t no_issue_after_selection_search = 0;
    std::uint64_t returned_issue_members = 0;
    Cycle first_observed_cycle = -1;
    Cycle last_observed_cycle = -1;

    std::uint64_t warp_attempts = 0;
    std::uint64_t invalid_warp_attempts = 0;
    std::uint64_t empty_ready_queue_attempts = 0;
    std::uint64_t finished_queue_entries_removed = 0;
    std::uint64_t issued_queue_entries_removed = 0;
    std::uint64_t group_head_deferred_attempts = 0;
    std::uint64_t bulk_head_attempts = 0;
    std::uint64_t bulk_head_unavailable_attempts = 0;
    std::uint64_t bulk_head_not_ready_attempts = 0;
    std::uint64_t barrier_head_attempts = 0;
    std::uint64_t barrier_head_not_ready_attempts = 0;
    std::uint64_t tail_barrier_selections = 0;

    // A candidate can be checked repeatedly by GTO/O2 or by group preflight.
    // Only node_ready_and_pipe invocations belong to this population.
    std::uint64_t candidate_predicate_checks = 0;
    std::uint64_t candidate_predicate_passes = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(Reject::count)> candidate_rejects{};
    std::uint64_t group_preflight_attempts = 0;
    std::uint64_t group_preflight_rejections = 0;

    // Actual issue_node calls, counted on each participating scheduler once.
    // Includes repeated sub-ops; this is not native SASS instruction count.
    std::uint64_t issue_commits = 0;
    std::uint64_t first_node_issue_commits = 0;
    std::uint64_t pipeline_issue_commits = 0;
    std::uint64_t barrier_issue_commits = 0;
    std::uint64_t bulk_issue_commits = 0;
};

inline thread_local bool enabled = false;
inline thread_local Counters counters{};
inline void reset() noexcept { counters = {}; }
inline void set_enabled(bool value) noexcept { enabled = value; }
inline bool is_enabled() noexcept { return enabled; }
inline Counters* active() noexcept { return enabled ? &counters : nullptr; }
inline Counters snapshot() noexcept { return counters; }
inline void reject(Counters* c, Reject reason) noexcept {
    if (c) ++c->candidate_rejects[static_cast<std::size_t>(reason)];
}
inline void begin_schedule(Counters* c, Cycle cycle) noexcept {
    if (!c) return;
    ++c->schedule_invocations;
    if (c->first_observed_cycle < 0 || cycle < c->first_observed_cycle)
        c->first_observed_cycle = cycle;
    if (cycle > c->last_observed_cycle) c->last_observed_cycle = cycle;
}
inline void end_schedule(Counters* c, std::size_t issued, bool no_residents = false) noexcept {
    if (!c) return;
    if (issued) {
        ++c->schedule_returns_with_issue;
        c->returned_issue_members += issued;
    } else {
        ++c->schedule_returns_without_issue;
        if (no_residents) ++c->no_issue_no_residents;
        else ++c->no_issue_after_selection_search;
    }
}

inline nlohmann::json snapshot_json() {
    const auto c = snapshot();
    nlohmann::json rejections = nlohmann::json::object();
    for (std::size_t i = 0; i < reject_names.size(); ++i)
        rejections[reject_names[i]] = c.candidate_rejects[i];
    return {
        {"schema", "GTSIM_SCHEDULER_VISIT_AGGREGATES_V1"}, {"enabled", enabled},
        {"scope", "actual visited scheduler decisions, across all SM/SPs in this host thread"},
        {"denominator", "schedule invocations / warp attempts / candidate predicate checks as named; never simulated cycles"},
        {"event_skipped_cycles", "unclassified; not reconstructed from host calls"},
        {"eligible_not_selected", nullptr}, {"unvisited_candidates", nullptr},
        {"dependency_root_cause", "not inferred; empty ready queues remain unclassified"},
        {"schedule_invocations", c.schedule_invocations},
        {"schedule_returns_with_issue", c.schedule_returns_with_issue},
        {"schedule_returns_without_issue", c.schedule_returns_without_issue},
        {"no_issue_no_residents", c.no_issue_no_residents},
        {"no_issue_after_selection_search", c.no_issue_after_selection_search},
        {"returned_issue_members", c.returned_issue_members},
        {"first_observed_cycle", c.first_observed_cycle}, {"last_observed_cycle", c.last_observed_cycle},
        {"warp_attempts", c.warp_attempts}, {"invalid_warp_attempts", c.invalid_warp_attempts},
        {"empty_ready_queue_attempts", c.empty_ready_queue_attempts},
        {"finished_queue_entries_removed", c.finished_queue_entries_removed},
        {"issued_queue_entries_removed", c.issued_queue_entries_removed},
        {"group_head_deferred_attempts", c.group_head_deferred_attempts},
        {"bulk_head_attempts", c.bulk_head_attempts},
        {"bulk_head_unavailable_attempts", c.bulk_head_unavailable_attempts},
        {"bulk_head_not_ready_attempts", c.bulk_head_not_ready_attempts},
        {"barrier_head_attempts", c.barrier_head_attempts},
        {"barrier_head_not_ready_attempts", c.barrier_head_not_ready_attempts},
        {"tail_barrier_selections", c.tail_barrier_selections},
        {"candidate_predicate_checks", c.candidate_predicate_checks},
        {"candidate_predicate_passes", c.candidate_predicate_passes},
        {"candidate_rejects", rejections},
        {"group_preflight_attempts", c.group_preflight_attempts},
        {"group_preflight_rejections", c.group_preflight_rejections},
        {"issue_commits", c.issue_commits}, {"first_node_issue_commits", c.first_node_issue_commits},
        {"pipeline_issue_commits", c.pipeline_issue_commits},
        {"barrier_issue_commits", c.barrier_issue_commits}, {"bulk_issue_commits", c.bulk_issue_commits}
    };
}
} // namespace GTSim::scheduler_observer
#endif
