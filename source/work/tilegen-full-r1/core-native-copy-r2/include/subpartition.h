#include "cycle.h"
#ifndef SUBPARTITION_H
#define SUBPARTITION_H

#include "scheduler.h"
#include "pipeline.h"
#include "memory.h"
#include "retry_prefix.h"
#include "host_issue_certificate.h"
#include <deque>
#include <vector>
#include <unordered_map>

namespace GTSim {

class NoC;  // Forward declaration

// Register file for a subpartition
struct Register {
    int register_size;
    int register_totally_used;

    explicit Register(int size_in_bytes)
        : register_size(size_in_bytes), register_totally_used(0) {}
};

// Subpartition containing scheduler, pipelines, and registers
class Subpartition {
public:
    bool event_mode = false;
    Cycle event_due = 0;
    host_issue::Certificate<Cycle> host_issue_certificate;
    bool host_issue_certificate_active=false;
    std::uint64_t host_schedule_certificate_skips=0;
    std::uint64_t host_forecast_certificate_reuses=0;
    std::uint64_t host_forecast_certificate_refreshes=0;
    std::uint64_t host_service_calls = 0;
    std::function<void(Cycle)> event_owner_wakeup;
    void wake_event(Cycle when) {
        host_issue_certificate.invalidate();
        event_due = std::min(event_due, when);
        if (event_owner_wakeup) event_owner_wakeup(when);
    }
    Cycle next_event_cycle(Cycle current_cycle);
    int subpartition_id;
    std::vector<Pipeline*> pipelines;
    host_pipeline::FrontMinimum<Cycle> host_pipeline_front;
    std::deque<std::pair<int, Cycle>> barrier_queue; // (node_id, complete_cycle)
    Scheduler* sp_scheduler;
    Register* sp_register;
    struct RetryEntry {
        int node_id;
        int bytes;
        int bank_conflict;
        int subop_index;
        bool is_l2;
        Cycle ready_cycle;
        CacheLineKey line_key;
        bool use_line_key;
        L1ReadMissMemo host_negative_memo{};
    };

    std::unordered_map<int, std::string> pending_memory_ops;  // node_id -> op type
    std::vector<RetryEntry> retry_queue_sram;
    std::deque<RetryEntry> retry_queue_l2;
    retry_host::NegativePrefix host_negative_prefix;
    std::unordered_map<int,int> whole_tile_frontend_completions;
    std::vector<int> deferred_tmem_pair_completions;
    // IDs only; normal CTA ownership checks cover this queue before reclamation.
    std::deque<std::pair<int,std::size_t>> async_shared_ready;
    void begin_async_shared(DAGNode* node, Cycle current_cycle);
    CtaSparseSlots<Subpartition*>* node_id_to_sp;
    std::size_t* completed_node_count;

    NoC* noc;
    int owning_sm_id;

    int tensor_core_throughput;
    bool O2_enabled;
    Cycle l2_issue_cycle;
    int l2_issue_count;
    int l2_max_transactions_per_cycle;
    int tmem_write_extra_latency_cycles;

    Subpartition(int id, int warp_count, int tensor_throughput, int simd_throughput,
                 int sfu_throughput, int ls_throughput_bytes,
                 int tensor_latency_cycles, int simd_latency_cycles,
                 int sfu_latency_cycles, int shfl_latency_cycles, int ls_latency_cycles,
                 int tensor_width, int simd_width, int sfu_width, int ls_width_bytes,
                 bool o2, SchedulePolicy policy, int l2_max_tx_per_cycle)
        : subpartition_id(id), noc(nullptr), owning_sm_id(-1),
          tensor_core_throughput(tensor_throughput),
          O2_enabled(o2), l2_max_transactions_per_cycle(l2_max_tx_per_cycle),
          tmem_write_extra_latency_cycles(0) {

        // Create pipelines (throughput is per-cycle capability)
        pipelines.push_back(new Pipeline("Tensor", tensor_latency_cycles, tensor_throughput, tensor_width));
        pipelines.push_back(new Pipeline("SIMD", simd_latency_cycles, simd_throughput, simd_width));
        pipelines.push_back(new Pipeline("SFU", sfu_latency_cycles, sfu_throughput, sfu_width));
        // SHFL is modeled as a lightweight cross-warp shuffle with fixed latency and no contention.
        pipelines.push_back(new Pipeline("SHFL", shfl_latency_cycles, 1 << 30, 1 << 30));
        pipelines.push_back(new Pipeline("LD", ls_latency_cycles, ls_throughput_bytes, ls_width_bytes));
        pipelines.push_back(new Pipeline("ST", ls_latency_cycles, ls_throughput_bytes, ls_width_bytes));

        if(TILEGEN_HOST_PIPELINE_FRONT_CACHE)
            for(auto* pipe:pipelines)pipe->host_front_owner=&host_pipeline_front;

        sp_scheduler = new Scheduler(warp_count, o2, policy);
        sp_scheduler->set_pipelines(&pipelines);
        sp_register = new Register(65536);  // 64KB register file
        node_id_to_sp = nullptr;
        completed_node_count = nullptr;
        l2_issue_cycle = -1;
        l2_issue_count = 0;
    }

    ~Subpartition() {
        for (auto* pipe : pipelines) delete pipe;
        delete sp_scheduler;
        delete sp_register;
    }

    // Step function for subpartition
    std::vector<int> step(Cycle current_cycle, Memory* sram, Memory* tmem_mem, L2Cache* l2);

    // Complete a node
    void complete_node(DAGNode* node, Cycle current_cycle);

    // Handle memory operation completion
    bool handle_memory_completion(int node_id, Cycle current_cycle);

    // Handle TMA issue completion (all transactions issued)
    void handle_tma_issue_complete(int node_id, Cycle current_cycle);

    void set_node_to_sp_map(CtaSparseSlots<Subpartition*>* map) {
        node_id_to_sp = map;
    }

    void set_completed_node_counter(std::size_t* counter) {
        completed_node_count = counter;
    }

private:
    // Helper: Try to enqueue memory operation based on node's OpType
    bool try_enqueue_memory(DAGNode* node, int bytes_val, Cycle current_cycle,
                           int bank_conflict_factor, int subop_index,
                           const CacheLineKey& line_key, bool use_line_key,
                           Memory* sram, Memory* tmem_mem, L2Cache* l2,
                           L1ReadMissMemo* host_memo=nullptr);

    void resolve_issue_dependencies(DAGNode* node, Cycle current_cycle);
};

} // namespace GTSim

#endif // SUBPARTITION_H
