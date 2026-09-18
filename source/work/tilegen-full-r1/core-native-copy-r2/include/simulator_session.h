#include "cycle.h"
#ifndef SIMULATOR_SESSION_H
#define SIMULATOR_SESSION_H

#include "simulator.h"

#include <memory>

namespace GTSim {

struct KernelSessionResult {
    std::array<std::uint64_t,6> host_events{}; // steps, skipped, SM, SP, bulk, accrued gaps
    Cycle start_cycle = 0;
    Cycle kernel_end_cycle = 0;
    Cycle quiescent_end_cycle = 0;
    Cycle drain_cycles = 0;
    L2RuntimeStatistics before;
    L2RuntimeStatistics after;
};

// Runs a sequence of kernel DAGs with one L2/DRAM object and a monotonic
// cycle domain. Per-kernel SM/scheduler state is rebuilt, while resident L2
// lines, sequence counters, and cumulative statistics remain live.
class ContinuousL2Session {
public:
    explicit ContinuousL2Session(
        const SimulatorConfig& config,
        L2DramCompletionBackend* completion_backend = nullptr,
        const L2DramAddressMapper* dram_address_mapper = nullptr,
        Cycle initial_cycle = 0);

    ContinuousL2Session(const ContinuousL2Session&) = delete;
    ContinuousL2Session& operator=(const ContinuousL2Session&) = delete;

    KernelSessionResult run_kernel(
        DAG* dag,
        Cycle max_kernel_cycles,
        bool verbose = false,
        L2RuntimeObserver* observer = nullptr,
        const L2AddressTransform* address_transform = nullptr,
        Cycle max_drain_cycles = 100000000);

    void set_whole_tile_port(WholeTileMemoryPort* port) { l2_->whole_tile_port = port; }
    // Host launch parameter only. Cache/backend identity and the continuous
    // clock remain unchanged; resource/occupancy provenance belongs to caller.
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
    std::unique_ptr<L2Cache> l2_;
    Cycle cycle_;
    bool kernel_running_ = false;
};

} // namespace GTSim

#endif // SIMULATOR_SESSION_H
