#include "cycle.h"
#ifndef BULK_COPY_H
#define BULK_COPY_H

#include "dag_node.h"
#include "memory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace GTSim {

// BulkCopyMechanism names the hardware mechanism represented by the shared
// per-SM bulk-copy engine. Hopper TMA and Ada LDGSTS/cp.async are deliberately
// distinct contracts even though both eventually admit cache-line keys to L2.
enum class BulkCopyMechanism {
    HOPPER_TMA,
    ADA_LDGSTS,
};

inline const char* bulk_copy_mechanism_name(BulkCopyMechanism mechanism) {
    switch (mechanism) {
        case BulkCopyMechanism::HOPPER_TMA: return "HOPPER_TMA";
        case BulkCopyMechanism::ADA_LDGSTS: return "ADA_LDGSTS";
    }
    return "UNKNOWN";
}

// Exact rational bytes/cycle. A numerator of 4,537,170,818 and denominator
// of 1,000,000,000 represents 4.537170818 B/cycle without floating-point drift.
struct BulkCopyRate {
    std::uint64_t numerator_bytes = 0;
    std::uint64_t denominator_cycles = 0;

    bool valid() const {
        return numerator_bytes > 0 && denominator_cycles > 0;
    }
};

struct AdaLdgstsContractDescriptor {
    const char* mechanism_name;
    const char* oracle_identity;
    BulkCopyRate single_warp_rate;
    BulkCopyRate aggregate_sm_rate;
    bool dram_to_sram_only;
    bool setup_latency_admitted;
    bool command_issue_interval_admitted;
    bool sector_behavior_admitted;
    bool runnable_profile;
};

inline AdaLdgstsContractDescriptor
make_rtx4000_ada_ldgsts_contract_descriptor() {
    return {
        "ADA_LDGSTS",
        "R77_DIRECT_ORACLE_EAD011F8679792EB",
        {4537170818ULL, 1000000000ULL},
        {11704224357ULL, 1000000000ULL},
        true,
        false,
        false,
        false,
        false,
    };
}

struct BulkCopyEngineConfig {
    BulkCopyMechanism mechanism = BulkCopyMechanism::HOPPER_TMA;
    int setup_latency_cycles = 0;
    int issue_interval_cycles = 1;
    int legacy_issue_rate_bytes_per_cycle = 1;
    BulkCopyRate per_source_rate{};
    BulkCopyRate aggregate_sm_rate{};

    static BulkCopyEngineConfig hopper_tma(int setup_latency,
                                            int issue_interval,
                                            int issue_rate_bytes_per_cycle) {
        if (setup_latency < 0 || issue_interval < 0 ||
            issue_rate_bytes_per_cycle <= 0) {
            throw std::invalid_argument("invalid Hopper TMA engine configuration");
        }
        BulkCopyEngineConfig config;
        config.mechanism = BulkCopyMechanism::HOPPER_TMA;
        config.setup_latency_cycles = setup_latency;
        config.issue_interval_cycles = issue_interval;
        config.legacy_issue_rate_bytes_per_cycle = issue_rate_bytes_per_cycle;
        return config;
    }

    // This factory configures the engine mechanism only. Supplying setup and
    // command-issue values here does not admit an RTX 4000 Ada timing profile;
    // SimulatorConfig keeps that profile fail-closed until those values and
    // sector behavior are independently admitted.
    static BulkCopyEngineConfig ada_ldgsts(int setup_latency,
                                            int issue_interval,
                                            BulkCopyRate per_source,
                                            BulkCopyRate aggregate_sm) {
        if (setup_latency < 0 || issue_interval < 0 ||
            !per_source.valid() || !aggregate_sm.valid()) {
            throw std::invalid_argument("invalid Ada LDGSTS engine configuration");
        }
        const long double source_rate =
            static_cast<long double>(per_source.numerator_bytes) /
            static_cast<long double>(per_source.denominator_cycles);
        const long double sm_rate =
            static_cast<long double>(aggregate_sm.numerator_bytes) /
            static_cast<long double>(aggregate_sm.denominator_cycles);
        if (sm_rate < source_rate) {
            throw std::invalid_argument(
                "Ada LDGSTS aggregate SM rate is below the per-source rate");
        }
        BulkCopyEngineConfig config;
        config.mechanism = BulkCopyMechanism::ADA_LDGSTS;
        config.setup_latency_cycles = setup_latency;
        config.issue_interval_cycles = issue_interval;
        config.legacy_issue_rate_bytes_per_cycle = 0;
        config.per_source_rate = per_source;
        config.aggregate_sm_rate = aggregate_sm;
        return config;
    }
};

// Convert a semantic/Hopper-shaped DRAM-to-SRAM bulk-copy node to the explicit
// Ada load mechanism. Ada LDGSTS is load-only; a TMA store must not be
// relabeled as LDGSTS.
inline void lower_dram_to_sram_copy_to_ada_ldgsts(DAGNode& node) {
    if (node.op_type != OpType::CP_DRAM2SRAM_TMA &&
        node.op_type != OpType::CP_DRAM2SRAM) {
        throw std::logic_error(
            "Ada LDGSTS lowering only accepts DRAM-to-SRAM copy nodes");
    }
    node.op = "cp.dram2sram_ldgsts";
    node.op_type = OpType::CP_DRAM2SRAM_LDGSTS;
}

struct AdaLdgstsGraphLoweringSummary {
    std::size_t dag_nodes = 0;
    std::size_t lowered_load_nodes = 0;
    std::size_t already_lowered_load_nodes = 0;
    std::size_t unresolved_tma_store_nodes = 0;

    bool complete_for_ada_execution() const {
        return unresolved_tma_store_nodes == 0;
    }
};

// Architecture lowering is explicit and inspectable. Loads become Ada LDGSTS;
// Hopper TMA stores remain unchanged and make the summary non-runnable until a
// separately admitted Ada store mechanism exists.
inline AdaLdgstsGraphLoweringSummary
lower_dag_bulk_copies_to_ada_ldgsts(DAG& dag) {
    AdaLdgstsGraphLoweringSummary summary;
    summary.dag_nodes = dag.nodes.size();
    for (DAGNode* node : dag.nodes) {
        if (node == nullptr) {
            throw std::logic_error("Ada LDGSTS lowering encountered a null DAG node");
        }
        if (node->op_type == OpType::CP_DRAM2SRAM_TMA ||
            node->op_type == OpType::CP_DRAM2SRAM) {
            lower_dram_to_sram_copy_to_ada_ldgsts(*node);
            ++summary.lowered_load_nodes;
        } else if (node->op_type == OpType::CP_DRAM2SRAM_LDGSTS) {
            ++summary.already_lowered_load_nodes;
        } else if (node->op_type == OpType::CP_SRAM2DRAM_TMA) {
            ++summary.unresolved_tma_store_nodes;
        }
    }
    return summary;
}

struct BulkCopyRequest {
    DAGNode* node = nullptr;
    bool is_write = false;
    Cycle ready_cycle = 0;
    int line_size_bytes = 0;
    int legacy_issue_rate_bytes_per_cycle = 0;
    double legacy_credit_bytes = 0.0;
    std::size_t next_line = 0;
    std::uint64_t enqueue_sequence = 0;
    std::vector<CacheLineKey> lines;
    bool whole_tile = false;
    std::size_t frontend_quanta = 0;
    std::size_t count() const { return whole_tile ? frontend_quanta : lines.size(); }
};

class BulkCopyUnit {
public:
    bool event_mode = false;
    Cycle event_due = 0;
    std::uint64_t host_service_calls = 0;
    std::uint64_t host_accrued_gap_cycles = 0;
    // Compatibility constructor: preserves the historical one-active-request
    // Hopper TMA FIFO behavior exactly.
    BulkCopyUnit(int setup_latency, int issue_interval,
                 int issue_rate_bytes_per_cycle)
        : BulkCopyUnit(BulkCopyEngineConfig::hopper_tma(
              setup_latency, issue_interval, issue_rate_bytes_per_cycle), -1) {}

    explicit BulkCopyUnit(const BulkCopyEngineConfig& config,
                          int owning_sm_id = -1)
        : config_(config), owning_sm_id_(owning_sm_id) {
        if (config_.mechanism == BulkCopyMechanism::HOPPER_TMA) {
            (void)BulkCopyEngineConfig::hopper_tma(
                config_.setup_latency_cycles,
                config_.issue_interval_cycles,
                config_.legacy_issue_rate_bytes_per_cycle);
        } else {
            (void)BulkCopyEngineConfig::ada_ldgsts(
                config_.setup_latency_cycles,
                config_.issue_interval_cycles,
                config_.per_source_rate,
                config_.aggregate_sm_rate);
        }
    }

    BulkCopyMechanism mechanism() const {
        return config_.mechanism;
    }

    int get_issue_interval(const DAGNode* node) const {
        if (node && node->tma_issue_interval >= 0) {
            return node->tma_issue_interval;
        }
        return config_.issue_interval_cycles;
    }

    int enqueue(DAGNode* node, Cycle current_cycle, L2Cache* l2) {
        if (node == nullptr || l2 == nullptr) {
            throw std::invalid_argument("bulk-copy enqueue requires node and L2");
        }
        if (config_.mechanism == BulkCopyMechanism::ADA_LDGSTS) {
            if (node->op_type != OpType::CP_DRAM2SRAM_LDGSTS) {
                throw std::logic_error(
                    "Ada bulk-copy engine requires an explicitly lowered LDGSTS load");
            }
            if (node->warp_id < 0) {
                throw std::logic_error("Ada LDGSTS source requires a non-negative warp id");
            }
            if (node->tma_issue_rate_bytes_per_cycle >= 0) {
                throw std::logic_error(
                    "integer per-node TMA rate override is invalid for Ada LDGSTS");
            }
        }

        const int line_size = l2->get_line_size_bytes();
        if (line_size <= 0) {
            throw std::logic_error("bulk-copy engine requires a positive L2 line size");
        }
        std::vector<CacheLineKey> lines;
        const bool empty_observed_source = node->explicit_async_shared_service_v1 &&
            !node->explicit_memory_subops.empty() &&
            std::all_of(node->explicit_memory_subops.begin(),node->explicit_memory_subops.end(),
                        [](const auto& sub){return sub.ranges.empty() && sub.requested_bytes==0;});
        const int subops = empty_observed_source ? 0 : l2->get_subop_count(*node);
        // Frontend rate accounting remains in 128 B work quanta; no row/line
        // addresses are created on the whole-tile path.
        std::size_t whole_quanta = 0;
        if (l2->uses_whole_tiles()) {
            if (config_.mechanism != BulkCopyMechanism::ADA_LDGSTS)
                throw std::logic_error("whole-tile bulk path requires Ada LDGSTS");
            whole_quanta = static_cast<std::size_t>(l2->get_transaction_count(*node));
            if(!node->explicit_memory_subops.empty() && std::all_of(node->explicit_memory_subops.begin(),node->explicit_memory_subops.end(),[](const auto&s){return s.requested_bytes==0 && s.ranges.empty();}))whole_quanta=0;
        }
        if (!l2->uses_whole_tiles()) {
        for (int subop = 0; subop < subops; ++subop) {
            auto sub_lines = l2->get_native_subop_lines(*node, subop);
            lines.insert(lines.end(), sub_lines.begin(), sub_lines.end());
        }
        if (lines.empty() && !node->explicit_async_shared_service_v1) {
            lines.push_back({node->matrix_id, 0});
        }

        }

        const int node_setup = node->tma_setup_latency >= 0
                                   ? node->tma_setup_latency
                                   : config_.setup_latency_cycles;
        const int node_issue_rate = node->tma_issue_rate_bytes_per_cycle >= 0
                                        ? node->tma_issue_rate_bytes_per_cycle
                                        : config_.legacy_issue_rate_bytes_per_cycle;
        if (node_setup < 0 ||
            (config_.mechanism == BulkCopyMechanism::HOPPER_TMA &&
             node_issue_rate <= 0)) {
            throw std::logic_error("invalid bulk-copy node timing override");
        }
        if (config_.mechanism == BulkCopyMechanism::ADA_LDGSTS) {
            validate_fractional_rate_for_line(config_.per_source_rate, line_size);
            validate_fractional_rate_for_line(config_.aggregate_sm_rate, line_size);
        }

        BulkCopyRequest request;
        request.node = node;
        request.is_write = node->op_type == OpType::CP_SRAM2DRAM_TMA;
        request.ready_cycle = current_cycle + node_setup;
        request.line_size_bytes = line_size;
        request.legacy_issue_rate_bytes_per_cycle = node_issue_rate;
        request.enqueue_sequence = next_enqueue_sequence_++;
        request.lines = std::move(lines);
        request.whole_tile = l2->uses_whole_tiles();
        request.frontend_quanta = whole_quanta;
        setup_queue_.push_back(std::move(request));
        event_due = std::min(event_due, std::max(cycle_add(current_cycle, 1), setup_queue_.back().ready_cycle));
        return l2->uses_whole_tiles() ? 1 : static_cast<int>(setup_queue_.back().lines.size());
    }

    std::vector<int> step(Cycle current_cycle, L2Cache* l2) {
        if (l2 == nullptr) {
            throw std::invalid_argument("bulk-copy step requires L2");
        }
        if (event_mode && current_cycle < event_due) return {};
        ++host_service_calls;
        if (config_.mechanism == BulkCopyMechanism::HOPPER_TMA) {
            auto done = step_hopper_tma(current_cycle, l2);
            if (event_mode) event_due = has_work() ? cycle_add(current_cycle, 1) : std::numeric_limits<Cycle>::max();
            return done;
        }
        if (event_mode && current_cycle > last_service_cycle_ && current_cycle - last_service_cycle_ > 1) {
            const auto gap = static_cast<std::uint64_t>(current_cycle - last_service_cycle_ - 1);
            accrue_without_service(gap);
            host_accrued_gap_cycles += gap;
        }
        auto done = step_ada_ldgsts(current_cycle, l2);
        last_service_cycle_ = current_cycle;
        if (event_mode) event_due = next_ada_event(current_cycle);
        return done;
    }

    bool has_work() const {
        return !setup_queue_.empty() || !legacy_pending_queue_.empty() || legacy_active_valid_ || ada_has_work();
    }
    // Read-only ownership audit; queue order, credits and source state unchanged.
    std::size_t pending_node_references(int first, int end) const {
        std::size_t count = 0;
        auto check = [&](const BulkCopyRequest& request) {
            if (request.node && request.node->id >= first && request.node->id < end) ++count;
        };
        for (const auto& request : setup_queue_) check(request);
        for (const auto& request : legacy_pending_queue_) check(request);
        if (legacy_active_valid_) check(legacy_active_);
        for (const auto& source : ada_sources_)
            for (const auto& request : source.requests) check(request);
        return count;
    }
private:
    Cycle last_service_cycle_ = 0;
    struct AdaSourceState {
        int warp_id = -1;
        std::uint64_t credit_units = 0;
        std::deque<BulkCopyRequest> requests;
    };

    static std::uint64_t checked_line_cost(const BulkCopyRate& rate,
                                           int line_size_bytes) {
        const std::uint64_t line = static_cast<std::uint64_t>(line_size_bytes);
        if (line > std::numeric_limits<std::uint64_t>::max() /
                       rate.denominator_cycles) {
            throw std::overflow_error("bulk-copy fixed-point line cost overflow");
        }
        return line * rate.denominator_cycles;
    }

    static void validate_fractional_rate_for_line(const BulkCopyRate& rate,
                                                   int line_size_bytes) {
        const std::uint64_t cost = checked_line_cost(rate, line_size_bytes);
        if (rate.numerator_bytes >= cost) {
            throw std::invalid_argument(
                "Ada LDGSTS contract requires a sub-line-per-cycle rate");
        }
    }

    static std::uint64_t token_cap(const BulkCopyRate& rate,
                                   int line_size_bytes) {
        const std::uint64_t cost = checked_line_cost(rate, line_size_bytes);
        if (cost > std::numeric_limits<std::uint64_t>::max() -
                       (rate.numerator_bytes - 1)) {
            throw std::overflow_error("bulk-copy token cap overflow");
        }
        // One complete line plus at most one cycle of fractional carry. This
        // preserves exact long-run rate while preventing unbounded credit from
        // turning downstream backpressure into a later burst.
        return cost + rate.numerator_bytes - 1;
    }

    static std::uint64_t add_capped(std::uint64_t value,
                                    std::uint64_t increment,
                                    std::uint64_t cap) {
        if (value >= cap || increment >= cap - value) return cap;
        return value + increment;
    }

    static std::uint64_t add_cycles_capped(std::uint64_t value, std::uint64_t rate,
                                         std::uint64_t count, std::uint64_t cap) {
        if (value >= cap) return cap;
        const auto need = cap - value;
        const auto cycles = need / rate + (need % rate != 0);
        return count >= cycles ? cap : value + count * rate;
    }

    void accrue_without_service(std::uint64_t count) {
        // setup arrivals are wake-up boundaries; none may be promoted here.
        int line_size = 0;
        for (auto& source : ada_sources_) {
            if (source.requests.empty()) { source.credit_units = 0; continue; }
            line_size = source.requests.front().line_size_bytes;
            source.credit_units = add_cycles_capped(source.credit_units,
                config_.per_source_rate.numerator_bytes, count,
                token_cap(config_.per_source_rate, line_size));
        }
        // Original idle step clears aggregate credit, including the first
        // idle cycle after a completed source. Preserve that transition.
        if (line_size == 0) ada_aggregate_credit_units_ = 0;
        else ada_aggregate_credit_units_ = add_cycles_capped(ada_aggregate_credit_units_,
            config_.aggregate_sm_rate.numerator_bytes, count,
            token_cap(config_.aggregate_sm_rate, line_size));
    }

    Cycle next_ada_event(Cycle now) const {
        const Cycle floor = cycle_add(now, 1);
        Cycle next = setup_queue_.empty() ? std::numeric_limits<Cycle>::max()
            : std::max(floor, setup_queue_.front().ready_cycle);
        bool active = false;
        auto cycles_for = [](std::uint64_t credit, std::uint64_t cost, std::uint64_t rate) {
            if (credit >= cost) return std::uint64_t(1);
            const auto need = cost - credit;
            return need / rate + (need % rate != 0);
        };
        for (const auto& source : ada_sources_) {
            if (source.requests.empty()) continue;
            active = true;
            const auto& request = source.requests.front();
            if (request.count() == 0) return floor;
            const auto a = cycles_for(source.credit_units,
                checked_line_cost(config_.per_source_rate, request.line_size_bytes),
                config_.per_source_rate.numerator_bytes);
            const auto b = cycles_for(ada_aggregate_credit_units_,
                checked_line_cost(config_.aggregate_sm_rate, request.line_size_bytes),
                config_.aggregate_sm_rate.numerator_bytes);
            next = std::min(next, cycle_add(now, checked_cycle(std::max(a, b))));
        }
        if (!active && ada_aggregate_credit_units_ != 0) next = std::min(next, floor);
        return next;
    }

    std::vector<int> step_hopper_tma(Cycle current_cycle, L2Cache* l2) {
        std::vector<int> issued_complete;
        while (!setup_queue_.empty() &&
               setup_queue_.front().ready_cycle <= current_cycle) {
            legacy_pending_queue_.push_back(std::move(setup_queue_.front()));
            setup_queue_.pop_front();
        }

        if (!legacy_active_valid_ && !legacy_pending_queue_.empty()) {
            legacy_active_ = std::move(legacy_pending_queue_.front());
            legacy_pending_queue_.pop_front();
            legacy_active_valid_ = true;
        }

        if (!legacy_active_valid_) return issued_complete;

        // Historical algorithm intentionally retained for sealed H100 evidence.
        legacy_active_.legacy_credit_bytes += static_cast<double>(
            legacy_active_.legacy_issue_rate_bytes_per_cycle);
        while (legacy_active_.next_line < legacy_active_.lines.size() &&
               legacy_active_.legacy_credit_bytes >=
                   legacy_active_.line_size_bytes) {
            const auto& line_key = legacy_active_.lines[legacy_active_.next_line];
            if (!l2->enqueue_transaction_key(*legacy_active_.node, line_key,
                                             legacy_active_.is_write,
                                             current_cycle,
                                             owning_sm_id_)) {
                break;
            }
            legacy_active_.legacy_credit_bytes -= legacy_active_.line_size_bytes;
            ++legacy_active_.next_line;
        }

        if (legacy_active_.next_line >= legacy_active_.lines.size()) {
            issued_complete.push_back(legacy_active_.node->id);
            legacy_active_valid_ = false;
        }
        return issued_complete;
    }

    AdaSourceState& find_or_create_ada_source(int warp_id) {
        for (auto& source : ada_sources_) {
            if (source.warp_id == warp_id) return source;
        }
        AdaSourceState source;
        source.warp_id = warp_id;
        ada_sources_.push_back(std::move(source));
        return ada_sources_.back();
    }

    bool ada_has_work() const {
        for (const auto& source : ada_sources_) {
            if (!source.requests.empty()) return true;
        }
        return false;
    }

    std::vector<int> step_ada_ldgsts(Cycle current_cycle, L2Cache* l2) {
        std::vector<int> issued_complete;
        while (!setup_queue_.empty() &&
               setup_queue_.front().ready_cycle <= current_cycle) {
            BulkCopyRequest request = std::move(setup_queue_.front());
            setup_queue_.pop_front();
            find_or_create_ada_source(request.node->warp_id)
                .requests.push_back(std::move(request));
        }

        if (!ada_has_work()) {
            ada_aggregate_credit_units_ = 0;
            return issued_complete;
        }

        int line_size = 0;
        for (auto& source : ada_sources_) {
            if (source.requests.empty()) {
                source.credit_units = 0;
                continue;
            }
            line_size = source.requests.front().line_size_bytes;
            const std::uint64_t cap = token_cap(config_.per_source_rate, line_size);
            source.credit_units = add_capped(
                source.credit_units,
                config_.per_source_rate.numerator_bytes,
                cap);
        }
        if (line_size <= 0) {
            throw std::logic_error("Ada LDGSTS active request has no line size");
        }
        ada_aggregate_credit_units_ = add_capped(
            ada_aggregate_credit_units_,
            config_.aggregate_sm_rate.numerator_bytes,
            token_cap(config_.aggregate_sm_rate, line_size));

        while (!ada_sources_.empty()) {
            bool found_eligible = false;
            std::size_t selected = 0;
            for (std::size_t offset = 0; offset < ada_sources_.size(); ++offset) {
                const std::size_t index =
                    (ada_round_robin_source_ + offset) % ada_sources_.size();
                auto& source = ada_sources_[index];
                if (source.requests.empty()) continue;
                const auto& request = source.requests.front();
                const std::uint64_t source_cost = request.count()==0 ? 0 : checked_line_cost(
                    config_.per_source_rate, request.line_size_bytes);
                const std::uint64_t aggregate_cost = request.count()==0 ? 0 : checked_line_cost(
                    config_.aggregate_sm_rate, request.line_size_bytes);
                if (source.credit_units < source_cost ||
                    ada_aggregate_credit_units_ < aggregate_cost) {
                    continue;
                }
                found_eligible = true;
                selected = index;
                break;
            }
            if (!found_eligible) break;

            auto& source = ada_sources_[selected];
            auto& request = source.requests.front();
            const bool accepted = request.count()==0 && request.node->explicit_async_shared_service_v1 ? true : request.whole_tile
                ? (request.next_line+1 < request.count() || l2->enqueue_whole_tile(*request.node,current_cycle,owning_sm_id_))
                : l2->enqueue_transaction_key(*request.node,request.lines[request.next_line],
                                             request.is_write,current_cycle,owning_sm_id_);
            if (!accepted) {
                // Neither token bucket nor line position advances on L2
                // backpressure; the exact request is retried later.
                break;
            }
            if(request.count()!=0) {
                source.credit_units -= checked_line_cost(config_.per_source_rate, request.line_size_bytes);
                ada_aggregate_credit_units_ -= checked_line_cost(config_.aggregate_sm_rate, request.line_size_bytes);
            }
            ++request.next_line;
            ada_round_robin_source_ = (selected + 1) % ada_sources_.size();

            if (request.next_line >= request.count()) {
                issued_complete.push_back(request.node->id);
                source.requests.pop_front();
                if (source.requests.empty()) source.credit_units = 0;
            }
        }
        return issued_complete;
    }

    BulkCopyEngineConfig config_;
    int owning_sm_id_ = -1;
    std::uint64_t next_enqueue_sequence_ = 0;
    std::deque<BulkCopyRequest> setup_queue_;

    // Legacy Hopper TMA state: one active request for the entire SM.
    std::deque<BulkCopyRequest> legacy_pending_queue_;
    BulkCopyRequest legacy_active_{};
    bool legacy_active_valid_ = false;

    // Ada state: one FIFO and exact token bucket per warp source, plus a shared
    // per-SM aggregate bucket. Vector order is first-ready order and therefore
    // deterministic; no unordered-container iteration affects issue order.
    std::vector<AdaSourceState> ada_sources_;
    std::size_t ada_round_robin_source_ = 0;
    std::uint64_t ada_aggregate_credit_units_ = 0;
};

}  // namespace GTSim

#endif  // BULK_COPY_H
