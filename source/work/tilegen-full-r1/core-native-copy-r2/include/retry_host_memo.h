#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
namespace GTSim::retry_host {
// Pure host-work counters. None is a GPU cycle or a modeled stall count.
inline thread_local bool enabled=true;
inline thread_local bool prefix_enabled=false,ready_front_enabled=false;
struct Counts {
 std::uint64_t retry_entries_visited=0,ready_retry_attempts=0;
 std::uint64_t l1_classify_calls=0,l1_locate_calls=0;
 std::uint64_t negative_memo_hits=0,memo_negative_records=0;
 std::uint64_t memo_identity_rechecks=0,memo_epoch_rechecks=0;
 std::uint64_t prefix_entries_skipped=0,prefix_blocked_passes=0,prefix_front_attempts=0,prefix_stamp_invalidations=0,prefix_certification_passes=0;
 std::uint64_t next_event_retry_entries_visited=0,next_event_ready_front_shortcuts=0;
};
inline thread_local Counts counts{};
inline nlohmann::json snapshot(){return {{"schema","GTSIM_RETRY_HOST_WORK_V1"},{"negative_memo_enabled",enabled},{"scope","host work counters; not GPU stalls or cycles"},{"retry_entries_visited",counts.retry_entries_visited},{"ready_retry_attempts",counts.ready_retry_attempts},{"l1_classify_calls",counts.l1_classify_calls},{"l1_locate_calls",counts.l1_locate_calls},{"negative_memo_hits",counts.negative_memo_hits},{"memo_negative_records",counts.memo_negative_records},{"memo_identity_rechecks",counts.memo_identity_rechecks},{"memo_epoch_rechecks",counts.memo_epoch_rechecks},{"prefix_enabled",prefix_enabled},{"ready_front_enabled",ready_front_enabled},{"prefix_entries_skipped",counts.prefix_entries_skipped},{"prefix_blocked_passes",counts.prefix_blocked_passes},{"prefix_front_attempts",counts.prefix_front_attempts},{"prefix_stamp_invalidations",counts.prefix_stamp_invalidations},{"prefix_certification_passes",counts.prefix_certification_passes},{"next_event_retry_entries_visited",counts.next_event_retry_entries_visited},{"next_event_ready_front_shortcuts",counts.next_event_ready_front_shortcuts}};}
}
