#include "scheduler.h"
#include "scheduler_observer.h"
#include "cycle_overlap.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#ifndef TILEGEN_HOST_LAZY_GTO_SORT
#define TILEGEN_HOST_LAZY_GTO_SORT 1
#endif

namespace GTSim {

static bool is_sram_read_op(const DAGNode* node);
static bool is_load_like_op(const DAGNode* node);
static bool is_store_like_op(const DAGNode* node);
static bool is_memory_op(const DAGNode* node);
static int memory_op_index(const DAGNode* node);
static bool is_wgmma_op(const DAGNode* node);
static bool is_tcgen05_mma_op(const DAGNode* node);
static bool is_group_issued_op(const DAGNode* node);
static int node_issue_group_size(const DAGNode* node);
static int node_issue_group_leader_warp(const DAGNode* node);
static bool is_tmem_op(const DAGNode* node);
static bool is_barrier_op(const DAGNode* node);
static bool is_bulk_copy_op(const DAGNode* node);

Cycle Scheduler::next_possible_issue_cycle(Cycle current_cycle) {
    const Cycle floor = cycle_add(current_cycle, 1);
    // Forecast only after real service. Never initialize scheduler state here.
    if (pipeline_map.empty()) return floor;
    Cycle next = std::numeric_limits<Cycle>::max();
    const auto& forecast_ready = ready_warp;
    const auto& forecast_tma = tma_next_issue_cycle;
    const auto& forecast_memory = memory_next_issue_cycle;
    for (int w : resident_warps) {
        for (auto* n : forecast_ready[w]) {
            if (n->finished || (n->total_transactions > 0 &&
                n->next_transaction_index >= n->total_transactions)) return floor;
            // TMEM can_issue registers paired handshakes; it is not a query.
            // Bulk/group/TMEM are only conservative query fallbacks,
            // not newly qualified event-driven workload families.
            if (is_tmem_op(n) || is_group_issued_op(n) || is_bulk_copy_op(n)) return floor;
            Cycle candidate = std::max(floor, n->ready_cycle);
            if (is_bulk_copy_op(n)) {
                // Existing O2 tail scanning bypasses tma_ready. Preserve that
                // behavior: its command interval is not a safe lower bound.
                if (O2_enabled) return floor;
                candidate = std::max(candidate, forecast_tma[w]);
            } else if (!is_barrier_op(n)) {
                auto p = pipeline_map.find(is_load_like_op(n) ? "LD" :
                    is_store_like_op(n) ? "ST" : n->pipeline_type);
                if (p != pipeline_map.end()) candidate = std::max(candidate, p->second->pipeline_next_issue_cycle);
                if (is_sram_read_op(n)) candidate = std::max(candidate, sram_next_issue_cycle);
                if (is_memory_op(n) && n->setup_latency > 0) {
                    const int idx = memory_op_index(n);
                    if (idx >= 0) candidate = std::max(candidate, forecast_memory[idx][w]);
                }
            }
            next = std::min(next, candidate);
            // Omitting full-queue and ordering constraints is conservative:
            // they can delay an issue, never make it possible earlier.
            if (!O2_enabled) break;
        }
    }
    return next;
}

Pipeline* Scheduler::node_ready_and_pipe(DAGNode* node, Cycle current_cycle,
                                         std::vector<Pipeline*>& pipelines,
                                         Memory* sram, L2Cache* l2) {
    auto* observation = scheduler_observer::active();
    if (observation) ++observation->candidate_predicate_checks;
    (void)pipelines;
    (void)l2;
    if (node->finished) {
        scheduler_observer::reject(observation, scheduler_observer::Reject::finished);
        return nullptr;
    }

    if (node->total_transactions > 0 &&
        node->next_transaction_index >= node->total_transactions) {
        scheduler_observer::reject(observation, scheduler_observer::Reject::already_issued);
        return nullptr;  // All sub-ops already issued
    }

    // Check if node is ready (event-driven dependency tracking)
    if (node->remaining_deps > 0 || node->ready_cycle > current_cycle) {
        scheduler_observer::reject(observation, node->remaining_deps > 0 ? scheduler_observer::Reject::unresolved_dependency : scheduler_observer::Reject::future_ready_cycle);
        return nullptr;
    }

    // Check if node's warp is resident (O(1) flag check)
    if (!warp_is_resident[node->warp_id]) {
        scheduler_observer::reject(observation, scheduler_observer::Reject::not_resident);
        return nullptr;  // Warp's TB not resident, cannot issue
    }

    if (is_tmem_op(node)) {
        if (!tmem_unit || !tmem_unit->can_issue(node, current_cycle)) {
            scheduler_observer::reject(observation, scheduler_observer::Reject::tmem_unavailable);
            return nullptr;
        }
    }

    if (is_sram_read_op(node) && current_cycle < sram_next_issue_cycle) {
        scheduler_observer::reject(observation, scheduler_observer::Reject::sram_issue_interval);
        return nullptr;
    }
    if (is_memory_op(node) && node->setup_latency > 0) {
        int idx = memory_op_index(node);
        if (idx >= 0 &&
            node->warp_id >= 0 &&
            node->warp_id < static_cast<int>(memory_next_issue_cycle[idx].size()) &&
            current_cycle < memory_next_issue_cycle[idx][node->warp_id]) {
            scheduler_observer::reject(observation, scheduler_observer::Reject::memory_issue_interval);
            return nullptr;
        }
    }

    // Check memory queue availability for ld/st operations (using enum for fast dispatch)
    switch (node->op_type) {
        case OpType::LD_SRAM2REG:
            if (sram->is_full(false)) {
                scheduler_observer::reject(observation, scheduler_observer::Reject::sram_read_queue_full);
                return nullptr;
            }
            break;
        case OpType::ST_REG2SRAM:
            if (sram->is_full(true)) {
                scheduler_observer::reject(observation, scheduler_observer::Reject::sram_write_queue_full);
                return nullptr;
            }
            break;
        case OpType::LD_SRAM2REG_DSM:
        case OpType::ST_REG2SRAM_DSM:
            break;
        case OpType::LD_DRAM2REG:
        case OpType::ST_REG2DRAM:
        case OpType::CP_DRAM2SRAM:
        case OpType::CP_SRAM2DRAM:
            break;
        default:
            break;
    }

    // Find pipeline using O(1) hash map lookup
    Pipeline* pipe = nullptr;
    if (is_load_like_op(node)) {
        auto it = pipeline_map.find("LD");
        if (it == pipeline_map.end()) {
            scheduler_observer::reject(observation, scheduler_observer::Reject::missing_pipeline);
            return nullptr;
        }
        pipe = it->second;
    } else if (is_store_like_op(node)) {
        auto it = pipeline_map.find("ST");
        if (it == pipeline_map.end()) {
            scheduler_observer::reject(observation, scheduler_observer::Reject::missing_pipeline);
            return nullptr;
        }
        pipe = it->second;
    } else {
        auto pipe_it = pipeline_map.find(node->pipeline_type);
        if (pipe_it == pipeline_map.end()) {
            scheduler_observer::reject(observation, scheduler_observer::Reject::missing_pipeline);
            return nullptr;  // Pipeline not found
        }
        pipe = pipe_it->second;
    }
    if (!pipe->can_issue(current_cycle)) {
        scheduler_observer::reject(observation, scheduler_observer::Reject::pipeline_issue_busy);
        return nullptr;
    }

    if (observation) ++observation->candidate_predicate_passes;
    return pipe;
}

std::pair<DAGNode*, Pipeline*> Scheduler::try_issue_from_warp(
    int warp_id, Cycle current_cycle, std::vector<Pipeline*>& pipelines,
    Memory* sram, L2Cache* l2) {

    auto* observation = scheduler_observer::active();
    if (observation) ++observation->warp_attempts;
    if (warp_id >= scheduler_warp_num) {
        if (observation) ++observation->invalid_warp_attempts;
        return {nullptr, nullptr};
    }

    auto& ready_queue = ready_warp[warp_id];

    while (!ready_queue.empty()) {
        DAGNode* front = ready_queue.front();
        if (front->finished) {
            if (observation) ++observation->finished_queue_entries_removed;
            ready_queue.pop_front();
            continue;
        }
        if (front->total_transactions > 0 &&
            front->next_transaction_index >= front->total_transactions) {
            if (observation) ++observation->issued_queue_entries_removed;
            ready_queue.pop_front();
            continue;
        }
        break;
    }

    if (ready_queue.empty()) {
        if (observation) ++observation->empty_ready_queue_attempts;
        return {nullptr, nullptr};
    }

    // Step 1: Try head first
    DAGNode* candidate_node = ready_queue.front();
    if (is_bulk_copy_op(candidate_node)) {
        if (observation) ++observation->bulk_head_attempts;
        if (!tma_unit) {
            if (observation) ++observation->bulk_head_unavailable_attempts;
            return {nullptr, nullptr};
        }
        if (candidate_node->remaining_deps > 0 ||
            candidate_node->ready_cycle > current_cycle ||
            !warp_is_resident[candidate_node->warp_id] ||
            !tma_ready(candidate_node, current_cycle)) {
            if (observation) ++observation->bulk_head_not_ready_attempts;
            return {nullptr, nullptr};
        }
        ready_queue.pop_front();
        return {candidate_node, nullptr};
    }
    if (is_barrier_op(candidate_node)) {
        if (observation) ++observation->barrier_head_attempts;
        if (candidate_node->remaining_deps > 0 ||
            candidate_node->ready_cycle > current_cycle ||
            !warp_is_resident[candidate_node->warp_id]) {
            if (observation) ++observation->barrier_head_not_ready_attempts;
            return {nullptr, nullptr};
        }
        ready_queue.pop_front();
        return {candidate_node, nullptr};
    }
    if (is_group_issued_op(candidate_node) && node_issue_group_size(candidate_node) > 1) {
        if (observation) ++observation->group_head_deferred_attempts;
        return {nullptr, nullptr};
    }
    Pipeline* pipe = node_ready_and_pipe(candidate_node, current_cycle, pipelines, sram, l2);

    if (pipe != nullptr) {
        ready_queue.pop_front();
        return {candidate_node, pipe};
    }

    // Step 2: Head is stalled - if O2 enabled, scan ready queue
    if (O2_enabled) {
        auto last_pipeline = last_issued_pipeline[warp_id];

        if (last_pipeline.has_value()) {
            for (size_t idx = 1; idx < ready_queue.size(); ++idx) {
                DAGNode* n = ready_queue[idx];
                if (is_barrier_op(n) &&
                    n->remaining_deps == 0 &&
                    n->ready_cycle <= current_cycle &&
                    warp_is_resident[n->warp_id]) {
                    if (observation) ++observation->tail_barrier_selections;
                    ready_queue.erase(ready_queue.begin() + static_cast<long>(idx));
                    return {n, nullptr};
                }
                Pipeline* p = node_ready_and_pipe(n, current_cycle, pipelines, sram, l2);
                if (p != nullptr && n->pipeline_type != last_pipeline.value()) {
                    ready_queue.erase(ready_queue.begin() + static_cast<long>(idx));
                    return {n, p};
                }
            }
        }

        for (size_t idx = 1; idx < ready_queue.size(); ++idx) {
            DAGNode* n = ready_queue[idx];
            if (is_barrier_op(n) &&
                n->remaining_deps == 0 &&
                n->ready_cycle <= current_cycle &&
                warp_is_resident[n->warp_id]) {
                if (observation) ++observation->tail_barrier_selections;
                ready_queue.erase(ready_queue.begin() + static_cast<long>(idx));
                return {n, nullptr};
            }
            Pipeline* p = node_ready_and_pipe(n, current_cycle, pipelines, sram, l2);
            if (p != nullptr) {
                ready_queue.erase(ready_queue.begin() + static_cast<long>(idx));
                return {n, p};
            }
        }
    }

    // Step 3: No ready nodes found in this warp
    return {nullptr, nullptr};
}

static bool is_l2_dram_op(const DAGNode* node) {
    return node->op_type == OpType::LD_DRAM2REG ||
           node->op_type == OpType::ST_REG2DRAM ||
           node->op_type == OpType::CP_DRAM2SRAM ||
           node->op_type == OpType::CP_SRAM2DRAM;
}

static bool is_sram_read_op(const DAGNode* node) {
    return node->op_type == OpType::LD_SRAM2REG ||
           node->op_type == OpType::LD_SRAM2REG_DSM ||
           node->op_type == OpType::CP_SRAM2TMEM;
}

static bool is_load_like_op(const DAGNode* node) {
    return node->op_type == OpType::LD_SRAM2REG ||
           node->op_type == OpType::LD_SRAM2REG_DSM ||
           node->op_type == OpType::LD_DRAM2REG ||
           node->op_type == OpType::CP_DRAM2SRAM ||
           node->op_type == OpType::TCGEN05_LD ||
           node->op_type == OpType::CP_SRAM2TMEM;
}

static bool is_store_like_op(const DAGNode* node) {
    return node->op_type == OpType::ST_REG2SRAM ||
           node->op_type == OpType::ST_REG2SRAM_DSM ||
           node->op_type == OpType::ST_REG2DRAM ||
           node->op_type == OpType::TCGEN05_ST ||
           node->op_type == OpType::CP_SRAM2DRAM;
}

static bool is_memory_op(const DAGNode* node) {
    return node->op_type == OpType::LD_SRAM2REG ||
           node->op_type == OpType::LD_SRAM2REG_DSM ||
           node->op_type == OpType::ST_REG2SRAM ||
           node->op_type == OpType::ST_REG2SRAM_DSM ||
           node->op_type == OpType::LD_DRAM2REG ||
           node->op_type == OpType::ST_REG2DRAM ||
           node->op_type == OpType::CP_DRAM2SRAM ||
           node->op_type == OpType::CP_SRAM2DRAM ||
           node->op_type == OpType::CP_DRAM2SRAM_TMA ||
           node->op_type == OpType::CP_SRAM2DRAM_TMA ||
           node->op_type == OpType::CP_DRAM2SRAM_LDGSTS ||
           node->op_type == OpType::TCGEN05_LD ||
           node->op_type == OpType::TCGEN05_ST ||
           node->op_type == OpType::CP_SRAM2TMEM;
}

static bool is_bulk_copy_op(const DAGNode* node) {
    return node->op_type == OpType::CP_DRAM2SRAM_TMA ||
           node->op_type == OpType::CP_SRAM2DRAM_TMA ||
           node->op_type == OpType::CP_DRAM2SRAM_LDGSTS;
}

static bool is_tmem_op(const DAGNode* node) {
    return TMEMUnit::is_tmem_op(node);
}

static int memory_op_index(const DAGNode* node) {
    switch (node->op_type) {
        case OpType::LD_SRAM2REG:
            return 0;
        case OpType::ST_REG2SRAM:
            return 1;
        case OpType::LD_SRAM2REG_DSM:
            return 2;
        case OpType::ST_REG2SRAM_DSM:
            return 3;
        case OpType::LD_DRAM2REG:
            return 4;
        case OpType::ST_REG2DRAM:
            return 5;
        case OpType::CP_DRAM2SRAM:
            return 6;
        case OpType::CP_SRAM2DRAM:
            return 7;
        case OpType::CP_SRAM2TMEM:
            return 8;
        case OpType::TCGEN05_LD:
            return 9;
        case OpType::TCGEN05_ST:
            return 10;
        default:
            return -1;
    }
}

static int ceil_div(int numerator, int denominator) {
    if (denominator <= 0) return 0;
    return (numerator + denominator - 1) / denominator;
}

static int compute_total_subops(const DAGNode* node, const Pipeline* pipe,
                                const L2Cache* l2, const Memory* /*sram*/) {
    if (node->op_type == OpType::WGMMA) {
        return 1;
    }
    if (is_l2_dram_op(node)) {
        int count = l2->get_subop_count(*node);
        return (count > 0) ? count : 1;
    }

    if (node->op_type == OpType::LD_SRAM2REG ||
        node->op_type == OpType::ST_REG2SRAM ||
        node->op_type == OpType::LD_SRAM2REG_DSM ||
        node->op_type == OpType::ST_REG2SRAM_DSM) {
        int access_bytes = get_access_granularity_bytes(*node);
        return std::max(1, compute_sram_subops(*node, access_bytes));
    }
    if (node->op_type == OpType::CP_SRAM2TMEM ||
        node->op_type == OpType::TCGEN05_LD ||
        node->op_type == OpType::TCGEN05_ST) {
        // Model TMEM moves as single bulk transfer transactions.
        return 1;
    }

    if(node->op_type==OpType::MMA && pipe->tensor_issue_work_semantics==TensorIssueWorkSemantics::DECLARED_FMA_WORK && node->tensor_reduction_extent>0) return 1;
    int elements = node->tile.r * node->tile.c;
    return std::max(1, ceil_div(elements, pipe->pipeline_throughput));
}

void Scheduler::issue_node(DAGNode* node, Pipeline* pipe, Cycle current_cycle,
                           Memory* sram, L2Cache* l2, int issue_span_override) {
    if(before_node_issue)before_node_issue(node,current_cycle);
    if (cycle_overlap::active() && node->start == -1) cycle_overlap::on_first_issue(node, subpartition_id, current_cycle);
    if (auto* observation = scheduler_observer::active()) {
        ++observation->issue_commits;
        if (node->start == -1) ++observation->first_node_issue_commits;
        if (node->op_type == OpType::BARRIER) ++observation->barrier_issue_commits;
        else if (is_bulk_copy_op(node)) ++observation->bulk_issue_commits;
        else ++observation->pipeline_issue_commits;
    }
    // Group issue may mutate a peer whose SP has not run in this cycle yet.
    notify_event(current_cycle);
    (void)sram;
    int warp_id = node->warp_id;

    if (node->start == -1) {
        node->start = current_cycle;
    }
    if (node->issue_cycle < 0) {
        node->issue_cycle = current_cycle;
    }
    if (is_tmem_op(node) && tmem_unit) {
        tmem_unit->on_issue(node, current_cycle);
    }
    last_issued_warp = warp_id;
    last_issue_cycle[warp_id] = current_cycle;
    last_issued_pipeline[warp_id] = node->pipeline_type;

    if (node->op_type == OpType::BARRIER) {
        if (node->total_transactions == 0) {
            node->total_transactions = 1;
            node->pending_transactions = 1;
            node->next_transaction_index = 1;
        }
        node->issue_done = true;
        warp_head_index[warp_id]++;
        return;
    }

    if (is_bulk_copy_op(node)) {
        if (node->total_transactions == 0 && tma_unit) {
            int count = tma_unit->enqueue(node, current_cycle, l2);
            if(node->explicit_async_shared_service_v1) node->async_copy_phase=1;
            node->total_transactions = count;
            node->pending_transactions = count;
            node->next_transaction_index = count;
        }
        if (tma_unit) {
            tma_next_issue_cycle[warp_id] =
                current_cycle + tma_unit->get_issue_interval(node);
        }
        // Resolve issue dependencies as soon as the TMA node is issued/enqueued.
        node->issue_done = true;
        warp_head_index[warp_id]++;
        return;
    }

    if (node->total_transactions == 0) {
        node->total_transactions = compute_total_subops(node, pipe, l2, sram);

        if (is_memory_op(node)) {
            if (is_l2_dram_op(node)) {
                node->pending_transactions = l2->get_total_transaction_count(*node);
            } else {
                node->pending_transactions = node->total_transactions;
            }
        } else {
            node->pending_transactions = node->total_transactions;
        }
        node->next_transaction_index = 0;
    }

    int subop_index = node->next_transaction_index;
    node->next_transaction_index++;

    if (!is_memory_op(node) && issue_span_override < 0 && node->setup_latency > 0) {
        issue_span_override = node->setup_latency;
    }
    if(node->op_type==OpType::MMA && pipe->tensor_issue_work_semantics==TensorIssueWorkSemantics::DECLARED_FMA_WORK && node->tensor_reduction_extent>0) {
        const auto work=node->tensor_fma_work_per_subpartition();
        const auto width=std::max(1,pipe->pipeline_width);
        const auto span=(work+width-1)/width;
        if(span>INT32_MAX)throw std::overflow_error("MMA issue span overflow");
        issue_span_override=std::max(issue_span_override,static_cast<int>(span));
    }
    pipe->issue_node(node->id, subop_index, current_cycle, issue_span_override);
    if (cycle_overlap::active()) cycle_overlap::on_pipeline_issue(node, pipe, subpartition_id, subop_index, current_cycle, std::get<2>(pipe->pipeline_executing_nodes.back()));
    if (is_memory_op(node) && node->setup_latency > 0) {
        int idx = memory_op_index(node);
        if (idx >= 0) {
            throttle_memory_issue(idx, warp_id, current_cycle + node->setup_latency);
        }
    }

    if (node->next_transaction_index < node->total_transactions) {
        int delay = 1;
        if (node->setup_latency > 0) {
            delay = node->setup_latency;
        }
        node->ready_cycle = current_cycle + delay;
        enqueue_ready(node);
    } else {
        node->issue_done = true;
    }

    // Track progress for reporting
    warp_head_index[warp_id]++;
}

int tensor_issue_span(const DAGNode& node, const Pipeline& pipe) {
    const std::int64_t work =
        pipe.tensor_issue_work_semantics ==
                TensorIssueWorkSemantics::DECLARED_FMA_WORK
            ? node.tensor_fma_work_per_subpartition()
            : static_cast<std::int64_t>(node.tile.r) * node.tile.c;
    if (work <= 0) {
        throw std::logic_error(
            pipe.tensor_issue_work_semantics ==
                    TensorIssueWorkSemantics::DECLARED_FMA_WORK
                ? "declared-FMA Tensor issue requires a declared positive work unit"
                : "legacy Tensor issue requires a positive output tile");
    }
    const std::int64_t denominator = std::max(1, pipe.pipeline_width);
    const std::int64_t span = (work + denominator - 1) / denominator;
    if (span > std::numeric_limits<int>::max()) {
        throw std::overflow_error("Tensor issue span exceeds int range");
    }
    return static_cast<int>(span);
}

static bool is_wgmma_op(const DAGNode* node) {
    return node->op_type == OpType::WGMMA;
}

static bool is_tcgen05_mma_op(const DAGNode* node) {
    return node->op_type == OpType::TCGEN05_MMA;
}

static bool is_group_issued_op(const DAGNode* node) {
    return is_wgmma_op(node) || is_tcgen05_mma_op(node);
}

static int node_issue_group_size(const DAGNode* node) {
    if (node->issue_group_size > 0) {
        return node->issue_group_size;
    }
    if (is_wgmma_op(node)) {
        return 4;
    }
    return 1;
}

static int node_issue_group_leader_warp(const DAGNode* node) {
    if (node->issue_group_leader_warp >= 0) {
        return node->issue_group_leader_warp;
    }
    int group_size = node_issue_group_size(node);
    if (group_size <= 1) {
        return node->warp_id;
    }
    return node->warp_id - (node->warp_id % group_size);
}

static bool is_barrier_op(const DAGNode* node) {
    return node->op_type == OpType::BARRIER;
}

std::vector<DAGNode*> Scheduler::schedule(Cycle current_cycle, std::vector<Pipeline*>& pipelines,
                                          Memory* sram, L2Cache* l2) {
    auto* observation = scheduler_observer::active();
    scheduler_observer::begin_schedule(observation, current_cycle);
    ensure_pipeline_map();

    // Residency changes only at TB dispatch/retire. Reuse the event-maintained
    // candidate list instead of rebuilding it by scanning every warp each cycle.
    if (resident_warps.empty()) {
        // No warps assigned to this subpartition
        scheduler_observer::end_schedule(observation, 0, true);
        return {};
    }

    if (subpartition_id == 0 && peer_schedulers != nullptr) {
        for (int warp_id : resident_warps) {
            if (warp_id % 4 != 0) {
                continue;
            }
            auto& leader_queue = ready_warp[warp_id];
            while (!leader_queue.empty()) {
                DAGNode* front = leader_queue.front();
                if (front->finished ||
                    (front->total_transactions > 0 &&
                     front->next_transaction_index >= front->total_transactions)) {
                    leader_queue.pop_front();
                    continue;
                }
                break;
            }
            if (leader_queue.empty()) {
                continue;
            }
            DAGNode* leader_node = leader_queue.front();
            if (!is_group_issued_op(leader_node)) {
                continue;
            }
            int group_size = node_issue_group_size(leader_node);
            if (group_size <= 1) {
                continue;
            }
            int leader_warp = node_issue_group_leader_warp(leader_node);
            if (leader_warp != warp_id) {
                continue;
            }
            // Current simulator has 4 subpartition schedulers. Keep group-issue
            // support aligned with this lane model for now.
            if (group_size != 4) {
                continue;
            }

            if (observation) ++observation->group_preflight_attempts;
            std::vector<int> warp_ids(group_size, 0);
            std::vector<Scheduler*> scheds(group_size, nullptr);
            std::vector<DAGNode*> nodes(group_size, nullptr);
            std::vector<Pipeline*> pipes(group_size, nullptr);
            for (int i = 0; i < group_size; ++i) {
                int grouped_warp = warp_id + i;
                warp_ids[i] = grouped_warp;
                scheds[i] = (*peer_schedulers)[grouped_warp % 4];
            }
            bool ready = true;

            for (int i = 0; i < group_size; ++i) {
                Scheduler* sched = scheds[i];
                sched->ensure_pipeline_map();
                auto& q = sched->ready_warp[warp_ids[i]];
                while (!q.empty()) {
                    DAGNode* front = q.front();
                    if (front->finished ||
                        (front->total_transactions > 0 &&
                         front->next_transaction_index >= front->total_transactions)) {
                        q.pop_front();
                        continue;
                    }
                    break;
                }
                if (q.empty()) {
                    ready = false;
                    break;
                }
                DAGNode* n = q.front();
                if (!is_group_issued_op(n)) {
                    ready = false;
                    break;
                }
                if (node_issue_group_size(n) != group_size ||
                    node_issue_group_leader_warp(n) != leader_warp ||
                    n->op_type != leader_node->op_type) {
                    ready = false;
                    break;
                }
                Pipeline* p = sched->node_ready_and_pipe(n, current_cycle, pipelines, sram, l2);
                if (p == nullptr) {
                    ready = false;
                    break;
                }
                nodes[i] = n;
                pipes[i] = p;
            }

            if (!ready) {
                if (observation) ++observation->group_preflight_rejections;
                continue;
            }

            int issue_span = tensor_issue_span(*nodes[0], *pipes[0]);
            if (nodes[0]->setup_latency > 0) {
                issue_span =
                    pipes[0]->tensor_issue_work_semantics ==
                            TensorIssueWorkSemantics::DECLARED_FMA_WORK
                        ? std::max(issue_span, nodes[0]->setup_latency)
                        : nodes[0]->setup_latency;
            }
            std::vector<DAGNode*> issued;
            issued.reserve(group_size);
            for (int i = 0; i < group_size; ++i) {
                Scheduler* sched = scheds[i];
                auto& q = sched->ready_warp[warp_ids[i]];
                if (!q.empty() && q.front() == nodes[i]) {
                    q.pop_front();
                }
                sched->issue_node(nodes[i], pipes[i], current_cycle, sram, l2, issue_span);
                issued.push_back(nodes[i]);
            }
            scheduler_observer::end_schedule(observation, issued.size());
            return issued;
        }
    }

    if (schedule_policy == SchedulePolicy::RR) {
        int count = static_cast<int>(resident_warps.size());
        if (count > 0) {
            int start = rr_cursor % count;
            for (int i = 0; i < count; ++i) {
                int idx = (start + i) % count;
                int warp_id = resident_warps[idx];
                auto [node, pipe] = try_issue_from_warp(warp_id, current_cycle, pipelines, sram, l2);
                if (node != nullptr) {
                    issue_node(node, pipe, current_cycle, sram, l2);
                    rr_cursor = (idx + 1) % count;
                    scheduler_observer::end_schedule(observation, 1);
                    return {node};
                }
            }
        }
    } else {
#if !TILEGEN_HOST_LAZY_GTO_SORT
        // Build priority list: warps sorted by last_issue_cycle (oldest first)
        std::vector<int> ranked_warps = resident_warps;
        std::sort(ranked_warps.begin(), ranked_warps.end(),
                  [this](int a, int b) { return last_issue_cycle[a] < last_issue_cycle[b]; });
#endif

        // Step 1: GTO - Try the warp that issued last cycle (Greedy part)
        if (last_issued_warp != -1 &&
            last_issued_warp % 4 == subpartition_id &&
            warp_is_resident[last_issued_warp]) {
            auto [node, pipe] = try_issue_from_warp(last_issued_warp, current_cycle, pipelines, sram, l2);
            if (node != nullptr) {
                // Successfully issued from same warp - continue for locality
                issue_node(node, pipe, current_cycle, sram, l2);
                scheduler_observer::end_schedule(observation, 1);
                return {node};
            }
        }

#if TILEGEN_HOST_LAZY_GTO_SORT
        // A failed greedy query does not change last_issue_cycle/resident_warps.
        // Keep the exact original comparator, input order and repeated greedy
        // warp attempt in the ranked loop; ties retain the original std::sort.
        std::vector<int> ranked_warps = resident_warps;
        std::sort(ranked_warps.begin(), ranked_warps.end(),
                  [this](int a, int b) { return last_issue_cycle[a] < last_issue_cycle[b]; });
#endif
        // Step 2: GTO - Cannot continue from last warp, try other warps in order of staleness
        for (int warp_id : ranked_warps) {
            auto [node, pipe] = try_issue_from_warp(warp_id, current_cycle, pipelines, sram, l2);
            if (node != nullptr) {
                // Successfully issued from this warp
                issue_node(node, pipe, current_cycle, sram, l2);
                scheduler_observer::end_schedule(observation, 1);
                return {node};
            }
        }
    }

    // Step 3: No warp in this subpartition could issue - cycle is stalled
    scheduler_observer::end_schedule(observation, 0);
    return {};
}

} // namespace GTSim
