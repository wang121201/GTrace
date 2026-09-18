#include "simulator_session.h"
#include "session_mode.h"

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace GTSim {
namespace {

std::unique_ptr<L2Cache> make_session_l2(
        const SimulatorConfig& config,
        L2DramCompletionBackend* completion_backend,
        const L2DramAddressMapper* dram_address_mapper) {
    int dram_bandwidth_bytes_per_cycle = 0;
    if (config.memory_model_semantics.dram_bandwidth ==
        DramBandwidthSemantics::LEGACY_INTEGER_DRAM_CYCLE_WITH_FREQUENCY_RATIO) {
        if (config.dram_bandwidth_gbps <= 0.0 ||
            config.dram_frequency_mhz <= 0.0 ||
            config.core_frequency_mhz <= 0.0) {
            throw std::invalid_argument(
                "legacy DRAM conversion requires positive bandwidth and frequencies");
        }
        dram_bandwidth_bytes_per_cycle = static_cast<int>(
            std::lround((config.dram_bandwidth_gbps * 1e9) /
                        (config.dram_frequency_mhz * 1e6)));
    }
    return std::make_unique<L2Cache>(
        config.l2_cache_size_bytes,
        config.l2_line_size_bytes,
        config.l2_hit_latency_cycles,
        config.l2_bandwidth_bytes_per_cycle,
        config.l2_write_bandwidth_bytes_per_cycle,
        config.l2_queue_depth,
        config.l2_bypass_cache,
        dram_bandwidth_bytes_per_cycle,
        config.l2_miss_penalty_cycles,
        config.core_frequency_mhz,
        config.dram_frequency_mhz,
        config.memory_model_semantics,
        config.per_sm_l1,
        completion_backend,
        dram_address_mapper);
}

} // namespace

ContinuousL2Session::ContinuousL2Session(
        const SimulatorConfig& config,
        L2DramCompletionBackend* completion_backend,
        const L2DramAddressMapper* dram_address_mapper, Cycle initial_cycle)
    : config_(config),
      l2_(make_session_l2(config, completion_backend, dram_address_mapper)),
      cycle_(initial_cycle) {
    if(initial_cycle<0)throw std::invalid_argument("negative initial cycle");
}

KernelSessionResult ContinuousL2Session::run_kernel(
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
        Simulator simulator(dag, config_, l2_.get(), cycle_);
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

} // namespace GTSim
