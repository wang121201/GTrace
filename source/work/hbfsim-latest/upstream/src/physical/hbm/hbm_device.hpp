#pragma once
// INDEPENDENT_PC_BEGIN test-configuration
#ifndef TILEGEN_HBF_INDEPENDENT_PC_TEST
#define TILEGEN_HBF_INDEPENDENT_PC_TEST 0
#endif
// INDEPENDENT_PC_END test-configuration

#include "physical/physical_types.hpp"
#include "physical/hbm/hbm_config.hpp"
#include "physical/gap_calendar.hpp"
#include "physical/resource_calendar.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hbfsim::physical::hbm {

// Access commands the controller issues. Refresh is a bank-set reservation
// (REFab/REFsb) resolved by the scheduler, not an entry of this enum.
enum class HbmCommand {
    ACT,
    PRE,
    RD,
    WR,
};

inline constexpr std::size_t kHbmCommandCount = 4;

struct HbmConfig {
    // Versioned controller address map (see docs/reference/model.md). The
    // scheme id and the interleave size are recorded in every summary.
    static constexpr std::string_view address_mapping_scheme() {
        return "pch-interleave-bg-rotate-v2";
    }

    static constexpr auto standard = kStandard;
    static constexpr std::uint64_t kDefaultInterleaveBytes = 256;
    HbmDeviceConfig device;
    HbmTimingConfig timing;
    HbmControllerConfig controller;

    [[nodiscard]] std::uint64_t pseudo_channel_width_bits() const;
    [[nodiscard]] std::uint64_t row_size_bytes() const;
    [[nodiscard]] std::uint64_t burst_bytes() const;
    // The interleave the map actually uses: interleave_bytes when it is
    // explicit and legal (throws otherwise), else the auto default above.
    [[nodiscard]] std::uint64_t effective_interleave_bytes() const;
    [[nodiscard]] double channel_bandwidth_GBps() const;
    [[nodiscard]] double pseudo_channel_bandwidth_GBps() const;
    [[nodiscard]] double command_clock_period_ns() const;
    [[nodiscard]] double command_clock_MHz() const;
    [[nodiscard]] double burst_duration_ns() const;
    [[nodiscard]] double tCCD_S_ns() const;
    [[nodiscard]] double tCCD_L_ns() const;
    [[nodiscard]] std::uint64_t command_clock_cycles(double time_ns) const;
    [[nodiscard]] double command_clock_time_ns(std::uint64_t cycles) const;
    [[nodiscard]] double command_aligned_time_ns(double time_ns) const;
};

struct HbmAddress {
    std::uint32_t stack = 0;
    std::uint32_t channel = 0;
    std::uint32_t pseudo_channel = 0;
    std::uint32_t bank_group = 0;
    std::uint32_t bank = 0;
    std::uint64_t row = 0;
    std::uint64_t offset = 0;

    [[nodiscard]] std::string path() const;
};

struct HbmStats {
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    // Included in the totals above. Controller-buffer traffic uses shared
    // data-channel reservations; ACT/PRE and row-hit counters cover only
    // command-engine requests, not this explicit channel-level abstraction.
    std::uint64_t controller_buffer_read_bytes = 0;
    std::uint64_t controller_buffer_write_bytes = 0;
    std::uint64_t controller_buffer_transfers = 0;
    double controller_buffer_bus_busy_ns = 0.0;
    std::uint64_t row_hits = 0;
    std::uint64_t row_misses = 0;
    std::uint64_t row_conflicts = 0;
    std::uint64_t activations = 0;
    std::uint64_t precharges = 0;
    // Refresh commands issued (REFab: one per tREFI per pseudo-channel;
    // REFsb: banks_per_group per tREFI per pseudo-channel).
    std::uint64_t refresh_count = 0;
    // Exact: derived from an integer count of data-burst cycles.
    double bus_busy_ns = 0.0;
    double first_arrival_ns = std::numeric_limits<double>::infinity();
    double finish_ns = 0.0;
    std::uint64_t pseudo_channels = 0;
    std::uint64_t active_pseudo_channels = 0;
    std::uint64_t max_pseudo_channel_accesses = 0;
    std::uint64_t max_queue_occupancy = 0;
    double max_pseudo_channel_busy_ns = 0.0;
    double avg_active_pseudo_channel_busy_ns = 0.0;
    // Symmetric replication receipts: parent requests whose admission copied
    // one representative pseudo-channel to identical peers, and burst children
    // whose completion was produced by such a copy instead of being simulated.
    std::uint64_t replicated_requests = 0;
    std::uint64_t replicated_bursts = 0;
    // Sum of every physical burst child's stage work. Children can overlap
    // across pseudo-channels, so this is intentionally not wall time.
    Breakdown stage_work;

    [[nodiscard]] double row_hit_rate() const;
    [[nodiscard]] double active_span_ns() const;
    [[nodiscard]] double utilization() const;
    [[nodiscard]] double bus_parallelism() const;
    [[nodiscard]] double pseudo_channel_busy_skew() const;
};

class HbmDevice {
public:
    explicit HbmDevice(
        HbmConfig config,
        AddressHeatmap* address_heatmap = nullptr);

    [[nodiscard]] HbmAddress decode(std::uint64_t addr) const;
    [[nodiscard]] std::uint64_t encode(const HbmAddress& addr) const;
    // Synchronous request: enqueue + pump in one call.
    [[nodiscard]] PhysicalCompletion issue(const PhysicalRequest& request);
    // Reserve the top of physical HBM for host-controller storage. Workload
    // addresses must stay below application_capacity_bytes(). No extra HBM
    // device, capacity, or independent bandwidth pool is created.
    void reserve_controller_buffer(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t controller_buffer_bytes() const { return controller_buffer_bytes_; }
    [[nodiscard]] std::uint64_t application_capacity_bytes() const {
        return config_.device.capacity_bytes - controller_buffer_bytes_;
    }
    [[nodiscard]] PhysicalCompletion transfer_controller_buffer(const PhysicalRequest& request);
    // Advance only at a joint system frontier, never to a speculative HBF
    // completion. An earlier application request can fill a future DMA gap.
    void advance_buffer_frontier(double at_ns);
    [[nodiscard]] std::uint64_t enqueue(const PhysicalRequest& request);
    [[nodiscard]] PhysicalCompletion pump(std::uint64_t ticket);
    [[nodiscard]] bool service_before(double arrival_ns);
// INDEPENDENT_PC_BEGIN public-api
    struct IndependentDrainResult {
        bool used_independent = false;
        // These are different units. The inactive path's field stays zero.
        std::uint64_t pc_service_one_calls = 0;
        std::uint64_t legacy_global_rounds = 0;
    };
    // No external observation/admission may interleave this synchronous call.
    // Normal finite execution only: per-PC command order and boundary outcomes
    // are preserved; global round count and additive stage_work order are not.
    // Unsupported states use the complete original global drain loop.
    [[nodiscard]] IndependentDrainResult drain_independent_before(double boundary_ns);
#if TILEGEN_HBF_INDEPENDENT_PC_TEST
    struct IndependentCommandRecord {
        std::size_t pc;
        std::uint64_t issue_cycle;
        std::optional<std::size_t> request_index;
        std::optional<std::size_t> precharge_bank;
        HbmCommand command;
        std::uint64_t ticket;
        bool operator==(const IndependentCommandRecord&) const = default;
    };
    [[nodiscard]] const auto& independent_test_commands() const { return independent_test_commands_; }
#endif
// INDEPENDENT_PC_END public-api
    [[nodiscard]] std::vector<std::pair<std::uint64_t, PhysicalCompletion>>
        take_completions();
    void drain_queues();
    [[nodiscard]] const HbmStats& stats() const {
        refresh_parallel_stats();
        return stats_;
    }
    // O(1) cumulative counters for persistent simulation-session batch deltas.
    // Geometry-wide parallelism fields are refreshed only by stats().
    [[nodiscard]] HbmStats execution_stats() const { return stats_; }
    [[nodiscard]] const HbmConfig& config() const { return config_; }
    void attach_address_heatmap(AddressHeatmap& address_heatmap) {
        if (last_enqueue_arrival_ns_) {
            throw std::runtime_error(
                "HBM address heatmap must be attached before the first request");
        }
        address_heatmap_ = &address_heatmap;
    }

private:
// INDEPENDENT_PC_BEGIN state
    // A successful admission can permanently disable the independent path.
    // Default value-copy/assignment copies this conservative history latch;
    // draining or taking completions never resets it.
    bool independent_trace_safe_ = true;
#if TILEGEN_HBF_INDEPENDENT_PC_TEST
    friend struct IndependentPcTestAccess;
    std::vector<IndependentCommandRecord> independent_test_commands_;
#endif
// INDEPENDENT_PC_END state
    // One gate per access command plus the refresh gate: the earliest cycle
    // at which a refresh may be issued to a bank that is already closed
    // (tRP after its PRE, or the end of its previous refresh window).
    static constexpr std::size_t kRefreshGate = kHbmCommandCount;
    using CommandGates = std::array<std::uint64_t, kHbmCommandCount + 1>;

    struct BankState {
        bool has_open_row = false;
        std::uint64_t open_row = 0;
        std::optional<std::size_t> active_request;
        std::uint64_t refresh_start_cycle = 0;
        std::uint64_t refresh_until_cycle = 0;
        // Earliest command-clock cycle at which each command may issue.
        CommandGates ready{};
    };

    struct BankGroupState {
        CommandGates ready{};
    };

    struct CommandProgress {
        PhysicalCompletion completion;
        std::optional<std::uint64_t> first_issue;
        std::uint64_t ready_cycle = 0;
        bool precharged = false;
        bool activated = false;
    };

    struct QueuedRequest {
        std::uint64_t ticket = 0;
        Op op = Op::Read;
        double arrival_ns = 0.0;
        // First command-clock cycle at which a command may issue
        // (arrival plus address mapping, rounded to the clock grid).
        std::uint64_t ready_cycle = 0;
        std::uint64_t bytes = 0;
        HbmAddress addr;
        TraceConfig trace;
        std::uint32_t bypass_count = 0;
        std::optional<CommandProgress> progress;
    };

    struct PendingRequest {
        PhysicalCompletion completion;
        std::size_t remaining_children = 0;
        std::size_t total_children = 0;
        std::size_t pseudo_channels = 0;
        // Pseudo-channel of the child currently on the critical path; ties on
        // finish time resolve to the lowest index so aggregation order never
        // changes the reported breakdown.
        std::size_t critical_pseudo_channel = 0;
        bool enqueue_complete = false;
        bool has_child_completion = false;
        bool retain_diagnostics = true;
    };

    struct PseudoChannelCommandState {
        std::vector<BankState> banks;
        std::vector<BankGroupState> bank_groups;
        std::uint32_t open_rows = 0;
        std::uint64_t service_floor_cycle = 0;
        std::uint64_t row_command_ready_cycle = 0;
        CommandGates ready{};
        std::vector<std::uint64_t> activations;
        // Position in the refresh schedule: the next unresolved refresh
        // command is command `refresh_command` of period `refresh_period`.
        std::uint64_t refresh_period = 0;
        std::uint32_t refresh_command = 0;
        bool has_refresh_issue = false;
        std::uint64_t last_refresh_issue = 0;
        // FR-FCFS pending queue, kept in arrival order.
        std::vector<QueuedRequest> queue;
    };

    struct PseudoChannelState : PseudoChannelCommandState {
        // Buffer traffic may arrive behind the command scheduler's floor.
        // Keep each channel's actual bus history separate from replicated
        // command state. Times are exact integer command-clock cycles.
        GapCalendar data_bus_cycles;
    };

    struct LocalAddress {
        std::uint32_t bank_group = 0;
        std::uint32_t bank = 0;
        std::uint64_t row = 0;
        std::uint64_t column_unit = 0;
    };

    struct FirstCommand {
        std::uint64_t issue_cycle = std::numeric_limits<std::uint64_t>::max();
        HbmCommand command = HbmCommand::ACT;
        bool row_hit = false;
    };

    struct ScheduledEvent {
        std::uint64_t issue_cycle = std::numeric_limits<std::uint64_t>::max();
        std::optional<std::size_t> request_index;
        std::optional<std::size_t> precharge_bank;
        HbmCommand command = HbmCommand::ACT;
    };

    // Per-ticket outcome of a representative pseudo-channel, replayed onto
    // its symmetric peers.
    struct ChildRecord {
        std::uint64_t ticket = 0;
        std::uint64_t count = 0;
        std::uint64_t physical_bytes = 0;
        double min_start_ns = std::numeric_limits<double>::infinity();
        double max_finish_ns = -std::numeric_limits<double>::infinity();
        Breakdown critical;
        Breakdown work;
    };

    // Counter deltas produced by a representative while it was recorded.
    struct CounterDelta {
        std::uint64_t read_bytes = 0;
        std::uint64_t write_bytes = 0;
        std::uint64_t row_hits = 0;
        std::uint64_t row_misses = 0;
        std::uint64_t row_conflicts = 0;
        std::uint64_t activations = 0;
        std::uint64_t precharges = 0;
        std::uint64_t refresh_count = 0;
        std::uint64_t bus_busy_cycles = 0;
        std::uint64_t accesses = 0;
        std::uint64_t busy_cycles = 0;
    };

    struct Recording {
        bool active = false;
        std::size_t representative = 0;
        std::uint64_t push_ticket = 0;
        std::uint64_t pushed = 0;
        // Deltas folded so far plus the counter snapshot taken when the
        // recording was last (re)started; a paused recording folds first so
        // work done for other classes in between is never attributed to it.
        CounterDelta delta;
        CounterDelta snapshot;
        std::vector<ChildRecord> records;
        std::vector<GapCalendar::Gap> data_bus_reservations;
    };

    struct SymmetryClass {
        std::size_t representative = 0;
        std::vector<std::size_t> members;
    };

    // Between external admissions, each class continues from its representative.
    // Counters and completions are replayed immediately; peer controller state
    // is materialized only before another path can inspect or mutate it.
    std::vector<SymmetryClass> service_classes_;

    HbmConfig config_;
    AddressHeatmap* address_heatmap_ = nullptr;
    // Immutable geometry derived and validated once at construction.
    std::uint64_t row_size_bytes_ = 0;
    std::uint64_t burst_bytes_ = 0;
    std::uint64_t units_per_row_ = 0;
    std::uint64_t stripe_bytes_ = 0;
    std::uint64_t total_pseudo_channels_ = 0;
    std::uint64_t banks_per_pseudo_channel_ = 0;
    std::uint64_t pseudo_channels_per_stack_ = 0;
    double command_clock_period_ns_ = 0.0;
    // Every controller timing in command-clock cycles.
    std::uint64_t burst_cycles_ = 0;
    std::uint64_t tccd_s_ = 0;
    std::uint64_t tccd_l_ = 0;
    std::uint64_t trcdrd_ = 0;
    std::uint64_t trcdwr_ = 0;
    std::uint64_t tcl_ = 0;
    std::uint64_t tcwl_ = 0;
    std::uint64_t trp_ = 0;
    std::uint64_t tras_ = 0;
    std::uint64_t trc_ = 0;
    std::uint64_t twr_ = 0;
    std::uint64_t trtp_ = 0;
    std::uint64_t trrd_s_ = 0;
    std::uint64_t trrd_l_ = 0;
    std::uint64_t tfaw_ = 0;
    std::uint64_t twtr_s_ = 0;
    std::uint64_t twtr_l_ = 0;
    std::uint64_t trtw_ = 0;
    std::uint64_t trefi_ = 0;
    std::uint64_t trfc_ = 0;
    std::uint64_t trfcsb_ = 0;
    std::uint64_t trrefd_ = 0;
    std::uint32_t refresh_commands_per_period_ = 1;
    std::vector<PseudoChannelState> pseudo_channels_;
    // Cumulative accounting is deliberately separate from controller state:
    // it never affects scheduling.
    std::vector<std::uint64_t> pseudo_channel_accesses_;
    std::vector<std::uint64_t> pseudo_channel_bus_busy_cycles_;
    std::uint64_t bus_busy_cycles_ = 0;
    mutable HbmStats stats_;
    std::uint64_t next_ticket_ = 0;
    std::uint64_t controller_buffer_bytes_ = 0;
    std::uint64_t buffer_frontier_cycle_ = 0;
    // Reused DMA grouping storage. Only touched channels are cleared, so
    // small mapping accesses do not scan or allocate for the whole device.
    std::vector<std::uint64_t> buffer_burst_counts_;
    std::vector<std::size_t> buffer_channels_;
    // Per-parent visitation marker used to build its unique pseudo-channel
    // route list in O(children).
    std::vector<std::uint64_t> route_ticket_markers_;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> ticket_pseudo_channels_;
    std::unordered_map<std::uint64_t, PendingRequest> pending_;
    std::unordered_map<std::uint64_t, PhysicalCompletion> completed_;
    std::optional<double> last_enqueue_arrival_ns_;
    // Command floor of the most recent enqueue: no later request can issue
    // before it, so expired timing state can be canonicalized against it.
    std::uint64_t floor_bound_cycle_ = 0;
    Recording recording_;

    [[nodiscard]] std::size_t pseudo_channel_index(const HbmAddress& addr) const;
    [[nodiscard]] std::size_t bank_index(const HbmAddress& addr) const;
    [[nodiscard]] LocalAddress local_address(std::uint64_t unit) const;
    void assign_pseudo_channel(HbmAddress& addr, std::size_t pseudo_channel) const;
    [[nodiscard]] std::uint64_t byte_address(
        std::size_t pseudo_channel,
        std::uint64_t unit,
        std::uint64_t unit_offset) const;
    [[nodiscard]] double ns(std::uint64_t cycles) const;
    void validate_and_begin_request(const PhysicalRequest& request);
    void create_parent(std::uint64_t ticket, const PhysicalRequest& request);
    void push_child(
        std::size_t pseudo_channel_index,
        std::uint64_t ticket,
        const PhysicalRequest& request,
        std::uint64_t ready_cycle,
        std::uint64_t bytes,
        HbmAddress addr);
    void enqueue_children(
        std::uint64_t ticket,
        const PhysicalRequest& request,
        std::uint64_t ready_cycle);
    void enqueue_replicated(
        std::uint64_t ticket,
        const PhysicalRequest& request,
        std::uint64_t ready_cycle);
    void service_until_complete(std::uint64_t ticket);
    void aggregate_child(
        std::uint64_t ticket,
        std::size_t pseudo_channel_index,
        PhysicalCompletion child);
    void complete_parent(std::uint64_t ticket, PendingRequest& parent);
    [[nodiscard]] std::size_t pick_next(const PseudoChannelState& pseudo_channel) const;
    [[nodiscard]] FirstCommand first_command(
        const PseudoChannelState& pseudo_channel,
        std::size_t request_index) const;
    [[nodiscard]] ScheduledEvent next_event(const PseudoChannelState& pseudo_channel) const;
    void service_one(
        std::size_t pseudo_channel_index,
        double boundary_ns = std::numeric_limits<double>::infinity());
    [[nodiscard]] CommandProgress& command_progress(QueuedRequest& queued);
    [[nodiscard]] PhysicalCompletion service_command(
        PseudoChannelState& pseudo_channel,
        QueuedRequest& queued,
        const FirstCommand& command);
    [[nodiscard]] std::uint64_t place_activation(
        const PseudoChannelState& pseudo_channel,
        std::uint64_t lower) const;
    [[nodiscard]] std::uint64_t place_column(
        const PseudoChannelState& pseudo_channel,
        bool write,
        std::uint64_t lower) const;
    void prune_placements(PseudoChannelState& pseudo_channel, std::uint64_t floor) const;
    [[nodiscard]] std::uint64_t refresh_nominal_cycle(
        std::uint64_t period,
        std::uint32_t command) const;
    [[nodiscard]] bool refresh_targets_bank(
        std::uint32_t command,
        std::size_t bank_index) const;
    void resolve_next_refresh(
        PseudoChannelState& pseudo_channel,
        std::size_t pseudo_channel_index,
        std::uint64_t issue_cycle,
        std::vector<TraceSpan>* spans);
    [[nodiscard]] bool skip_idle_refreshes(
        PseudoChannelState& pseudo_channel,
        double boundary_ns);
    void apply_command_state(
        PseudoChannelState& pseudo_channel,
        const HbmAddress& addr,
        HbmCommand command,
        std::uint64_t issue_cycle);
    // Symmetric replication.
    [[nodiscard]] bool replicable(const PhysicalRequest& request) const;
    [[nodiscard]] std::uint64_t normalization_floor(
        const PseudoChannelState& pseudo_channel) const;
    // A timing gate at or below the floor can no longer delay any command,
    // so it compares equal to an idle gate; anything later must match
    // exactly. Both the class hash and the exact comparison use this.
    [[nodiscard]] static std::uint64_t normalized_gate(
        std::uint64_t gate,
        std::uint64_t floor);
    [[nodiscard]] std::uint64_t symmetry_hash(std::size_t pseudo_channel_index) const;
    [[nodiscard]] bool symmetric(std::size_t lhs, std::size_t rhs) const;
    [[nodiscard]] std::vector<SymmetryClass> partition_symmetric(
        const std::vector<std::size_t>& candidates) const;
    [[nodiscard]] CounterDelta counter_snapshot(std::size_t representative) const;
    void begin_recording(std::size_t representative, std::uint64_t push_ticket);
    void fold_recording();
    [[nodiscard]] Recording pause_recording();
    void resume_recording(Recording recording);
    void replay_recording(const SymmetryClass& symmetry_class);
    void synchronize_class(const SymmetryClass& symmetry_class);
    void flush_service_classes();
    void replicate_recording(const SymmetryClass& symmetry_class);
    void refresh_parallel_stats() const;
};

} // namespace hbfsim::physical::hbm
