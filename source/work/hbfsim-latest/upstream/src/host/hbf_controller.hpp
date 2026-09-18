#pragma once

#include "physical/physical_types.hpp"
#include "host/hbf_config.hpp"
#include "physical/hbf/hbf_device.hpp"
#include "physical/resource_calendar.hpp"
#include "physical/hbm/hbm_device.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>


namespace hbfsim::host {
using namespace hbfsim::physical;

inline constexpr std::string_view kPlacementMappingScheme =
    "page-striped-stack-local-mapping-v2";



struct ResidentMappingCapacity {
    std::uint64_t pages_per_stack = 0;
    std::uint64_t bytes_per_stack = 0;
    std::uint64_t total_bytes = 0;
};

[[nodiscard]] ResidentMappingCapacity derive_resident_mapping_capacity(
    const HbfConfig& config);

struct ControllerDramBudget {
    std::uint64_t bytes_per_stack = 0;
    std::uint64_t total_bytes = 0;
};

[[nodiscard]] ControllerDramBudget derive_controller_dram_budget(
    const HbfConfig& config,
    std::uint64_t capacity_denominator);

[[nodiscard]] ControllerDramBudget derive_write_buffer_dram_capacity(
    const HbfConfig& config);

struct HbfAddress {
    std::uint32_t stack = 0;
    std::uint32_t channel = 0;
    std::uint32_t die = 0;
    std::uint32_t plane = 0;
    std::uint32_t block = 0;
    std::uint32_t page = 0;
    std::uint64_t offset = 0;

    [[nodiscard]] std::string path() const;
};

struct HbfStats {
    std::uint64_t hbio_data_resources = 0;
    std::uint64_t hbio_command_resources = 0;
    std::uint64_t host_gc_read_bytes = 0;
    std::uint64_t host_gc_write_bytes = 0;
    double host_gc_control_ns = 0.0;
    std::uint64_t host_gc_buffer_bytes = 0;
    std::uint64_t host_zone_remaps = 0;
    std::uint64_t host_zone_invalidations = 0;
    std::uint64_t read_requests = 0;
    std::uint64_t program_requests = 0;
    std::uint64_t erase_requests = 0;
    std::uint64_t auto_erase_requests = 0;
    std::uint64_t logical_read_bytes = 0;
    std::uint64_t logical_write_bytes = 0;
    std::uint64_t physical_read_bytes = 0;
    std::uint64_t physical_write_bytes = 0;
    // Payload-byte decomposition of physical_write_bytes. These counters are
    // updated at the actual program event, after any controller coalescing.
    // They deliberately exclude OOB; ecc_encode_codeword_bytes below is the
    // corresponding raw page+OOB quantity.
    std::uint64_t data_program_payload_bytes = 0;
    // Direct programs into controller-bypassed physical extents. This is a
    // subset of data_program_payload_bytes/data_programs and lets lifecycle
    // studies distinguish append-to-publish traffic from page-mapped writes.
    std::uint64_t raw_physical_program_payload_bytes = 0;
    std::uint64_t raw_physical_programs = 0;
    std::uint64_t mapping_program_payload_bytes = 0;
    std::uint64_t gc_relocation_payload_bytes = 0;
    std::uint64_t page_reads = 0;
    // Foreground data-page programs after write-buffer coalescing. Excludes
    // mapping-page programs and every GC relocation program. Together:
    // page_programs == data_programs + mapping_page_programs + gc_relocations.
    std::uint64_t data_programs = 0;
    std::uint64_t page_programs = 0;
    std::uint64_t block_erases = 0;
    // Block-level wear telemetry over every non-static block. The sum includes
    // one owned in-flight raw erase per erase-pending block, matching the
    // issue-count semantics of block_erases. Static read-only blocks are
    // excluded because they cannot consume P/E cycles.
    std::uint64_t writable_blocks = 0;
    std::uint64_t writable_pages = 0;
    std::uint64_t writable_payload_bytes = 0;
    std::uint64_t worn_blocks = 0;
    std::uint64_t block_erase_count_sum = 0;
    long double block_erase_count_sum_squares = 0.0L;
    std::uint32_t min_block_erase_count = 0;
    std::uint32_t max_block_erase_count = 0;
    std::uint64_t mapping_entries = 0;
    // Exact per-block erase-count distribution over the same writable-block
    // population: histogram[erase_count] = number of blocks. Lifetime
    // criteria that retire the device once a fraction of its blocks reach a
    // P/E limit need this percentile view; sum/min/max/stddev cannot
    // recover it.
    std::map<std::uint32_t, std::uint64_t> block_erase_count_histogram;
    std::uint64_t invalidations = 0;
    // Logical footprint of the complete page-level L2P, independent of the
    // selected controller organization.
    std::uint64_t mapping_table_bytes = 0;
    std::uint64_t mapping_table_bytes_per_stack = 0;
    std::uint64_t mapping_table_pages_per_stack = 0;
    // FullResident-only provisioned footprint. These remain zero in Cached
    // mode so a receipt cannot confuse logical table size with DRAM use.
    std::uint64_t resident_mapping_table_bytes = 0;
    std::uint64_t resident_mapping_table_bytes_per_stack = 0;
    std::uint64_t resident_mapping_pages_per_stack = 0;
    // Cached-only global translation directory. It is charged before the
    // remaining controller DRAM is rounded down to whole cache pages.
    std::uint64_t mapping_directory_entry_bytes = 0;
    std::uint64_t mapping_directory_bytes = 0;
    std::uint64_t mapping_directory_bytes_per_stack = 0;
    // Cached-only usable capacity after even per-stack/page partitioning.
    // ctrl_dram_bytes may contain a small unusable remainder.
    std::uint64_t mapping_cache_capacity_bytes = 0;
    std::uint64_t mapping_cache_capacity_bytes_per_stack = 0;
    std::uint64_t mapping_cache_pages_per_stack = 0;
    // Capacity accounting for the one shared controller-DRAM pool. The write
    // buffer remains zero when write coalescing is disabled.
    std::uint64_t controller_dram_budget_bytes = 0;
    std::uint64_t controller_dram_budget_bytes_per_stack = 0;
    std::uint64_t host_hbm_reserved_bytes = 0;
    std::uint64_t host_hbm_read_bytes = 0;
    std::uint64_t host_hbm_write_bytes = 0;
    double host_hbm_busy_ns = 0.0;
    double host_hbm_queue_wait_ns = 0.0;
    std::uint64_t write_buffer_capacity_bytes = 0;
    std::uint64_t write_buffer_capacity_bytes_per_stack = 0;
    std::uint64_t mapping_cache_entries = 0;
    std::uint64_t mapping_cache_peak_entries = 0;
    std::uint64_t mapping_cache_live_bytes = 0;
    std::uint64_t mapping_cache_peak_bytes = 0;
    std::uint64_t mapping_cache_promotions = 0;
    std::uint64_t mapping_compression_input_bytes = 0;
    std::uint64_t mapping_compression_output_bytes = 0;
    std::uint64_t mapping_merge_media_reads = 0;
    std::uint64_t mapping_directory_lookup_ops = 0;
    std::uint64_t mapping_buffer_lookup_ops = 0;
    std::uint64_t mapping_buffer_patch_ops = 0;
    std::uint64_t mapping_dirty_probe_ops = 0;
    std::uint64_t mapping_checkpoint_epoch_ops = 0;
    std::uint64_t mapping_cache_buffer_hits = 0;
    std::uint64_t mapping_buffer_coalesced_reads = 0;
    std::uint64_t mapping_merge_buffer_hits = 0;
    double mapping_codec_work_ns = 0.0;
    std::uint64_t mapping_scratch_capacity_bytes = 0;
    std::uint64_t mapping_cache_hits = 0;
    std::uint64_t mapping_cache_misses = 0;
    std::uint64_t mapping_cache_erased_misses = 0;
    // A miss can be satisfied by a nested GC mapping access while dirty
    // victim writeback is making room. It remains a miss for the initiating
    // access, but does not issue a second media read or erased-page install.
    std::uint64_t mapping_cache_coalesced_misses = 0;
    std::uint64_t mapping_cache_evictions = 0;
    std::uint64_t mapping_cache_dirty_evictions = 0;
    std::uint64_t mapping_media_reads = 0;
    std::uint64_t mapping_media_read_bytes = 0;
    std::uint64_t mapping_lookup_ops = 0;
    std::uint64_t mapping_user_lookup_ops = 0;
    std::uint64_t mapping_gc_lookup_ops = 0;
    std::uint64_t mapping_update_ops = 0;
    std::uint64_t mapping_user_update_ops = 0;
    std::uint64_t mapping_gc_update_ops = 0;
    std::uint64_t mapping_dram_wait_ops = 0;
    double mapping_dram_wait_ns = 0.0;
    double mapping_dram_wait_max_ns = 0.0;
    double mapping_dram_issue_busy_ns = 0.0;
    std::uint64_t mapping_dram_resources = 0;
    std::uint64_t mapping_page_programs = 0;
    std::uint64_t static_reserved_pages = 0;
    // Erased or programmed pages fenced from the FTL for an externally mapped
    // physical extent. Unlike static_reserved_pages, erased pages remain
    // writable through AddressSpace::Physical.
    std::uint64_t raw_reserved_pages = 0;
    // Initial logical images are pre-existing media state: they consume
    // physical data/mapping pages but never count as workload writes or WAF.
    // Dense ranges use a compact directory and materialize only pages that
    // are overwritten or relocated.
    std::uint64_t initial_logical_data_pages = 0;
    std::uint64_t initial_mapping_pages = 0;
    std::uint64_t compact_initial_logical_data_pages = 0;
    std::uint64_t compact_initial_mapping_pages = 0;
    std::uint64_t compact_live_logical_data_pages = 0;
    std::uint64_t compact_live_mapping_pages = 0;
    std::uint64_t compact_retired_logical_data_pages = 0;
    std::uint64_t compact_retired_mapping_pages = 0;
    std::uint64_t write_buffer_hits = 0;
    std::uint64_t write_buffer_misses = 0;
    std::uint64_t read_buffer_hits = 0;
    std::uint64_t read_buffer_misses = 0;
    std::uint64_t read_buffer_read_bytes = 0;
    std::uint64_t write_buffer_flushes = 0;
    std::uint64_t write_buffer_merged_bytes = 0;
    std::uint64_t write_buffer_read_hits = 0;
    std::uint64_t write_buffer_read_bytes = 0;
    std::uint64_t write_buffer_dram_read_ops = 0;
    std::uint64_t write_buffer_dram_write_ops = 0;
    std::uint64_t write_buffer_dram_read_bytes = 0;
    std::uint64_t write_buffer_dram_write_bytes = 0;
    std::uint64_t write_buffer_dram_wait_ops = 0;
    double write_buffer_dram_wait_ns = 0.0;
    double write_buffer_dram_wait_max_ns = 0.0;
    double write_buffer_dram_issue_busy_ns = 0.0;
    // Writes that had to wait for a controller-DRAM slot: slots stay occupied until
    // the flushed page has actually been programmed, so sustained overload
    // is admitted at program rate (real capacity backpressure).
    std::uint64_t write_buffer_slot_wait_ops = 0;
    double write_buffer_slot_wait_ns = 0.0;
    std::uint64_t read_splits = 0;
    std::uint64_t read_split_pages = 0;
    std::uint64_t page_read_admission_events = 0;
    std::uint64_t page_read_admission_waited_pages = 0;
    double page_read_admission_wait_ns = 0.0;
    double page_read_admission_max_wait_ns = 0.0;
    // Exact page-granular read coverage in experiment receipts.
    std::uint64_t scalar_read_requests = 0;
    std::uint64_t scalar_read_pages = 0;
    std::uint64_t flash_scheduler_enqueues = 0;
    std::uint64_t flash_scheduler_issues = 0;
    std::uint64_t gc_runs = 0;
    std::uint64_t gc_relocations = 0;
    // Relocation and reclaim accounting is kept separate from foreground and
    // mapping-page programs. Every full GC victim must satisfy:
    //   gc_data_relocations + gc_mapping_relocations == gc_relocations
    //   gc_relocations + gc_reclaimed_invalid_pages
    //       == gc_runs * pages_per_block
    std::uint64_t gc_data_relocations = 0;
    std::uint64_t gc_mapping_relocations = 0;
    std::uint64_t gc_reclaimed_invalid_pages = 0;
    std::uint64_t gc_user_blocked_runs = 0;
    // Resolved logical capacity (configured or derived) and the GC-only
    // reserve it was derived against, in pages over the whole device.
    std::uint64_t logical_capacity_pages = 0;
    std::uint64_t logical_capacity_bytes = 0;
    std::uint64_t gc_reserve_pages = 0;
    // Static wear-leveling telemetry. One run relocates the live pages of one
    // cold closed block into a worn block and reclaims host ownership. Its
    // programs are kept apart from gc_relocations so GC victim conservation
    // holds and
    //   page_programs == data_programs + mapping_page_programs
    //       + gc_relocations + static_wear_leveling_relocations
    //   static_wear_leveling_relocations
    //       + static_wear_leveling_reclaimed_invalid_pages
    //       == static_wear_leveling_runs * pages_per_block
    //   block_erases == auto_erase_requests + erase_requests.
    std::uint64_t static_wear_leveling_checks = 0;
    std::uint64_t static_wear_leveling_runs = 0;
    std::uint64_t static_wear_leveling_relocations = 0;
    std::uint64_t static_wear_leveling_relocation_payload_bytes = 0;
    std::uint64_t static_wear_leveling_reclaimed_invalid_pages = 0;
    std::uint64_t static_wear_leveling_budget_deferrals = 0;
    std::uint64_t static_wear_leveling_cooldown_exclusions = 0;
    std::uint64_t static_wear_leveling_unique_source_blocks = 0;
    std::uint64_t static_wear_leveling_repeat_source_runs = 0;
    // Thermal governor telemetry. All fields stay zero while the thermal
    // model is disabled. Counters and work accumulate eagerly at media
    // admission; the temperature aggregates below them are refreshed only
    // by stats() and project every stack to the current finish frontier.
    std::uint64_t thermal_throttled_media_ops = 0;
    std::uint64_t thermal_throttle_engagements = 0;
    double thermal_throttle_wait_ns = 0.0;
    double thermal_pacing_busy_ns = 0.0;
    double thermal_throttled_span_ns = 0.0;
    double thermal_media_energy_j = 0.0;
    bool thermal_enabled = false;
    double thermal_boundary_temperature_c = 0.0;
    double thermal_boot_temperature_c = 0.0;
    double thermal_peak_temperature_c = 0.0;
    double thermal_final_temperature_c = 0.0;
    std::uint64_t thermal_throttled_stacks = 0;
    // Lazily recomputed structural audit of every physical page. Static
    // reservations consume whole blocks; pages without a materialized static
    // image are therefore reported explicitly instead of being mislabeled as
    // free or valid.
    std::uint64_t total_pages = 0;
    std::uint64_t free_pages = 0;
    std::uint64_t valid_pages = 0;
    std::uint64_t invalid_pages = 0;
    std::uint64_t pending_program_pages = 0;
    std::uint64_t pending_mapping_publications = 0;
    std::uint64_t static_unmaterialized_pages = 0;
    bool accounting_verified = false;
    double ecc_issue_busy_ns = 0.0;
    double ecc_decode_queue_wait_ns = 0.0;
    double ecc_encode_queue_wait_ns = 0.0;
    double ecc_decode_latency_work_ns = 0.0;
    double ecc_encode_latency_work_ns = 0.0;
    double ecc_decode_issue_busy_ns = 0.0;
    double ecc_encode_issue_busy_ns = 0.0;
    std::uint64_t ecc_decode_ops = 0;
    std::uint64_t ecc_encode_ops = 0;
    std::uint64_t ecc_codeword_bytes = 0;
    std::uint64_t ecc_decode_codeword_bytes = 0;
    std::uint64_t ecc_encode_codeword_bytes = 0;
    std::uint64_t active_ecc_dies = 0;
    std::uint64_t max_ecc_inflight_per_die = 0;
    double max_ecc_issue_busy_ns = 0.0;
    double avg_active_ecc_issue_busy_ns = 0.0;
    double media_busy_ns = 0.0;
    double hb_io_command_busy_ns = 0.0;
    double hb_io_data_busy_ns = 0.0;
    double sequencer_busy_ns = 0.0;
    double page_buffer_bank_busy_ns = 0.0;
    double read_lane_busy_ns = 0.0;
    double subarray_read_busy_ns = 0.0;
    double first_arrival_ns = std::numeric_limits<double>::infinity();
    double finish_ns = 0.0;
    std::uint64_t stacks = 0;
    std::uint64_t channels = 0;
    std::uint64_t active_channels = 0;
    std::uint64_t dies = 0;
    std::uint64_t active_dies = 0;
    std::uint64_t planes = 0;
    std::uint64_t active_planes = 0;
    std::uint64_t media_lanes = 0;
    std::uint64_t active_media_lanes = 0;
    std::uint64_t subarrays = 0;
    std::uint64_t active_subarrays = 0;
    std::uint64_t page_buffer_banks = 0;
    std::uint64_t active_page_buffer_banks = 0;
    std::uint64_t max_plane_ops = 0;
    std::uint64_t max_media_lane_reads = 0;
    std::uint64_t max_subarray_reads = 0;
    std::uint64_t max_page_buffer_bank_reads = 0;
    std::uint64_t max_die_transactions = 0;
    double max_plane_media_busy_ns = 0.0;
    double avg_active_plane_media_busy_ns = 0.0;
    double avg_active_plane_ops = 0.0;
    double max_media_lane_busy_ns = 0.0;
    double avg_active_media_lane_busy_ns = 0.0;
    double avg_active_media_lane_reads = 0.0;
    double max_subarray_busy_ns = 0.0;
    double avg_active_subarray_busy_ns = 0.0;
    double avg_active_subarray_reads = 0.0;
    double max_page_buffer_bank_busy_ns = 0.0;
    double avg_active_page_buffer_bank_busy_ns = 0.0;
    double avg_active_page_buffer_bank_reads = 0.0;
    double max_channel_busy_ns = 0.0;
    double avg_active_channel_busy_ns = 0.0;
    double avg_active_die_transactions = 0.0;
    // Sum of every top-level issue/drain Breakdown exactly once, including
    // all internal mapping/GC work attributed to that causal operation.
    // Stages may overlap and therefore must not be summed as elapsed time.
    Breakdown stage_work;
    // Exclusive resource reservation work. Dividing by resource count and
    // active wall span yields bounded utilization for the named resource.
    double logic_ingress_busy_ns = 0.0;
    double tsv_busy_ns = 0.0;
    double sram_busy_ns = 0.0;
    double flash_source_queue_busy_ns = 0.0;
    double channel_command_busy_ns = 0.0;
    double channel_data_busy_ns = 0.0;
    std::uint64_t logic_ingress_resources = 0;
    std::uint64_t tsv_resources = 0;
    std::uint64_t sram_resources = 0;
    std::uint64_t flash_source_queue_resources = 0;
    std::uint64_t channel_command_resources = 0;
    std::uint64_t channel_data_resources = 0;

    // Canonical WAF: programmed media payload bytes divided by host-written
    // bytes (logical writes plus raw append-to-publish programs). This
    // includes data-page rounding, mapping-page programs, and GC
    // relocations, but deliberately excludes OOB/ECC and transport framing.
    // A zero host-write denominator is undefined, never WAF=0.
    [[nodiscard]] std::optional<double> waf() const;
    [[nodiscard]] double active_span_ns() const;
    [[nodiscard]] double media_utilization() const;
    [[nodiscard]] double io_utilization() const;
    [[nodiscard]] double media_parallelism() const;
    [[nodiscard]] double read_lane_parallelism() const;
    [[nodiscard]] double subarray_read_parallelism() const;
    [[nodiscard]] double channel_parallelism() const;
    [[nodiscard]] double hbio_parallelism() const;
    [[nodiscard]] double hbio_command_utilization() const;
    [[nodiscard]] double hbio_data_utilization() const;
    [[nodiscard]] double sequencer_parallelism() const;
    [[nodiscard]] double ecc_issue_parallelism() const;
    [[nodiscard]] double ecc_issue_utilization() const;
    [[nodiscard]] double plane_media_skew() const;
    [[nodiscard]] double plane_op_skew() const;
    [[nodiscard]] double media_lane_skew() const;
    [[nodiscard]] double media_lane_read_skew() const;
    [[nodiscard]] double subarray_busy_skew() const;
    [[nodiscard]] double subarray_read_skew() const;
    [[nodiscard]] double page_buffer_bank_parallelism() const;
    [[nodiscard]] double page_buffer_bank_skew() const;
    [[nodiscard]] double page_buffer_bank_read_skew() const;
    [[nodiscard]] double channel_busy_skew() const;
    [[nodiscard]] double die_transaction_skew() const;
};

struct HbfReadEngineStats {
    std::uint64_t scalar_read_requests = 0;
    std::uint64_t scalar_read_pages = 0;
};

struct DirtyRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    // Last-writer provenance for these bytes. Write-buffer ranges remain
    // disjoint, so an overwrite can replace the source of only the bytes it
    // actually updates without relabeling the rest of the buffered page.
    HeatmapTrafficSource heatmap_source = HeatmapTrafficSource::Direct;
};

// Read-only, deterministic state exported for independent validation.  This
// deliberately contains semantic FTL state rather than scheduler internals:
// callers can prove mapping/page/block conservation without changing time,
// materializing callbacks, or depending on hash-table iteration order.
struct HbfAuditMapping {
    std::uint64_t key = 0;
    std::uint64_t ppn = 0;
};

struct HbfAuditPage {
    std::uint64_t ppn = 0;
    std::string status;
    std::string owner;
    std::uint64_t logical_key = 0;
    std::uint64_t block_epoch = 0;
};

struct HbfAuditBlock {
    std::uint64_t block = 0;
    std::string role;
    std::uint32_t valid_pages = 0;
    std::uint32_t invalid_pages = 0;
    std::uint32_t free_pages = 0;
    std::uint32_t next_page = 0;
    std::uint32_t erase_count = 0;
    std::uint32_t pending_program_pages = 0;
    std::uint32_t pending_mapping_publications = 0;
    std::uint64_t epoch = 0;
    bool erase_pending = false;
};

struct HbfAuditSnapshot {
    std::vector<HbfAuditMapping> logical_mappings;
    std::vector<HbfAuditMapping> mapping_pages;
    // Live pages and in-flight reservations only. Invalid occupancy is the
    // allocated block prefix minus live pages and pending reservations; dead
    // pages have no owner/mapping payload to retain or serialize.
    std::vector<HbfAuditPage> materialized_pages;
    std::vector<HbfAuditBlock> blocks;
    std::vector<std::uint64_t> free_pages_per_stack;
    std::vector<std::uint64_t> data_allocation_cursors;
    std::vector<std::uint64_t> mapping_allocation_cursors;
    std::vector<std::uint64_t> gc_allocation_cursors;
    std::vector<std::uint64_t> dirty_mapping_vpns;
    std::uint64_t free_pages = 0;
    std::uint64_t pending_dirty_mapping_events = 0;
    std::uint64_t pending_lpn_updates = 0;
    std::uint64_t pending_vpn_updates = 0;
    std::uint64_t pending_commits = 0;
    std::uint64_t write_buffer_entries = 0;
    std::uint64_t inflight_buffered_generations = 0;
    std::uint64_t pending_physical_programs = 0;
    std::uint64_t pending_block_transitions = 0;

    [[nodiscard]] bool quiescent() const {
        return dirty_mapping_vpns.empty() &&
            pending_dirty_mapping_events == 0 &&
            pending_lpn_updates == 0 &&
            pending_vpn_updates == 0 &&
            pending_commits == 0 &&
            write_buffer_entries == 0 &&
            inflight_buffered_generations == 0 &&
            pending_physical_programs == 0 &&
            pending_block_transitions == 0;
    }
};

// Quiescent, media-persistent state needed to resume the FTL in a fresh
// process. Controller caches, buffers, scheduler calendars, and timing
// frontiers are intentionally absent: a crash loses them. Version 3 carries
// live materialized state and the sparse compact directory. Invalid pages are
// implicit in each block's allocation prefix, not serialized page tombstones.
struct HbfPersistentPlane {
    std::vector<std::uint64_t> free_blocks;
    std::optional<std::uint64_t> active_data_block;
    std::optional<std::uint64_t> active_mapping_block;
    std::optional<std::uint64_t> active_gc_block;
};

struct HbfPersistentCompactVpnRange {
    std::uint64_t first_entry = 0;
    std::uint64_t page_count = 0;
    std::uint64_t stack_page_offset = 0;

    bool operator==(const HbfPersistentCompactVpnRange&) const = default;
};

struct HbfPersistentCompactLiveBlock {
    std::uint64_t block = 0;
    std::uint32_t live_pages = 0;

    bool operator==(const HbfPersistentCompactLiveBlock&) const = default;
};

struct HbfPersistentCompactImage {
    bool mutable_image = false;
    std::uint64_t first_lpn = 0;
    std::uint64_t page_count = 0;
    std::uint64_t first_vpn = 0;
    std::uint64_t vpn_slot_count = 0;
    std::uint64_t mapping_page_count = 0;
    std::vector<std::vector<std::uint64_t>> data_blocks_by_plane;
    std::vector<std::optional<std::uint64_t>> mapping_ppns;
    std::vector<HbfPersistentCompactVpnRange> vpn_ranges;
    std::vector<HbfPersistentCompactLiveBlock> live_data_pages_by_block;
    std::vector<HbfPersistentCompactLiveBlock> live_mapping_pages_by_block;
    std::vector<std::uint64_t> retired_lpns;
    std::vector<std::uint64_t> retired_mapping_vpns;

    bool operator==(const HbfPersistentCompactImage&) const = default;
};

struct HbfPersistentImage {
    std::uint32_t version = 5;
    std::uint32_t zone_size_blocks = 1;
    std::uint32_t channels_per_stack = 0;
    bool zone_managed = false;
    std::map<std::uint64_t, std::uint64_t> zone_remapping;
    std::uint32_t stacks = 0;
    std::uint64_t planes = 0;
    std::uint64_t blocks_per_plane = 0;
    std::uint64_t pages_per_block = 0;
    std::uint64_t page_size_bytes = 0;
    std::uint64_t mapping_entries_per_page = 0;
    HbfAuditSnapshot state;
    std::vector<HbfPersistentPlane> plane_state;
    std::optional<HbfPersistentCompactImage> compact_image;
};

// Aggregate physical block state over equal page ranges of the whole
// device, in PPN order (the same binning as the physical address heatmap).
// Static read-only blocks count their whole capacity as valid pages.
struct HbfBlockProfileBin {
    std::uint64_t blocks = 0;
    std::uint64_t valid_pages = 0;
    std::uint64_t invalid_pages = 0;
    std::uint64_t free_pages = 0;
    std::uint64_t pending_pages = 0;
    std::uint64_t erase_count_sum = 0;
    std::uint32_t min_erase_count = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t max_erase_count = 0;
    std::uint64_t data_blocks = 0;
    std::uint64_t gc_blocks = 0;
    std::uint64_t mapping_blocks = 0;
    std::uint64_t free_blocks = 0;
    std::uint64_t static_blocks = 0;
    std::uint64_t raw_physical_blocks = 0;
};

struct HbfQuiescenceStats {
    std::uint64_t dirty_mapping_pages = 0;
    std::uint64_t pending_dirty_mapping_events = 0;
    std::uint64_t pending_lpn_updates = 0;
    std::uint64_t pending_vpn_updates = 0;
    std::uint64_t pending_commits = 0;
    std::uint64_t write_buffer_entries = 0;
    std::uint64_t inflight_buffered_generations = 0;
    std::uint64_t pending_physical_programs = 0;
    std::uint64_t pending_block_transitions = 0;

    [[nodiscard]] bool quiescent() const {
        return dirty_mapping_pages == 0 &&
            pending_dirty_mapping_events == 0 &&
            pending_lpn_updates == 0 &&
            pending_vpn_updates == 0 &&
            pending_commits == 0 &&
            write_buffer_entries == 0 &&
            inflight_buffered_generations == 0 &&
            pending_physical_programs == 0 &&
            pending_block_transitions == 0;
    }
};

// Simulation facade: media timing/PEC/remap registers live here; host software
// policy and GC/WL state live in host::HbfController. Logical calls invoke that
// software layer. Channel-local calls expose the OCP invalid-zone lifecycle.
class HbfController {
public:
    explicit HbfController(
        HbfConfig config,
        AddressHeatmap* address_heatmap = nullptr);

    ~HbfController();
    HbfController(HbfController&&) noexcept;
    HbfController& operator=(HbfController&&) noexcept;
    void attach_hbm_buffer(physical::hbm::HbmDevice& hbm);

    [[nodiscard]] HbfAddress decode(std::uint64_t addr) const;
    [[nodiscard]] std::uint64_t encode(const HbfAddress& addr) const;
    [[nodiscard]] PhysicalCompletion issue(const PhysicalRequest& request);
    [[nodiscard]] PhysicalCompletion drain_pending(
        std::string id,
        double arrival_ns,
        TraceConfig trace = {});
    void prepopulate_raw_physical_page(std::uint64_t byte_address);
    void prepopulate_logical_pages(const std::vector<std::uint64_t>& lpns);
    // Dense read-only initial images are common in capacity sweeps. This
    // compact contiguous range form preserves logical translation and the
    // exact FTL physical placement, including partial boundary mapping pages,
    // while avoiding one hash-table entry per data page. Once installed, the
    // device accepts logical reads/drain only.
    void prepopulate_read_only_logical_page_range(
        std::uint64_t first_lpn,
        std::uint64_t page_count);
    // Mutable dense initial images use the same exact FTL placement as
    // prepopulate_logical_pages(), but keep untouched L2P/data state compact.
    // First overwrite and GC relocation materialize only the affected pages.
    void prepopulate_mutable_logical_page_range(
        std::uint64_t first_lpn,
        std::uint64_t page_count);
    // Raw/static data and the managed FTL may never own the same NAND block.
    // Reserve every block touched by these encoded PPNs before issuing work.
    // Repeated reservations are idempotent; intersecting live FTL state fails.
    void reserve_static_physical_pages(const std::vector<std::uint64_t>& ppns);
    // Reserve already-decoded physical block indices without materializing
    // page states. This is used by digest-bound immutable object populations
    // whose untouched pages still consume whole NAND blocks.
    void reserve_static_physical_block_indices(
        const std::vector<std::size_t>& block_indices);
    // Dense static images use the same block coordinate on every fabric
    // plane. Reserve an inclusive source-block extent without materializing
    // one page-table entry per immutable page.
    void reserve_static_physical_block_extent(
        std::uint32_t first_block,
        std::uint32_t block_count);
    // Fence a dense block-coordinate extent on every plane for direct,
    // sequential physical programming. The FTL can no longer allocate from
    // these blocks, while their erased pages remain raw-program targets.
    void reserve_raw_physical_block_extent(
        std::uint32_t first_block,
        std::uint32_t block_count);
    // A restored image must carry the externally declared raw ownership.
    void validate_raw_physical_block_extent(
        std::uint32_t first_block,
        std::uint32_t block_count) const;
    // Aggregate, mapping, and parallelism stats are recomputed lazily here:
    // refreshing them on every issue() scans live/pending mappings and every
    // subarray/lane/bank, which dominates large runs.
    [[nodiscard]] const HbfStats& stats() const {
        stats_.logical_capacity_pages = logical_capacity_pages();
        stats_.logical_capacity_bytes = stats_.logical_capacity_pages * config_.device.page_size_bytes;
        refresh_parallel_stats();
        return stats_;
    }
    // O(1) cumulative counters for persistent simulation-session batch deltas.
    // Structural capacity/wear and fabric-parallelism fields are refreshed
    // only by stats(); this view is deliberately safe on every serving batch.
    [[nodiscard]] const HbfStats& execution_stats() const { return stats_; }
    [[nodiscard]] const HbfConfig& config() const { return config_; }
    [[nodiscard]] std::uint64_t logical_capacity_pages() const;
    // O(1) execution-coverage snapshot for persistent simulation-session batch
    // receipts. Unlike stats(), this deliberately performs no geometry or
    // FTL audit and is therefore safe on every serving batch boundary.
    [[nodiscard]] HbfReadEngineStats read_engine_stats() const {
        return HbfReadEngineStats{
            .scalar_read_requests = stats_.scalar_read_requests,
            .scalar_read_pages = stats_.scalar_read_pages,
        };
    }
    [[nodiscard]] HbfQuiescenceStats quiescence_stats() const;
    // O(blocks) scan; bin_count must divide the device's block count.
    [[nodiscard]] std::vector<HbfBlockProfileBin> block_profile(
        std::size_t bin_count) const;
    // Exact effective P/E count in physical-block order for every writable
    // block. StaticReadOnly blocks are omitted, matching
    // HbfStats::writable_blocks; an already-scheduled erase is included before
    // its visibility callback.
    [[nodiscard]] std::vector<std::uint32_t> block_erase_counts() const;
    // OCP 0.7.0 section 11.4 / Figure 46. Channel-local addresses are packed
    // stack/channel/offset; Physical addresses remain simulator media probes.
    [[nodiscard]] PhysicalCompletion issue_channel_local(const PhysicalRequest& request);
    [[nodiscard]] PhysicalCompletion invalidate_zone(
        std::uint32_t stack, std::uint32_t channel, std::uint64_t zone, double at_ns);
    [[nodiscard]] PhysicalCompletion remap_zones(
        std::uint32_t stack, std::uint32_t channel,
        std::uint64_t first, std::uint64_t second, double at_ns);
    [[nodiscard]] PhysicalCompletion reset_zone(
        std::uint32_t stack, std::uint32_t channel, std::uint64_t zone, double at_ns);
    void write_wear_snapshot_json(std::ostream& output) const;
    [[nodiscard]] std::string wear_snapshot_json() const {
        std::ostringstream output;
        write_wear_snapshot_json(output);
        return output.str();
    }
    // Execute only lazy state callbacks whose modeled completion time is no
    // later than the caller's already-completed causal frontier. This does not
    // flush buffers, persist dirty mapping pages, or advance future media work.
    void materialize_committed_state_through(double completed_frontier_ns);
    [[nodiscard]] HbfAuditSnapshot audit_snapshot() const;
    [[nodiscard]] HbfPersistentImage persistent_image() const;
    void restore_persistent_image(const HbfPersistentImage& image);
    void attach_address_heatmap(AddressHeatmap& address_heatmap) {
        if (last_issue_arrival_ns_) {
            throw std::runtime_error(
                "HBF address heatmap must be attached before the first request");
        }
        address_heatmap_ = &address_heatmap;
    }
    // Which stack owns a logical page (shared-nothing FTL partition); the
    // composition layer needs this to route per-stack base-die-link traffic.
    [[nodiscard]] std::size_t stack_for_logical_page(std::uint64_t lpn) const {
        return stack_for_lpn(lpn);
    }

private:
    using ResourceTimeline = physical::ResourceTimeline;
    // White-box tests build frozen event snapshots without changing the
    // production layout or exposing cache mutation through the public API.
    friend struct HbfReadBufferTestAccess;
    friend struct HbfGcIndexTestAccess;

    enum class TransactionSource {
        User,
        Mapping,
        Host,
        Prepopulate,
    };

    enum class TransactionKind {
        Read,
        Program,
        Erase,
    };

    enum class MappingAccessKind {
        Lookup,
        Update,
    };

    // User and host GC reads return decoded payload over HBIO.
    // Only local read-modify-write and mapping fetches end in logic-die SRAM.
    enum class ReadPayloadRoute {
        External,
        HostBuffer,
    };

    enum class BlockRole {
        Free,
        StaticReadOnly,
        RawPhysical,
        Data,
        Mapping,
        GC,
    };

    enum class PageStatus {
        Erased,
        StaticReadOnly,
        Valid,
    };

    enum class PageOwner {
        Unassigned,
        Logical,
        Mapping,
        RawPhysical,
        StaticReadOnly,
    };

    // Exact serial-resource reservation calendar. Besides the busy-until
    // frontier it retains every still-usable idle interval behind that
    // frontier. The augmented treap indexes each subtree's largest gap, so
    // out-of-call-order reservations can find the earliest fitting interval
    // without a linear scan and without discarding physical capacity.
    struct PlaneState {
        struct BusyWindow {
            double begin_ns = 0.0;
            double end_ns = 0.0;
            // Program-suspend budget left inside this window and the
            // earliest instant the next suspension may start (the previous
            // suspension's resume has completed). Zero budget marks a
            // non-preemptible window (erase, or suspend disabled).
        };
        struct SubarrayState {
            ResourceTimeline timeline;
            double busy_ns = 0.0;
            std::uint64_t read_count = 0;
        };

        struct MediaLaneState {
            ResourceTimeline timeline;
            double busy_ns = 0.0;
            std::uint64_t read_count = 0;
        };

        struct PageBufferBankState {
            ResourceTimeline timeline;
            double busy_ns = 0.0;
            std::uint64_t read_count = 0;
        };

        // Non-preemptible program/erase reservations for the NAND bank.
        ResourceTimeline array_barrier;
        std::vector<BusyWindow> full_plane_windows;

        double media_busy_ns = 0.0;
        // Union of array-busy intervals. Batch reads share one tR window and
        // independent subarrays may overlap; summing per-request tR would
        // produce impossible >100% normalized plane utilization.
        std::vector<BusyWindow> media_busy_windows;
        std::uint64_t read_count = 0;
        std::uint64_t program_count = 0;
        std::uint64_t erase_count = 0;
        // Plane erase count at the last static wear-leveling cold-block
        // search; the next search waits for the configured interval.
        std::uint64_t static_wear_leveling_checked_erase_count = 0;
        std::vector<SubarrayState> subarrays;
        std::vector<MediaLaneState> media_lanes;
        std::vector<PageBufferBankState> page_buffer_banks;
        std::deque<std::size_t> free_blocks;
        std::optional<std::size_t> active_data_block;
        std::optional<std::size_t> active_mapping_block;
        std::optional<std::size_t> active_gc_block;
    };

    struct DieState {
        struct EccInflightInterval {
            double start_ns = 0.0;
            double finish_ns = 0.0;
        };

        ResourceTimeline sequencer;
        // Decode and encode conservatively share one pipelined issue port per
        // die. Only the initiation interval occupies this calendar; response
        // latency may overlap later codewords.
        ResourceTimeline ecc_issue;
        // Retained latency intervals: only those that may still overlap a
        // future codeword, i.e. that finish after the causal arrival
        // watermark. Intervals behind the watermark are folded into
        // ecc_max_inflight_folded, the exact peak concurrency over all time
        // points at or before ecc_folded_through_ns, so the public statistic
        // stays exact while memory stays bounded by in-flight work.
        std::vector<EccInflightInterval> ecc_inflight_intervals;
        std::uint64_t ecc_max_inflight_folded = 0;
        double ecc_folded_through_ns = 0.0;
        std::size_t ecc_intervals_at_last_fold = 0;
        // A compressed run has a regular ECC initiation train. Retaining its
        // analytical peak avoids one host object per 4 KiB codeword while
        // preserving the public pipeline-concurrency statistic.
        double ecc_issue_busy_ns = 0.0;
        std::uint64_t ecc_decode_ops = 0;
        std::uint64_t ecc_encode_ops = 0;
        double sequencer_busy_ns = 0.0;
        std::uint64_t transaction_count = 0;
        // Per-source TSU slots issue whichever transaction is ready first
        // (backfill): in-order issue would let one late transaction park
        // the queue while the die sits idle.
        std::array<ResourceTimeline, 4> source_queues{};
    };

    struct ChannelState {
        ResourceTimeline command;
        ResourceTimeline data;
        double command_busy_ns = 0.0;
        double data_busy_ns = 0.0;
        std::uint64_t command_count = 0;
        std::uint64_t data_count = 0;
    };

    struct LogicDieState {
        // Ingress admission accepts requests in ARRIVAL order: a backfilled
        // timeline of issue slots. A bare busy-until cursor ordered requests
        // by CALL order instead — one late-arriving background op (e.g. a
        // staged backing write) parked the cursor in the future and every
        // later-called but earlier-arriving read queued behind it (measured:
        // staged fills waiting >1 ms of phantom ingress queue).
        ResourceTimeline ingress;
        ResourceTimeline tsv;
        // Every reservation on this finite SRAM calendar must contribute the
        // same duration to Breakdown::sram_staging_ns. stats() enforces that
        // aggregate work-conservation invariant.
        ResourceTimeline sram;
        // Single pipelined controller-DRAM issue port for the stack-resident
        // L2P table. Only issue_ns occupies the port; response latency overlaps
        // later accesses.
        ResourceTimeline mapping_dram_issue;
        // Data-buffer bank in the same capacity pool. A separate issue
        // timeline represents an independently banked data path; capacity is
        // nevertheless shared exactly with mapping metadata.
        ResourceTimeline write_buffer_dram_issue;

    };

    struct PageState {
        PageStatus status = PageStatus::Erased;
        PageOwner owner = PageOwner::Unassigned;
        std::uint64_t lpn = 0;
        std::uint64_t block_epoch = 0;
    };

    struct BlockState {
        BlockRole role = BlockRole::Free;
        std::uint32_t valid_pages = 0;
        std::uint32_t invalid_pages = 0;
        std::uint32_t free_pages = 0;
        std::uint32_t next_page = 0;
        std::uint32_t erase_count = 0;
        // GC may reclaim only fully committed blocks. Allocated-but-not-yet
        // programmed pages and programmed-but-not-yet-published mappings are
        // explicit ownership pins, not invalid/free space.
        std::uint32_t pending_program_pages = 0;
        std::uint32_t pending_mapping_publications = 0;
        // Completion of the latest read/program already issued to this block.
        // A later destructive erase may backfill around other blocks, but not
        // ahead of an earlier access to its own target.
        double issued_media_ready_ns = 0.0;
        // Monotonic incarnation of this physical block. Every page
        // reservation and delayed mapping publication captures it; an erase
        // increments the epoch so stale callbacks cannot resurrect data.
        std::uint64_t epoch = 0;
        bool erase_pending = false;
        // Derived host-side GC index bookkeeping, never persisted. Coalesce
        // all page updates to this block until the next victim selection.
        std::uint32_t gc_bucket = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t gc_erase_key = 0;
        bool gc_index_dirty = false;
        // Allocate the 1024-bit validity map only after a block receives live
        // data. Multi-million-block profiles otherwise spent ~512 MiB on
        // bitmaps for erased blocks before the first request.
        std::unique_ptr<std::array<std::uint64_t, 16>> valid_bitmap;

        void set_valid(std::uint32_t page) {
            if (!valid_bitmap) {
                valid_bitmap = std::make_unique<std::array<std::uint64_t, 16>>();
                valid_bitmap->fill(0);
            }
            (*valid_bitmap)[page >> 6] |= 1ull << (page & 63);
        }
        void clear_valid(std::uint32_t page) {
            if (valid_bitmap) {
                (*valid_bitmap)[page >> 6] &= ~(1ull << (page & 63));
            }
        }
        [[nodiscard]] bool is_valid(std::uint32_t page) const {
            return valid_bitmap && (((*valid_bitmap)[page >> 6] >> (page & 63)) & 1);
        }
        void set_valid_range(std::uint32_t first_page, std::uint32_t count) {
            if (count == 0 || first_page + count > 1024) {
                throw std::runtime_error("invalid HBF compact valid-page range");
            }
            if (!valid_bitmap) {
                valid_bitmap = std::make_unique<std::array<std::uint64_t, 16>>();
                valid_bitmap->fill(0);
            }
            const auto end = first_page + count;
            auto page = first_page;
            while (page < end) {
                const auto word = page >> 6;
                const auto word_end = std::min<std::uint32_t>(
                    end,
                    (word + 1) << 6);
                const auto width = word_end - page;
                const auto low = page & 63;
                const auto mask = width == 64 ?
                    std::numeric_limits<std::uint64_t>::max() :
                    ((std::uint64_t{1} << width) - 1) << low;
                (*valid_bitmap)[word] |= mask;
                page = word_end;
            }
        }
    };

    // An original compact page needs only one retirement bit, not a hash
    // node for its full LPN. Allocate one word for each touched 64-page group;
    // this remains sparse for cold weights and compact for sequential KV.
    struct RetiredPageSet {
        std::unordered_map<std::uint64_t, std::uint64_t> words;
        std::uint64_t count = 0;

        [[nodiscard]] bool contains(std::uint64_t page) const {
            const auto word = words.find(page >> 6);
            return word != words.end() &&
                (word->second & (std::uint64_t{1} << (page & 63))) != 0;
        }
        bool insert(std::uint64_t page) {
            auto& word = words[page >> 6];
            const auto bit = std::uint64_t{1} << (page & 63);
            if (word & bit) return false;
            word |= bit;
            ++count;
            return true;
        }
        [[nodiscard]] std::uint64_t size() const { return count; }
        template<class Callback> void for_each(Callback&& callback) const {
            for (const auto& [index, word] : words) {
                auto bits = word;
                while (bits) {
                    callback((index << 6) + std::countr_zero(bits));
                    bits &= bits - 1;
                }
            }
        }
    };

    struct CompactLogicalImage {
        struct VpnRange {
            // Mapping pages are stack-local: entry i covers the i-th page
            // assigned to this stack, not one contiguous run of global LPNs.
            std::uint64_t first_entry = 0;
            std::uint64_t page_count = 0;
            // Number of earlier compact-image data pages allocated on this
            // VPN's stack. Together with the in-range LPN offset, this recovers
            // the ordinary round-robin data-allocation plane and page ordinal.
            std::uint64_t stack_page_offset = 0;
        };

        struct DataBlockLocation {
            std::size_t plane = 0;
            std::uint64_t block_ordinal = 0;
        };

        bool mutable_image = false;
        std::uint64_t first_lpn = 0;
        std::uint64_t page_count = 0;
        std::uint64_t first_vpn = 0;
        // Consecutive directory slots covering all stacks in every touched
        // mapping group. A partial first/last group can leave inactive slots.
        std::uint64_t vpn_slot_count = 0;
        std::uint64_t mapping_page_count = 0;
        // Per physical plane, data blocks in the order that the normal FTL
        // allocator assigned them. Page ordinal within a plane selects one
        // block/page without materializing an L2P entry.
        std::vector<std::vector<std::uint64_t>> data_blocks_by_plane;
        // One optional mapping-page PPN per directory slot. This is small
        // (one entry per 2 MiB with the published 512-entry mapping page and
        // 4 KiB data pages); inactive boundary slots remain empty.
        std::vector<std::optional<std::uint64_t>> mapping_ppns;
        // One compact descriptor per directory slot. Boundary slots may cover
        // only a suffix or prefix of their stack-local mapping page.
        std::vector<VpnRange> vpn_ranges;
        // Sparse reverse indexes let GC recover the logical owner of a compact
        // valid page without expanding one entry per data page.
        std::unordered_map<std::uint64_t, DataBlockLocation>
            data_block_locations;
        std::vector<std::vector<std::uint64_t>> vpn_offsets_by_stack;
        std::unordered_map<std::uint64_t, std::uint64_t>
            mapping_vpn_by_ppn;
        // Live compact pages remain represented only by block bitmaps and
        // these per-block counts. Retirement clears a validity bit; the
        // allocated prefix retains invalid occupancy without a PageState.
        std::unordered_map<std::uint64_t, std::uint32_t>
            live_data_pages_by_block;
        std::unordered_map<std::uint64_t, std::uint32_t>
            live_mapping_pages_by_block;
        RetiredPageSet retired_lpns;
        RetiredPageSet retired_mapping_vpns;
    };

    struct CompactPageIdentity {
        std::uint64_t logical_key = 0;
        PageOwner owner = PageOwner::Unassigned;
    };

    struct PageRunSpan {
        std::size_t plane = 0;
        // Page ordinal in this physical plane (block * pages_per_block + page).
        std::uint64_t first_page = 0;
        std::uint64_t page_count = 0;
    };

    struct GcHeadroom {
        // The stack's GC pool: every whole free block plus the free pages of
        // the GC write frontier. Relocation and its induced translation
        // writebacks draw on all of it.
        std::uint64_t relocation_pages = 0;
        // Pages the requested foreground role can allocate right now: its
        // own write frontiers plus the whole free blocks it may open
        // without taking the pool below the floor that one worst-case
        // victim needs (gc_reserve_requirement_pages). Active capacity
        // belongs only to that role.
        std::uint64_t foreground_pages = 0;
        // Pages the same role can write before the pool drops to the
        // configured GC-only reserve (gc_reserved_free_blocks_per_plane
        // pooled per stack): its frontiers plus the pool above the reserve.
        // Preventive GC keeps this above the low watermark.
        std::uint64_t preventive_pages = 0;
        // Pages the stack's in-flight relocation reclaims will add to
        // foreground_pages / preventive_pages when they commit. Preventive
        // GC counts them so it never over-commits the pool while reclaims
        // are still in flight.
        std::uint64_t returning_pages = 0;
        std::uint64_t returning_preventive_pages = 0;
    };

    enum class RelocationPurpose {
        GarbageCollection,
        StaticWearLeveling,
    };

    // One block whose live pages are being relocated a few at a time. Each
    // paced step reads, programs and conditionally publishes the next live
    // pages after scan_page; reclaim is scheduled once the scan passes the
    // last page. Every commit sequence scheduled by a step is recorded so the
    // reclaim dependency chain stays exact (unrelated commits scheduled by
    // interleaved foreground work are never pulled forward).
    struct ActiveRelocation {
        std::size_t block = 0;
        RelocationPurpose purpose = RelocationPurpose::GarbageCollection;
        std::uint32_t scan_page = 0;
        std::uint32_t invalid_pages_at_start = 0;
        std::uint32_t relocated_pages = 0;
        // Wear-leveling migrations write into the stack's cold frontier,
        // which must lead the cold block by at least half the gap.
        std::uint32_t cold_destination_minimum_erases = 0;
        // A partial cold frontier can require a second destination. Reserve
        // that eligible worn block at admission; a generic free-page count
        // cannot guarantee a later block meets the wear floor.
        std::optional<std::size_t> cold_destination_spare;
        double selection_ready_ns = 0.0;
        double chain_ready_ns = 0.0;
        std::vector<std::uint64_t> dependency_sequences;
        std::vector<std::uint64_t> destination_ppns;
    };

    struct ScheduledTransfer {
        double start_ns = 0.0;
        double finish_ns = 0.0;
        double wait_ns = 0.0;
    };



    struct WriteBufferEntry {
        std::uint64_t lpn = 0;
        std::vector<DirtyRange> ranges;
        std::list<std::uint64_t>::iterator iterator;
        // Merged data is readable only after its Host DRAM access completes.
        double ready_ns = 0.0;
    };

    struct InflightBufferedWrite {
        std::uint64_t generation = 0;
        std::vector<DirtyRange> ranges;
        double ready_ns = 0.0;
        double commit_ns = 0.0;
        std::uint64_t target_ppn = 0;
        std::uint64_t target_block_epoch = 0;
    };

    struct PendingMappingUpdate {
        std::uint64_t sequence = 0;
        std::uint64_t program_commit_sequence = 0;
        double commit_ns = 0.0;
        std::uint64_t new_ppn = 0;
        std::uint64_t block_epoch = 0;
        // A destructive raw erase can retire the target before this mapping
        // publishes. Keep the record as an ordering tombstone until both the
        // mapping callback and that erase have completed.
        double destructive_ready_ns = 0.0;
        // Conditional publication of a GC/wear-leveling copy (carries an
        // expected old PPN). Reads may bypass it while the old copy is
        // intact; host writes never wait for it.
        bool relocation = false;
    };

    struct PendingCommit {
        std::size_t stack = 0;
        std::function<void()> action;
    };

    struct PendingPhysicalProgram {
        double commit_ns = 0.0;
        std::uint64_t commit_sequence = 0;
    };

    // A host reclaim or an explicit raw erase awaiting its dependencies.
    struct PendingBlockTransition {
        double finish_ns = 0.0;
        std::uint64_t commit_sequence = 0;
        bool garbage_collection = false;
        // Diagnostic: the relocation chain was a static wear-leveling
        // migration of a fully valid cold block rather than a GC victim.
        bool static_wear_leveling = false;
        // A GC reclaim is legal only after every relocation program,
        // translation publication, and induced mapping checkpoint has
        // committed.  Raw physical erases leave this list empty.
        std::vector<std::uint64_t> dependency_sequences;
        // Reverse ownership edge used when a raw erase targets a relocation
        // destination before this source chain has committed.
        std::vector<std::uint64_t> destination_ppns;
    };

    struct MappingCacheEntry {
        // A hit may pipeline once the miss fill has produced the line. The
        // physical slot may be evicted only after every issued DRAM access has
        // returned. Keeping these frontiers separate prevents an unrelated
        // entry in the same mapping page from waiting on a future FTL update.
        double fill_ready_ns = 0.0;
        double evictable_ns = 0.0;
        std::uint64_t backing_vpn = 0;
        std::uint64_t charged_bytes = 0;
        bool dense = true;
        double modified_ns = -1.0;
        std::uint32_t pins = 0;
        std::list<std::uint64_t>::iterator iterator;
    };

    struct MappingScratchPage {
        std::optional<std::uint64_t> vpn;
        double fill_ready_ns = 0.0;
        double evictable_ns = 0.0;
        std::uint64_t touch = 0;
    };

    // Copyable lumped-RC thermal node so stats() can project a stack to the
    // finish frontier without mutating governor state. Heat is deposited at
    // media admission; scalar reads reserve their budget per request while
    // depositing heat per actual media page.
    struct ThermalNodeState {
        double temperature_c = 0.0;
        double clock_ns = 0.0;
        bool throttled = false;
        double peak_c = 0.0;
    };

    struct ThermalStackState {
        ThermalNodeState node;
        // Non-overlapping energy / power slots cap the throttled budget.
        // A program reserved after a future erase must leave the intervening
        // idle budget available to independent banks.
        ResourceTimeline pacing;
    };

    struct ThermalAdmission {
        double ready_ns = 0.0;
        double budget_finish_ns = 0.0;
    };

    HbfConfig config_;
    std::unique_ptr<physical::hbf::HbfDevice> media_;
    bool media_image_seeded_ = false;
    void seed_media_image();
    AddressHeatmap* address_heatmap_ = nullptr;
    std::uint64_t zone_index(std::uint32_t stack,
        std::uint32_t channel, std::uint64_t zone) const;
    std::uint64_t physical_zone(std::uint64_t zone) const;
    bool zone_available(std::uint64_t physical, bool invalid) const;
    void claim_zone(std::uint64_t physical);
    void zone_barrier(double at_ns);
    std::vector<double> control_ready_ns_;
    std::vector<double> copy_ready_ns_;
    std::uint64_t gc_reserve_requirement_pages() const;
    std::uint64_t gc_reserve_pages(std::size_t stack) const;
    std::uint64_t dirty_cached_mapping_pages(std::size_t stack) const;
    std::uint64_t induced_translation_writebacks(std::size_t block_index,
        std::uint64_t dirty_cached_mapping_pages) const;
    std::uint64_t mapping_allocation_pool_demand(std::size_t stack,
        std::uint64_t pages) const;
    std::uint64_t mapping_frontier_free_pages(std::size_t stack) const;
    std::uint64_t mapping_pool_demand(std::uint64_t pages, std::uint64_t frontier_free_pages) const;
    bool mapping_allocation_preserves_relocation(std::size_t stack) const;
    std::uint64_t worst_gc_victim_demand(std::size_t stack) const;
    std::uint64_t gc_relocation_capacity(std::size_t stack) const;
    void remove_managed_block_wear(std::size_t block);
    // Every block role change goes through here to keep the active-plane
    // counters exact; the accounting audit cross-checks them by rescanning.
    [[nodiscard]] static bool managed_block_role(BlockRole role);
    void set_block_role(std::size_t block_index, BlockRole role);
    void rebuild_active_plane_counts();
    [[nodiscard]] std::size_t scan_active_planes_in_stack(std::size_t stack) const;
    void add_managed_block_wear(std::size_t block);
    GcHeadroom gc_headroom(std::size_t stack,
        BlockRole allocation_role) const;
    void mark_gc_candidate_dirty(std::size_t block);
    void refresh_gc_candidates(std::size_t stack);
    std::optional<std::size_t> choose_gc_victim(std::size_t stack);
    std::optional<ActiveRelocation> start_relocation(std::size_t block_index,
        RelocationPurpose purpose,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    bool relocation_scan_complete(const ActiveRelocation& relocation) const;
    std::optional<std::size_t> ensure_cold_block(std::size_t stack,
        std::uint32_t minimum_erase_count);
    std::uint32_t relocate_live_pages(ActiveRelocation& relocation,
        double at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        std::uint32_t max_pages);
    double schedule_relocation_reclaim(ActiveRelocation relocation,
        double at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    std::optional<std::size_t> pending_gc_reclaim(std::size_t stack) const;
    void maybe_run_gc(double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        std::uint64_t required_pages,
        std::size_t stack,
        BlockRole allocation_role,
        std::optional<std::size_t> preferred_plane = std::nullopt);
    void complete_active_relocations(double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    std::uint64_t allocate_page_from_pinned_block(std::size_t block_index,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    std::uint64_t wear_leveling_erase_clock(std::size_t stack) const;
    void maybe_start_static_wear_leveling(double at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        std::size_t stack,
        std::size_t hot_plane,
        std::uint32_t hot_erase_count);
    void advance_static_wear_leveling(double at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        std::size_t stack);
    std::uint64_t stack_minimum_erase_count(std::size_t stack) const;

    using GcCandidateKey = std::pair<std::uint32_t, std::size_t>;
    using GcCandidateBucket = std::set<GcCandidateKey>;
    std::vector<std::vector<GcCandidateBucket>> gc_candidates_by_stack_;
    std::vector<std::vector<std::size_t>> gc_dirty_blocks_by_stack_;
    std::vector<std::map<std::uint32_t, std::uint64_t>> managed_wear_by_stack_;
    std::vector<bool> gc_active_by_stack_;
    std::vector<std::optional<ActiveRelocation>> gc_victim_by_stack_;
    std::vector<std::optional<ActiveRelocation>> wear_leveling_by_stack_;
    std::vector<std::optional<std::size_t>> pending_wear_check_by_stack_;
    std::vector<bool> wear_leveling_engaged_by_stack_;
    std::vector<double> wear_leveling_credit_by_stack_;
    std::vector<std::optional<std::uint64_t>> wear_leveling_last_move_by_block_;
    std::vector<std::uint32_t> wear_leveling_source_runs_by_block_;
    std::vector<std::optional<std::size_t>> cold_block_by_stack_;
    std::vector<std::size_t> next_gc_allocation_plane_per_stack_;

    // Base-die remap registers. Keys/values are global zone indices;
    // omitted entries are identity. PEC belongs to blocks_, never this map.
    std::map<std::uint64_t, std::uint64_t> zone_remapping_;
    bool zone_managed_ = false;
    std::vector<std::uint32_t> initial_erase_counts_;
    static constexpr std::uint32_t subarrays_per_plane_ = 1; // ordered NAND bank
    std::vector<ChannelState> channels_;
    std::vector<DieState> dies_;
    std::vector<PlaneState> planes_;
    std::vector<LogicDieState> logic_dies_;
    std::vector<BlockState> blocks_;
    std::unordered_map<std::uint64_t, std::uint64_t> lpn_to_ppn_;
    std::unordered_map<std::uint64_t, std::uint64_t> mapping_vpn_to_ppn_;
    // Only live pages and pending programs need identity/epoch records.
    // Invalid pages remain allocated in BlockState until host reclaim or a raw erase.
    std::unordered_map<std::uint64_t, PageState> programmed_pages_;
    std::optional<CompactLogicalImage> compact_logical_image_;
    // Exact, coalesced half-open LPN intervals touched by logical writes or
    // compact-page relocation.  Compact page runs may bypass the sparse
    // mutable state only when their own interval is disjoint from this set.
    std::map<std::uint64_t, std::uint64_t> mutated_lpn_ranges_;
    std::unordered_set<std::uint64_t> dirty_mapping_vpns_;
    std::unordered_map<
        std::uint64_t,
        std::set<std::pair<double, std::uint64_t>>> pending_dirty_mapping_events_;
    std::unordered_map<std::uint64_t, std::vector<PendingMappingUpdate>> pending_lpn_updates_;
    std::unordered_map<std::uint64_t, std::vector<PendingMappingUpdate>> pending_vpn_updates_;
    // Raw physical programs reserve their page immediately (like the FTL
    // allocator) but become valid only at media completion. This table keeps
    // the in-flight ownership explicit until that commit fires.
    std::unordered_map<std::uint64_t, PendingPhysicalProgram>
        pending_physical_programs_;
    std::unordered_map<std::size_t, PendingBlockTransition>
        pending_block_transitions_;
    // A targeted dependency may materialize one object's future commit
    // without advancing unrelated blocks/planes. These object-scoped
    // watermarks prevent a later call from using that state in its past.
    std::vector<double> materialized_ready_by_block_;
    // Commit-ready frontiers per key. An entry at or behind the causal
    // arrival watermark can never delay a future request, so
    // prune_expired_state() drops such entries once the tables have doubled
    // since the last sweep (amortized O(1) per request).
    std::unordered_map<std::uint64_t, double> materialized_ready_by_lpn_;
    std::unordered_map<std::uint64_t, double> materialized_ready_by_vpn_;
    std::unordered_map<std::uint64_t, double> materialized_ready_by_ppn_;
    std::size_t materialized_ready_prune_baseline_ = 0;
    std::unordered_map<std::uint64_t, std::vector<InflightBufferedWrite>> inflight_buffered_writes_;
    // Write buffer is controller DRAM: one independent partition per stack.
    // write_buffer_pages / write_buffer_flush_threshold_pages apply per stack.
    std::vector<std::list<std::uint64_t>> write_buffer_lru_by_stack_;
    std::vector<std::unordered_map<std::uint64_t, WriteBufferEntry>> write_buffer_by_stack_;
    // Controller-DRAM slot lifetime: a flushed entry's slot stays occupied until its
    // logical mapping publishes (release instants below, one per in-flight
    // generation). The old generation remains readable until that same
    // deadline; releasing at media program completion created two owners for
    // one controller-DRAM slot and a false buffer hit.
    std::vector<std::multiset<double>> write_buffer_slot_release_by_stack_;
    // One completion frontier per allocated page-read credit. Once all
    // credits are allocated, the earliest finishing transaction releases the
    // next credit. Top-level requests are required to arrive in timestamp
    // order, so this completion-order window is deterministic and exact.
    std::vector<std::multiset<double>> page_read_credit_release_by_stack_;
    // Page-granular stack-local LRU. The authoritative L2P state remains in
    // lpn_to_ppn_/compact state for semantic correctness; these structures
    // model only which persistent mapping pages are currently in DRAM.
    std::vector<std::list<std::uint64_t>> mapping_cache_lru_by_stack_;
    std::vector<std::unordered_map<std::uint64_t, MappingCacheEntry>>
        mapping_cache_by_stack_;
    std::vector<std::uint64_t> mapping_cache_bytes_by_stack_;
    // Entry-cache misses share and retain complete translation-page fills.
    // The same bounded pages provide codec scratch for the other layouts.
    std::vector<std::vector<MappingScratchPage>> mapping_scratch_by_stack_;
    // Entry-cache directory also owns one 8-byte persisted epoch per VPN.
    std::vector<double> mapping_checkpointed_through_ns_;
    // Shared-nothing FTL partition: each stack owns its planes, free pages,
    // allocation cursor, and GC; nothing crosses a stack boundary.
    // Data, Mapping, and GC have independent placement streams. Maintenance
    // fallback must not perturb the next foreground stripe decision.
    std::vector<std::size_t> next_data_allocation_plane_per_stack_;
    std::vector<std::size_t> next_mapping_allocation_plane_per_stack_;
    std::uint64_t total_pages_ = 0;
    physical::hbm::HbmDevice* buffer_hbm_ = nullptr;
    std::vector<std::uint64_t> buffer_hbm_cursor_by_stack_;
    [[nodiscard]] double host_memory_transfer(std::size_t stack, std::uint64_t bytes,
        Op op, double earliest_ns, Breakdown& breakdown, std::vector<TraceSpan>* spans);
    std::uint64_t free_pages_ = 0;
    std::vector<std::uint64_t> free_pages_per_stack_;
    // Blocks per plane whose role is neither StaticReadOnly nor RawPhysical,
    // and planes per stack holding at least one such block. Maintained by
    // set_block_role so GC headroom does not rescan every block role.
    std::vector<std::uint32_t> managed_blocks_by_plane_;
    std::vector<std::size_t> active_planes_by_stack_;
    // P/E cycles already present in a restored image. Operational erase
    // counters start at zero in the recovered process, while structural wear
    // telemetry continues from this persistent baseline.
    std::uint64_t restored_block_erases_ = 0;
    std::uint64_t mapping_table_pages_per_stack_ = 0;
    std::uint64_t mapping_table_bytes_per_stack_ = 0;
    std::uint64_t mapping_cache_pages_per_stack_ = 0;
    std::uint64_t mapping_cache_record_limit_per_stack_ = 0;
    std::map<std::pair<double, std::uint64_t>, PendingCommit> pending_commits_;
    // Per-stack ordered index of pending_commits_ keys and a sequence-to-time
    // map. Stack-selective application, "next commit on this stack", and
    // selected-sequence application then cost O(log n) each instead of a
    // linear scan of every pending commit, which reached two thirds of host
    // time under a write backlog. Every insert and erase goes through
    // insert_pending_commit()/take_pending_commit() so the three stay
    // consistent.
    std::vector<std::set<std::pair<double, std::uint64_t>>>
        pending_commit_keys_by_stack_;
    std::unordered_map<std::uint64_t, double> pending_commit_time_by_sequence_;
    void insert_pending_commit(
        double at_ns,
        std::uint64_t sequence,
        PendingCommit commit);
    [[nodiscard]] std::function<void()> take_pending_commit(
        std::map<std::pair<double, std::uint64_t>, PendingCommit>::iterator
            commit);
    std::uint64_t next_commit_sequence_ = 0;
    std::uint64_t next_cache_touch_sequence_ = 0;
    std::uint64_t next_buffer_generation_ = 0;
    std::optional<double> last_issue_arrival_ns_;
    // Lower bound on the ready time of every reservation created from the
    // current or any future top-level issue/drain call. Exact calendars may
    // reclaim idle intervals only before this causal watermark.
    double reservation_causal_watermark_ns_ = 0.0;
    // A top-level drain is a full stack barrier. Ordinary targeted commit
    // advancement uses the per-object frontiers above instead, so unrelated
    // blocks and planes do not inherit a stack-wide dependency.
    std::vector<double> causal_state_ready_by_stack_;
    // State time already made causal for the active top-level operation.
    // Temporal read-buffer state may be materialized through this instant,
    // never through arbitrary future work scheduled inside the operation.
    std::vector<double> state_observation_by_stack_;
    // Dirty mapping pages per stack (cached mode: resident dirty lines),
    // maintained with dirty_mapping_vpns_ so GC admission is O(1).
    std::vector<std::uint64_t> dirty_mapping_pages_by_stack_;
    // Logical capacity in pages over the whole device. Derived values follow
    // static/raw extent reservations and are frozen by the first logical
    // use (prepopulation, request, or image restore).
    std::uint64_t logical_capacity_pages_ = 0;
    bool logical_capacity_frozen_ = false;
    // Latest completion of issued maintenance/background work. It contributes
    // to device finish telemetry and to an explicit drain, but not to an
    // unrelated foreground request's state dependency.
    double background_finish_ns_ = 0.0;
    // One thermal node + pacing calendar per stack; empty when the thermal
    // model is disabled. Runtime-only state: a restored persistent image
    // boots at the idle steady-state temperature.
    std::vector<ThermalStackState> thermal_stacks_;
    double thermal_idle_temperature_c_ = 0.0;
    double thermal_boot_temperature_c_ = 0.0;
    double thermal_pacing_power_w_ = 0.0;
    double thermal_tau_ns_ = 0.0;
    double thermal_read_energy_j_ = 0.0;
    double thermal_program_energy_j_ = 0.0;
    double thermal_erase_energy_j_ = 0.0;
    mutable HbfStats stats_;

    [[nodiscard]] std::list<std::uint64_t>& write_buffer_lru(std::size_t stack);
    [[nodiscard]] std::unordered_map<std::uint64_t, WriteBufferEntry>& write_buffer(std::size_t stack);
    [[nodiscard]] double preview_full_plane_window(
        PlaneState& plane,
        double earliest_ns,
        double duration_ns);
    void record_full_plane_window(
        PlaneState& plane,
        double begin_ns,
        double end_ns);
    void record_plane_media_busy(PlaneState& plane, double begin_ns, double end_ns);
    // Drop per-operation state that no request arriving at or after the
    // causal watermark can observe: folded ECC latency intervals, commit
    // frontiers behind the watermark, and superseded dirty-mapping events.
    // Plane window vectors prune their dead prefix at their own insertion
    // points. Everything here is unobservable by construction, so results
    // are bit-identical with or without pruning.
    void prune_expired_state(double causal_arrival_watermark_ns);
    // Fold every retained ECC latency interval that finishes at or before
    // the watermark into the die's exact running peak concurrency.
    static void fold_ecc_inflight_intervals(
        DieState& die,
        double causal_arrival_watermark_ns);
    [[nodiscard]] static std::uint64_t ecc_max_inflight(const DieState& die);
    // Read-buffer state lives on the logic die of the stack that owns the
    // PPN (derived internally): keying by the request's ingress die would
    // strand entries that the owning block's erase can never purge.
    [[nodiscard]] bool read_buffer_contains(std::uint64_t ppn, double at_ns);
    void read_buffer_insert(std::uint64_t ppn, double ready_ns);
    void read_buffer_purge_page(std::uint64_t ppn);
    void read_buffer_purge_block(std::size_t block_index);
    [[nodiscard]] double serve_read_from_read_buffer(
        std::uint64_t ppn,
        std::uint64_t bytes,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans, bool host_buffer = false);
    [[nodiscard]] std::size_t stack_for_lpn(std::uint64_t lpn) const;
    [[nodiscard]] std::size_t stack_for_vpn(std::uint64_t mapping_vpn) const;
    [[nodiscard]] std::size_t planes_per_stack() const;
    [[nodiscard]] std::size_t stack_of_plane(std::size_t plane) const;
    [[nodiscard]] std::size_t stack_of_block(std::size_t block_index) const;
    [[nodiscard]] std::size_t mapping_plane_for_vpn(std::uint64_t mapping_vpn) const;
    [[nodiscard]] std::size_t stack_index(const HbfAddress& addr) const;
    [[nodiscard]] std::size_t channel_index(const HbfAddress& addr) const;
    [[nodiscard]] std::size_t die_index(const HbfAddress& addr) const;
    [[nodiscard]] std::size_t plane_index(const HbfAddress& addr) const;
    [[nodiscard]] std::size_t block_index(const HbfAddress& addr) const;
    [[nodiscard]] std::size_t block_plane_index(std::size_t block_index) const;
    void reserve_static_physical_blocks(
        const std::vector<std::size_t>& block_indices);
    [[nodiscard]] std::size_t subarray_index(const HbfAddress& addr) const;
    [[nodiscard]] std::size_t media_lane_index(const HbfAddress& addr) const;
    [[nodiscard]] std::size_t page_buffer_bank_index(const HbfAddress& addr) const;
    [[nodiscard]] std::uint64_t page_count_for(const HbfAddress& addr, std::uint64_t bytes) const;
    [[nodiscard]] HbfAddress decode_ppn(std::uint64_t ppn) const;
    [[nodiscard]] std::uint64_t encode_ppn(const HbfAddress& addr) const;
    [[nodiscard]] HbfAddress logical_page_address(std::uint64_t logical_byte_addr) const;
    [[nodiscard]] std::uint64_t mapping_vpn_for_lpn(std::uint64_t lpn) const;
    [[nodiscard]] std::optional<std::uint64_t> compact_lpn_ppn(
        std::uint64_t lpn) const;
    [[nodiscard]] std::optional<std::uint64_t> compact_mapping_ppn(
        std::uint64_t mapping_vpn) const;
    [[nodiscard]] std::optional<std::uint64_t> compact_lpn_for_ppn(
        std::uint64_t ppn) const;
    [[nodiscard]] std::optional<CompactPageIdentity>
        compact_page_identity(std::uint64_t ppn) const;
    void retire_compact_page(
        std::uint64_t logical_key,
        PageOwner owner);
    void prepopulate_compact_logical_page_range(
        std::uint64_t first_lpn,
        std::uint64_t page_count,
        bool mutable_image);
    [[nodiscard]] std::uint64_t logical_mapping_entry_count() const;
    [[nodiscard]] std::size_t source_index(TransactionSource source) const;
    [[nodiscard]] std::string source_name(TransactionSource source) const;
    [[nodiscard]] HeatmapTrafficSource resolve_heatmap_source(
        TransactionSource source,
        HeatmapTrafficSource attribution) const;
    [[nodiscard]] std::string kind_name(TransactionKind kind) const;
    // Bytes a raw flash-side page operation moves (data + OOB).
    [[nodiscard]] std::uint64_t page_wire_bytes() const {
        return config_.device.page_size_bytes + config_.device.oob_bytes_per_page;
    }
    [[nodiscard]] std::string role_name(BlockRole role) const;
    void flush_all_dirty_mapping_pages(
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    void mark_mapping_page_dirty(std::uint64_t mapping_vpn, double at_ns);
    void access_mapping(
        std::uint64_t lpn,
        TransactionSource source,
        MappingAccessKind kind,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    void ensure_mapping_page_cached(
        std::uint64_t mapping_vpn,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        std::optional<std::uint64_t> requested_lpn = std::nullopt,
        bool for_update = false);
    [[nodiscard]] std::uint64_t mapping_cache_key(std::uint64_t lpn) const;
    [[nodiscard]] std::uint64_t mapping_cache_record_bytes(
        std::uint64_t mapping_vpn, double at_ns, bool for_update) const;
    bool read_mapping_backing_page(
        std::uint64_t mapping_vpn, double& at_ns, Breakdown& breakdown,
        std::vector<TraceSpan>* spans, bool merge);
    void mapping_dram_access(double& at_ns, std::size_t stack,
        Breakdown& breakdown, std::vector<TraceSpan>* spans,
        const char* name, std::uint64_t& counter, bool write = false);
    MappingScratchPage* mapping_buffer_page(std::size_t stack, std::uint64_t vpn);
    MappingScratchPage* acquire_mapping_scratch(std::size_t stack, double& at_ns,
        Breakdown& breakdown, std::vector<TraceSpan>* spans);
    void merge_cached_mapping_entries(std::uint64_t vpn, double& at_ns,
        Breakdown& breakdown, std::vector<TraceSpan>* spans);
    void service_mapping_codec(std::uint64_t vpn, double& at_ns,
        Breakdown& breakdown, std::vector<TraceSpan>* spans, const char* name);
    [[nodiscard]] bool mapping_entry_dirty(const MappingCacheEntry& entry) const;
    void evict_mapping_cache_record(std::size_t stack, double& at_ns,
        Breakdown& breakdown, std::vector<TraceSpan>* spans);
    void touch_mapping_cache_entry(
        std::size_t stack,
        std::uint64_t mapping_vpn);
    void flush_dirty_mapping_page(
        std::uint64_t mapping_vpn,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    // What a logical access needs from the current mapping: the page's data
    // (reads, partial-page merges) or only ordering against earlier host
    // writes (a full-page overwrite).
    enum class LookupIntent {
        ReadData,
        OverwriteFullPage,
    };
    [[nodiscard]] std::optional<std::uint64_t> lookup_lpn(
        std::uint64_t lpn,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        LookupIntent intent);
    [[nodiscard]] std::optional<std::uint64_t> visible_lpn_at(
        std::uint64_t lpn,
        double at_ns) const;
    // Waits for earlier host writes to this LPN to publish. Relocation
    // publications are waited for only when requested: a read of a page
    // whose relocation is still in flight may use the intact old copy while
    // the victim has not been reclaimed.
    void wait_for_prior_lpn_commit(
        std::uint64_t lpn,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        bool wait_for_relocations);
    [[nodiscard]] bool current_ppn_has_pending_transition(std::uint64_t lpn) const;
    // A raw (host-issued) erase destroys the data the mapping still names;
    // waits for its completion. GC reclaims are never waited for here.
    void wait_for_pending_block_erase(
        std::uint64_t ppn,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    void wait_for_lpn_dependencies(
        std::uint64_t lpn,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        LookupIntent intent);
    void wait_for_pending_vpn_erase(
        std::uint64_t mapping_vpn,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    // Logical capacity: derive (config 0) or validate the configured value
    // against the current static/raw reservations, then freeze it.
    [[nodiscard]] std::uint64_t derive_logical_capacity_pages() const;
    void resolve_logical_capacity();
    void require_logical_capacity(std::uint64_t last_lpn);
    void add_pending_block_transition_commits(
        std::size_t block_index,
        std::unordered_set<std::uint64_t>& sequences) const;
    void tag_pending_mapping_updates_for_erase(
        std::size_t block_index,
        std::uint64_t retired_block_epoch,
        double ready_ns);
    void retire_mapping_update_tombstones(
        std::size_t block_index,
        double ready_ns);
    [[nodiscard]] std::optional<std::uint64_t> visible_mapping_vpn_at(
        std::uint64_t mapping_vpn,
        double at_ns) const;
    void schedule_lpn_mapping_commit(
        std::uint64_t lpn,
        std::uint64_t new_ppn,
        double commit_ns,
        std::uint64_t program_commit_sequence,
        std::optional<std::uint64_t> expected_old_ppn = std::nullopt);
    void schedule_vpn_mapping_commit(
        std::uint64_t mapping_vpn,
        std::uint64_t new_ppn,
        double commit_ns,
        std::uint64_t program_commit_sequence,
        std::optional<std::uint64_t> expected_old_ppn = std::nullopt);
    void schedule_physical_program_commit(
        std::uint64_t ppn,
        double commit_ns);
    void reserve_physical_program_range(
        std::uint64_t first_ppn,
        std::uint64_t pages);
    [[nodiscard]] std::uint64_t schedule_commit(
        std::size_t stack,
        double at_ns,
        std::function<void()> action);
    void apply_selected_commits_through(
        const std::unordered_set<std::uint64_t>& sequences,
        double at_ns);
    void apply_commits_through(double at_ns);
    void apply_stack_commits_through(std::size_t stack, double at_ns);
    void apply_all_commits();
    [[nodiscard]] bool advance_to_next_commit(double& at_ns, std::size_t stack);
    [[nodiscard]] bool write_buffer_covers(
        const WriteBufferEntry& entry,
        std::uint64_t begin,
        std::uint64_t end) const;
    [[nodiscard]] bool dirty_ranges_cover(
        const std::vector<DirtyRange>& ranges,
        std::uint64_t begin,
        std::uint64_t end) const;
    [[nodiscard]] bool write_buffer_full_page(const WriteBufferEntry& entry) const;
    [[nodiscard]] std::uint64_t write_buffer_covered_bytes(const WriteBufferEntry& entry) const;
    [[nodiscard]] double serve_read_from_write_buffer(
        std::uint64_t lpn,
        std::uint64_t begin,
        std::uint64_t end,
        const std::vector<DirtyRange>& ranges,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    [[nodiscard]] double admit_foreground_page_read(
        std::size_t stack,
        double offered_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    void complete_foreground_page_read(
        std::size_t stack,
        double finish_ns);
    [[nodiscard]] double schedule_external_write_ingress(
        std::size_t stack,
        std::uint64_t bytes,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        const std::string& detail,
        const std::string& sram_span_name);
    [[nodiscard]] double schedule_external_request_command(
        std::size_t stack,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        const std::string& detail, std::string_view source = "user");
    [[nodiscard]] double schedule_write_buffer_dram_access(
        std::size_t stack,
        std::uint64_t bytes,
        bool write,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        const std::string& name,
        const std::string& detail);
    [[nodiscard]] double schedule_external_read_egress(
        std::size_t stack,
        std::uint64_t bytes,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        const std::string& name,
        const std::string& detail);
    // Write-buffer staging and flush act on the LPN's owning stack (derived
    // internally, like the read buffer above).
    [[nodiscard]] double stage_write_buffer_range(
        std::uint64_t lpn,
        std::uint64_t begin,
        std::uint64_t end,
        HeatmapTrafficSource heatmap_source,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    void flush_write_buffer_entry(
        std::uint64_t lpn,
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    void flush_all_write_buffer_entries(
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    [[nodiscard]] std::uint64_t allocate_free_page(
        double& at_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        BlockRole role,
        std::size_t stack,
        std::optional<std::size_t> preferred_plane = std::nullopt);
    [[nodiscard]] std::size_t choose_allocation_plane(
        std::size_t stack,
        BlockRole role);
    [[nodiscard]] std::size_t allocate_block_from_plane(std::size_t plane_index, BlockRole role);
    [[nodiscard]] std::optional<std::size_t>& active_block_ref(PlaneState& plane, BlockRole role);
    [[nodiscard]] std::uint64_t allocate_page_from_block(std::size_t block_index);
    [[nodiscard]] std::uint64_t allocate_compact_pages_on_plane(
        std::size_t plane_index,
        BlockRole role,
        std::uint32_t page_count,
        std::vector<std::uint64_t>* assigned_blocks = nullptr,
        std::unordered_map<std::uint64_t, std::uint32_t>*
            live_pages_by_block = nullptr);
    void set_mapping_page_dirty(std::uint64_t mapping_vpn);
    void clear_mapping_page_dirty(std::uint64_t mapping_vpn);
    [[nodiscard]] std::size_t active_planes_in_stack(std::size_t stack) const;
    void invalidate_ppn(std::uint64_t ppn);
    void invalidate_live_page(std::uint64_t ppn);
    void mark_programmed(
        std::uint64_t ppn,
        std::uint64_t lpn,
        PageOwner owner = PageOwner::Logical);
    [[nodiscard]] std::uint64_t schedule_media_program_commit(
        std::uint64_t ppn,
        std::uint64_t lpn,
        PageOwner owner,
        double commit_ns);
    void release_invalid_block(std::size_t block_index);
    void refresh_accounting_stats() const;
    [[nodiscard]] ScheduledTransfer reserve(
        double earliest_ns,
        double duration_ns,
        ResourceTimeline& timeline);
    [[nodiscard]] ScheduledTransfer schedule_flash_transaction(
        const HbfAddress& addr,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        TransactionSource source,
        TransactionKind kind);
    [[nodiscard]] double schedule_command_path(
        const HbfAddress& addr,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        TransactionSource source, bool host_command = true);
    [[nodiscard]] double schedule_ecc(
        const HbfAddress& addr,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        TransactionSource source,
        bool decode);
    [[nodiscard]] double schedule_read_page(
        std::uint64_t ppn,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        TransactionSource source,
        HeatmapTrafficSource heatmap_attribution,
        ReadPayloadRoute route,
        std::uint64_t external_payload_bytes,
        double* decoded_ready_ns = nullptr,
        bool thermally_preadmitted = false);

    [[nodiscard]] std::vector<PageRunSpan> static_page_run_spans(
        std::uint64_t first_source_page,
        std::uint64_t pages) const;
    [[nodiscard]] std::uint64_t static_ppn_for_source_page(
        std::uint64_t source_page) const;
    void record_mutated_lpn_range(
        std::uint64_t first_lpn,
        std::uint64_t pages);
    [[nodiscard]] bool mutated_lpn_range_overlaps(
        std::uint64_t first_lpn,
        std::uint64_t pages) const;
    [[nodiscard]] double schedule_program_page(
        std::uint64_t ppn,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        TransactionSource source,
        HeatmapTrafficSource heatmap_attribution,
        double* source_consumed_ns = nullptr);
    [[nodiscard]] double schedule_erase_block(
        std::size_t block_index,
        double earliest_ns,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans,
        TransactionSource source,
        HeatmapTrafficSource heatmap_attribution, bool auto_erase = false);
    // Integrate one thermal node forward to t_ns: decay toward the idle
    // asymptote with the analytic release crossing of the hysteresis.
    // record=false is used on copies for stats projection and must not
    // touch telemetry.
    void thermal_advance(
        ThermalNodeState& node,
        double t_ns,
        bool record) const;
    // Pacing decision only (no heat): while the stack is throttled, delay
    // the work until a free energy/pacing-power slot in the stack calendar.
    // media_ops sizes the paced telemetry. Callers that
    // pace a whole scalar request account buffered pages conservatively
    // here while depositing heat per actual media page.
    [[nodiscard]] ThermalAdmission thermal_pace_media(
        std::size_t stack,
        double earliest_ns,
        double energy_j,
        std::uint64_t media_ops,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    // Deposit media energy at at_ns: raises the node immediately, records
    // the peak, and runs the engage transition. Exact media accounting:
    // thermal_media_energy_j grows only here.
    void thermal_deposit_media(
        std::size_t stack,
        double at_ns,
        double energy_j);
    // Combined pace + deposit for single operations (programs, erases,
    // internal reads) and page runs, where every counted page is media.
    [[nodiscard]] ThermalAdmission thermal_admit_media(
        std::size_t stack,
        double earliest_ns,
        double energy_j,
        std::uint64_t media_ops,
        Breakdown& breakdown,
        std::vector<TraceSpan>* spans);
    void refresh_parallel_stats() const;
};

} // namespace hbfsim::host
