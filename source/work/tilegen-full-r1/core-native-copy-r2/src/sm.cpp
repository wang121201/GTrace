#include "sm.h"
#include "noc.h"
#include <algorithm>

namespace GTSim {

void SM::initialize_tb_scheduler(const std::vector<DAGNode*>& all_nodes) {
    if (!cta_materialize) {
    // Build mapping from TB ID to nodes
    int max_node_id = 0;
    for (auto* node : all_nodes) {
        int tb_id = node->thread_block_id;
        all_tb_ids.insert(tb_id);
        tb_to_nodes[tb_id].push_back(node);
        max_node_id = std::max(max_node_id, node->id);
    }

    // Initialize remaining node counters for fast completion checks
    for (int tb_id : all_tb_ids) {
        tb_remaining_count[tb_id] = static_cast<int>(tb_to_nodes[tb_id].size());
    }

    // Build direct node ID → TB ID mapping for O(1) lookup in hot path
    node_id_to_tb_id.resize(max_node_id + 1, -1);  // -1 indicates unmapped
    for (auto* node : all_nodes) {
        node_id_to_tb_id[node->id] = node->thread_block_id;
    }

    }
    // Dispatch first R TBs as resident
    int dispatched = 0;
    for (int tb_id : all_tb_ids) {
        if (dispatched < max_resident_tbs) {
            if (cta_materialize) cta_materialize(tb_id);
            resident_tb_ids.insert(tb_id);

            // Notify all subpartitions about TB dispatch (event-driven)
            for (auto* sp : sps) {
                sp->sp_scheduler->on_tb_dispatch(tb_id);
            }

            dispatched++;
        } else {
            waiting_tb_queue.push(tb_id);
        }
    }

    tb_dispatch_ready.clear();
}

bool SM::is_tb_complete(int tb_id) {
    // Optimized: O(1) counter check instead of O(nodes) iteration
    auto it = tb_remaining_count.find(tb_id);
    if (it == tb_remaining_count.end()) {
        return true;  // No nodes for this TB
    }
    return it->second == 0;
}

bool SM::dispatch_next_tb(Cycle current_cycle) {
    if (waiting_tb_queue.empty()) {
        return false;  // No TBs waiting
    }

    if (!tb_dispatch_ready.empty() && current_cycle < tb_dispatch_ready.front()) {
        return false;  // Dispatch delay not yet satisfied
    }

    if (!tb_dispatch_ready.empty()) {
        tb_dispatch_ready.pop_front();
    }

    int next_tb = waiting_tb_queue.front();
    waiting_tb_queue.pop();
    if (cta_materialize) cta_materialize(next_tb);
    resident_tb_ids.insert(next_tb);

    // Notify all subpartitions about TB dispatch (event-driven)
    for (auto* sp : sps) {
        sp->sp_scheduler->on_tb_dispatch(next_tb);
    }

    return true;
}

void SM::on_node_complete(int node_id) {
    if (event_owner_wakeup) event_owner_wakeup(0);
    if (node_id < 0 || node_id >= static_cast<int>(node_id_to_tb_id.size())) {
        return;
    }
    int tb_id = node_id_to_tb_id[node_id];
    if (tb_id == -1) {
        return;
    }
    auto it = tb_remaining_count.find(tb_id);
    if (it != tb_remaining_count.end() && it->second > 0) {
        it->second--;
        if(cta_node_complete)cta_node_complete(node_id);
    }
}

void SM::set_noc(NoC* noc_ptr) {
    for (auto* sp : sps) {
        sp->noc = noc_ptr;
        sp->owning_sm_id = sm_id;
    }
}

void SM::step(Cycle current_cycle, L2Cache* l2,
              std::vector<int>* unhandled_sram_completions) {
    ++host_service_calls;
    // Step 1: Step SRAM and handle completions
    auto completed = sram->step(current_cycle);
    for (int node_id : completed) {
        if (node_id_to_sp && node_id >= 0 &&
            node_id < static_cast<int>(node_id_to_sp->size())) {
            if (auto* sp = (*node_id_to_sp)[node_id]) {
                if (std::find(sps.begin(), sps.end(), sp) != sps.end()) {
                    bool finished = sp->handle_memory_completion(node_id, current_cycle);
                    if (finished) {
                        on_node_complete(node_id);
                    }
                } else if (unhandled_sram_completions) {
                    // This SRAM completion is for a node owned by another SM's subpartition
                    // (remote access that was enqueued on this SM's SRAM by the NoC)
                    unhandled_sram_completions->push_back(node_id);
                }
            }
        }
    }

    // Step 2: Step TMEM memory and handle completions
    auto tmem_completed = tmem_mem->step(current_cycle);
    for (int node_id : tmem_completed) {
        if (node_id_to_sp && node_id >= 0 &&
            node_id < static_cast<int>(node_id_to_sp->size())) {
            if (auto* sp = (*node_id_to_sp)[node_id]) {
                if (std::find(sps.begin(), sps.end(), sp) != sps.end()) {
                    bool finished = sp->handle_memory_completion(node_id, current_cycle);
                    if (finished) {
                        on_node_complete(node_id);
                    }
                }
            }
        }
    }

    // Step 3: Step TMA unit (enqueues L2 transactions)
    auto tma_issued = tma->step(current_cycle, l2);
    if (!tma_issued.empty()) {
        for (int node_id : tma_issued) {
            if (node_id_to_sp && node_id >= 0 &&
                node_id < static_cast<int>(node_id_to_sp->size())) {
                if (auto* sp = (*node_id_to_sp)[node_id]) {
                    if (std::find(sps.begin(), sps.end(), sp) != sps.end()) {
                        sp->handle_tma_issue_complete(node_id, current_cycle);
                    }
                }
            }
        }
    }

    // Step 4: Step each subpartition
    for (auto* sp : sps) {
        auto non_memory_completions = sp->step(current_cycle, sram, tmem_mem, l2);

        // Decrement TB remaining count using direct array lookup (O(1))
        for (int node_id : non_memory_completions) {
            on_node_complete(node_id);
        }

        // Global scoreboard already updated at completion time.
    }

    // Step 5: Check for completed TBs and dispatch new ones
    std::vector<int> completed_tbs;
    for (int tb_id : resident_tb_ids) {
        if (is_tb_complete(tb_id)) {
            completed_tbs.push_back(tb_id);
        }
    }

    for (int tb_id : completed_tbs) {
        // Notify all subpartitions about TB retire (event-driven)
        for (auto* sp : sps) {
            sp->sp_scheduler->on_tb_retire(tb_id);
        }

        resident_tb_ids.erase(tb_id);
        if (cta_retire) cta_retire(tb_id,current_cycle);
        if (block_schedule_latency_cycles > 0) {
            tb_dispatch_ready.push_back(current_cycle + block_schedule_latency_cycles);
        } else {
            tb_dispatch_ready.push_back(current_cycle);
        }
    }

    while (resident_tb_ids.size() < static_cast<size_t>(max_resident_tbs) &&
           !waiting_tb_queue.empty()) {
        if (!dispatch_next_tb(current_cycle)) {
            break;
        }
    }
}

Cycle SM::next_event_cycle(Cycle current_cycle) const {
    const Cycle floor = cycle_add(current_cycle, 1);
    Cycle next = std::min(sram->next_completion_cycle, tmem_mem->next_completion_cycle);
    next = std::min(next, tma->event_due);
    for (auto* sp : sps) next = std::min(next, sp->event_due);
    if (!waiting_tb_queue.empty() && resident_tb_ids.size() < static_cast<std::size_t>(max_resident_tbs))
        next = std::min(next, tb_dispatch_ready.empty() ? floor : tb_dispatch_ready.front());
    return next == std::numeric_limits<Cycle>::max() ? next : std::max(floor, next);
}

} // namespace GTSim
