#include "cycle.h"
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "dag_node.h"
#include "cta_sparse_slots.h"
#include "lazy_ready_queue.h"
#include "pipeline.h"
#include "memory.h"
#include "tma.h"
#include "tmem.h"
#include <vector>
#include <deque>
#include <cstdint>
#include <unordered_map>
#include <optional>
#include <set>
#include <array>
#include <algorithm>
#include <functional>

namespace GTSim {

// Forward declarations
class Memory;

enum class SchedulePolicy {
    GTO,
    RR
};

// Return the issue occupancy for one Tensor group member. Legacy mode uses
// output elements; explicit declared-work mode consumes the node's scalar FMA
// work contract and rejects undeclared Tensor nodes.
int tensor_issue_span(const DAGNode& node, const Pipeline& pipe);

// Scheduler implementing per-subpartition GTO (Greedy-Then-Oldest) policy
class Scheduler {
public:
    std::uint64_t host_node_membership_generation=0;
    void host_note_node_membership_change(){
        if(host_node_membership_generation==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("host node membership generation overflow");
        ++host_node_membership_generation;
    }
    // Notification only: never performs issue/readiness queries with side effects.
    std::function<void(Cycle)> event_wakeup;
    std::function<void(DAGNode*,Cycle)> before_node_issue; // Host stage lookahead only.
    void notify_event(Cycle when = 0) { if (event_wakeup) event_wakeup(when); }
    Cycle next_possible_issue_cycle(Cycle current_cycle);
    int scheduler_warp_num;
    CtaSparseSlots<std::vector<DAGNode*>> scheduler_warp;  // Nodes per warp
    CtaSparseSlots<std::uint8_t>* scoreboard;              // Track completed nodes (global)
    CtaSparseSlots<LazyReadyQueue> ready_warp;       // Ready nodes per warp
    std::unordered_map<int, DAGNode*> node_dict;        // Quick node lookup
    CtaSparseSlots<int> warp_head_index;                   // Current head per warp

    // Track last issued pipeline per warp for fairness (O2 feature)
    CtaSparseSlots<std::optional<std::string>> last_issued_pipeline;

    // Track last issue cycle for each warp (for GTO scheduling)
    CtaSparseSlots<Cycle> last_issue_cycle;

    // Track which warp issued last cycle
    int last_issued_warp;

    // Track which subpartition this scheduler belongs to
    int subpartition_id;

    // Configuration flags
    bool O2_enabled;  // Dynamic allocation
    SchedulePolicy schedule_policy;
    int rr_cursor;

    // TB residency optimization: O(1) lookup with event-driven updates
    CtaSparseSlots<bool> warp_is_resident;                      // Per-warp residency flag
    std::unordered_map<int, std::vector<int>> tb_to_warps;   // Map TB ID to its warp IDs
    std::vector<int> resident_warps;                          // Event-maintained scheduler candidates

    // Pipeline lookup optimization: O(1) hash map instead of O(N) linear search
    std::unordered_map<std::string, Pipeline*> pipeline_map;
    std::vector<Scheduler*>* peer_schedulers;
    std::vector<Pipeline*>* pipelines_ptr;
    TMAUnit* tma_unit;
    TMEMUnit* tmem_unit;
    CtaSparseSlots<Cycle> tma_next_issue_cycle;
    CtaSparseSlots<int> tma_issue_interval_cache;

    Cycle sram_next_issue_cycle;
    static constexpr int kMemoryOpTypes = 11;
    std::array<CtaSparseSlots<Cycle>, kMemoryOpTypes> memory_next_issue_cycle;
    int startup_delay_cycles;

    Scheduler(int warp_count, bool o2 = false, SchedulePolicy policy = SchedulePolicy::GTO)
        : scheduler_warp_num(warp_count),
          scheduler_warp(warp_count),
          scoreboard(nullptr),
          ready_warp(warp_count),
          warp_head_index(warp_count, 0),
          last_issued_pipeline(warp_count, std::nullopt),
          last_issue_cycle(warp_count, -1000),
          last_issued_warp(-1),
          subpartition_id(-1),
          O2_enabled(o2),
          schedule_policy(policy),
          rr_cursor(0),
          warp_is_resident(warp_count, false),
          peer_schedulers(nullptr),
          pipelines_ptr(nullptr),
          tma_unit(nullptr),
          tmem_unit(nullptr),
          tma_next_issue_cycle(warp_count, 0),
          tma_issue_interval_cache(warp_count, -1),
          sram_next_issue_cycle(0),
          memory_next_issue_cycle{},
          startup_delay_cycles(0) {}  // Default: no warps resident

    void init_memory_issue_cycles(int warp_count) {
        for (auto& vec : memory_next_issue_cycle) {
            vec.assign(warp_count, 0);
        }
    }

    // Initialize scheduler with nodes
    void init_with_nodes(const std::vector<DAGNode*>& nodes, int total_nodes, int sp_id,
                         CtaSparseSlots<std::uint8_t>* global_scoreboard) {
        host_note_node_membership_change();
        scoreboard = global_scoreboard;
        init_memory_issue_cycles(scheduler_warp_num);
        if (scoreboard && scoreboard->size() < static_cast<size_t>(total_nodes)) {
            scoreboard->resize(total_nodes, 0);
        }
        subpartition_id = sp_id;

        for (auto* node : nodes) {
            node_dict[node->id] = node;
            scheduler_warp[node->warp_id].push_back(node);
            if (node->remaining_deps == 0) {
                ready_warp[node->warp_id].push_back(node);
            }
        }

        // Build TB-to-warps mapping for event-driven residency tracking
        for (int warp_id = 0; warp_id < scheduler_warp_num; ++warp_id) {
            if (!scheduler_warp[warp_id].empty()) {
                // All nodes in a warp belong to the same TB - check first node
                int tb_id = scheduler_warp[warp_id][0]->thread_block_id;
                tb_to_warps[tb_id].push_back(warp_id);
            }
        }
    }

    // Event-driven residency updates: called when a TB is dispatched to this SM
    void on_tb_dispatch(int tb_id) {
        notify_event();
        auto it = tb_to_warps.find(tb_id);
        if (it != tb_to_warps.end()) {
            for (int warp_id : it->second) {
                if (!warp_is_resident[warp_id]) {
                    resident_warps.push_back(warp_id);
                }
                warp_is_resident[warp_id] = true;
            }
        }
    }

    // Event-driven residency updates: called when a TB completes and is retired
    void on_tb_retire(int tb_id) {
        notify_event();
        auto it = tb_to_warps.find(tb_id);
        if (it != tb_to_warps.end()) {
            for (int warp_id : it->second) {
                warp_is_resident[warp_id] = false;
                auto resident_it = std::find(resident_warps.begin(), resident_warps.end(), warp_id);
                if (resident_it != resident_warps.end()) {
                    resident_warps.erase(resident_it);
                }
            }
        }
    }

    // Main scheduling function - returns list of issued nodes
    std::vector<DAGNode*> schedule(Cycle current_cycle, std::vector<Pipeline*>& pipelines,
                                   Memory* sram, L2Cache* l2);

    void enqueue_ready(DAGNode* node) {
        if (node->finished || node->remaining_deps != 0) return;
        ready_warp[node->warp_id].push_back(node);
        notify_event(node->ready_cycle);
    }

    void throttle_sram_issue(Cycle next_cycle) {
        if (next_cycle > sram_next_issue_cycle) {
            sram_next_issue_cycle = next_cycle;
        }
    }

    void throttle_memory_issue(int op_index, int warp_id, Cycle next_cycle) {
        if (op_index < 0 || op_index >= kMemoryOpTypes) {
            return;
        }
        auto& cycles = memory_next_issue_cycle[op_index];
        if (warp_id < 0 || warp_id >= static_cast<int>(cycles.size())) {
            return;
        }
        if (next_cycle > cycles[warp_id]) {
            cycles[warp_id] = next_cycle;
        }
    }

    void set_peer_schedulers(std::vector<Scheduler*>* peers) {
        peer_schedulers = peers;
    }

    void set_pipelines(std::vector<Pipeline*>* pipes) {
        pipelines_ptr = pipes;
    }

    void set_tma_unit(TMAUnit* unit) {
        tma_unit = unit;
    }

    void set_tmem_unit(TMEMUnit* unit) {
        tmem_unit = unit;
    }

    bool tma_ready(const DAGNode* node, Cycle current_cycle) const {
        if (node->warp_id < 0 || node->warp_id >= static_cast<int>(tma_next_issue_cycle.size())) {
            return false;
        }
        return current_cycle >= tma_next_issue_cycle[node->warp_id];
    }

    void ensure_pipeline_map() {
        if (!pipeline_map.empty() || pipelines_ptr == nullptr) {
            return;
        }
        for (auto* pipe : *pipelines_ptr) {
            pipeline_map[pipe->pipeline_name] = pipe;
        }
    }

private:
    // Helper: Check if node is ready and find its pipeline
    Pipeline* node_ready_and_pipe(DAGNode* node, Cycle current_cycle,
                                  std::vector<Pipeline*>& pipelines,
                                  Memory* sram, L2Cache* l2);

    // Helper: Try to issue one instruction from given warp
    std::pair<DAGNode*, Pipeline*> try_issue_from_warp(
        int warp_id, Cycle current_cycle, std::vector<Pipeline*>& pipelines,
        Memory* sram, L2Cache* l2);

    // Helper: Issue a node and update tracking state
    void issue_node(DAGNode* node, Pipeline* pipe, Cycle current_cycle,
                   Memory* sram, L2Cache* l2, int issue_span_override = -1);
};

} // namespace GTSim

#endif // SCHEDULER_H
