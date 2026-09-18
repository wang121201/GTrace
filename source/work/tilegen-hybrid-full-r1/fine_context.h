#pragma once

// Borrowed-memory form of the frozen ContinuousL2Session execution glue.
// Compile through the hybrid driver's effective full32/light-L2 VFS overlay.
#include "simulator_session.h"
#include "session_mode.h"

#include <limits>
#include <sstream>
#include <stdexcept>

namespace hybrid_full {
using GTSim::Cycle;
using GTSim::DAG;
using GTSim::KernelSessionResult;
using GTSim::L2AddressTransform;
using GTSim::L2Cache;
using GTSim::L2RuntimeObserver;
using GTSim::L2RuntimeStatistics;
using GTSim::Simulator;
using GTSim::SimulatorConfig;
using GTSim::WholeTileMemoryPort;

// Ownership and sequencing are the caller's responsibility: the L2, its native
// backend/full address mapper, and shared_cycle outlive this object; contexts
// execute serially and hand off only with all previous frontend work drained.
// A run failure is fail-stop for the caller; it is not a rollback of L2 state.
class FineContext {
public:
    FineContext(const SimulatorConfig& config, L2Cache& l2, Cycle& shared_cycle)
        : config_(config), l2_(&l2), cycle_(shared_cycle) {
        if (shared_cycle < 0)
            throw std::invalid_argument("negative initial cycle");
    }

    FineContext(const FineContext&) = delete;
    FineContext& operator=(const FineContext&) = delete;
    FineContext(FineContext&&) = delete;
    FineContext& operator=(FineContext&&) = delete;

    KernelSessionResult run_kernel(
        DAG* dag,
        Cycle max_kernel_cycles,
        bool verbose = false,
        L2RuntimeObserver* observer = nullptr,
        const L2AddressTransform* address_transform = nullptr,
        Cycle max_drain_cycles = 100000000);

    void set_whole_tile_port(WholeTileMemoryPort* port) { l2_->whole_tile_port = port; }
    void set_next_kernel_resident_cta_limit(int limit) {
        if (limit < 1 || limit > 32)
            throw std::invalid_argument("resident CTA limit must be in 1..32");
        if (kernel_running_ || !l2_->is_quiescent())
            throw std::logic_error("resident CTA limit requires a quiescent kernel boundary");
        config_.max_concurrent_blocks_per_sm = limit;
    }
    Cycle cycle() const { return cycle_; }
    bool is_quiescent() const { return l2_->is_quiescent(); }
    const L2Cache& l2() const { return *l2_; }
    L2RuntimeStatistics statistics() const { return l2_->runtime_statistics(); }

private:
    SimulatorConfig config_;
    L2Cache* const l2_;  // Borrowed, never rebuilt, reset, flushed, or finalized.
    Cycle& cycle_;      // The driver's authoritative signed monotonic clock.
    bool kernel_running_ = false;
};

inline KernelSessionResult FineContext::run_kernel(
    DAG* dag,
    Cycle max_kernel_cycles,
    bool verbose,
    L2RuntimeObserver* observer,
    const L2AddressTransform* address_transform,
    Cycle max_drain_cycles) {
    if (dag == nullptr || (dag->nodes.empty() && dag->cta_graph_store == nullptr)) {
        throw std::invalid_argument("continuous session requires a non-empty DAG");
    }
    if (max_kernel_cycles <= 0 || max_drain_cycles < 0) {
        throw std::invalid_argument("continuous session cycle budgets are invalid");
    }
    if (!l2_->is_quiescent()) {
        throw std::logic_error("continuous session kernel boundary is not quiescent");
    }
    if (cycle_ > std::numeric_limits<Cycle>::max() - max_kernel_cycles) {
        throw std::overflow_error("continuous session cycle domain overflow");
    }

    KernelSessionResult result;
    result.start_cycle = cycle_;
    result.before = l2_->runtime_statistics();
    l2_->begin_kernel();
    l2_->set_runtime_observer(observer);
    l2_->set_address_transform(address_transform);

    kernel_running_ = true;
    try {
        Simulator simulator(dag, config_, l2_, cycle_);
        // SP-only native-line service: global, SM and TMA still run each cycle.
        // Initial ready queues already exist; do not forecast before first service.
        if (!simulator.simulated_gpu->noc && !l2_->whole_tile_port) {
            for (auto* sm : simulator.simulated_gpu->sms) {
                for (auto* sp : sm->sps) {
                    sp->event_mode = tiny_runtime::sp_event_mode;
                    sp->event_due = 0;
                    sp->sp_scheduler->event_wakeup = [sp](Cycle c) { sp->wake_event(c); };
                }
            }
        }
        // Continuous machine-readable producers must not inherit the legacy
        // human cycle summary written by Simulator::run.  Keep direct
        // Simulator callers byte-compatible; only honor silence_mode here.
        std::ostringstream suppressed_output;
        if (config_.silence_mode) {
            simulator.output_stream = &suppressed_output;
        }
        const bool completed = simulator.run(cycle_ + max_kernel_cycles, verbose);
        cycle_ = simulator.cycle;
        result.host_events[0] = simulator.host_step_calls;
        result.host_events[1] = simulator.host_skipped_cycles;
        for (auto* sm : simulator.simulated_gpu->sms) {
            result.host_events[2] += sm->host_service_calls;
            result.host_events[4] += sm->tma->host_service_calls;
            result.host_events[5] += sm->tma->host_accrued_gap_cycles;
            for (auto* sp : sm->sps) result.host_events[3] += sp->host_service_calls;
        }
        result.kernel_end_cycle = cycle_;
        if (!completed) {
            throw std::runtime_error("continuous session kernel timed out");
        }

        while (!l2_->is_quiescent()) {
            if (result.drain_cycles >= max_drain_cycles ||
                cycle_ == std::numeric_limits<Cycle>::max()) {
                throw std::runtime_error("continuous session drain timed out");
            }
            ++cycle_;
            const auto unexpected = l2_->step(cycle_);
            if (!unexpected.empty()) {
                throw std::logic_error(
                    "continuous session observed a node completion after kernel completion");
            }
            l2_->end_cycle(cycle_);
            ++result.drain_cycles;
        }
        result.quiescent_end_cycle = cycle_;
        result.after = l2_->runtime_statistics();
    } catch (...) {
        kernel_running_ = false;
        l2_->set_runtime_observer(nullptr);
        l2_->set_address_transform(nullptr);
        throw;
    }

    kernel_running_ = false;
    l2_->set_runtime_observer(nullptr);
    l2_->set_address_transform(nullptr);
    return result;
}

} // namespace hybrid_full
