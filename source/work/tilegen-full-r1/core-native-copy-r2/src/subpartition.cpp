#include "subpartition.h"
#include "cycle_overlap.h"
#include "noc.h"
#include <algorithm>
#include <utility>
#include <cstdio>

namespace GTSim {

static bool is_l2_dram_op(const DAGNode* node) {
    return node->op_type == OpType::LD_DRAM2REG ||
           node->op_type == OpType::ST_REG2DRAM ||
           node->op_type == OpType::CP_DRAM2SRAM ||
           node->op_type == OpType::CP_SRAM2DRAM;
}

static bool is_memory_op(const DAGNode* node) {
    return node->op_type == OpType::LD_SRAM2REG ||
           node->op_type == OpType::LD_SRAM2REG_DSM ||
           node->op_type == OpType::ST_REG2SRAM ||
           node->op_type == OpType::ST_REG2SRAM_DSM ||
           node->op_type == OpType::TCGEN05_LD ||
           node->op_type == OpType::TCGEN05_ST ||
           node->op_type == OpType::CP_SRAM2TMEM ||
           is_l2_dram_op(node);
}

static int compute_chunk_bytes(const DAGNode* node, int subop_index, int throughput_bytes) {
    int total_bytes = node->tile.r * node->tile.c * node->element_size_bytes();
    int offset = subop_index * throughput_bytes;
    int remaining = total_bytes - offset;
    if (remaining <= 0) return 0;
    return std::min(throughput_bytes, remaining);
}

static int get_pipeline_throughput(const Subpartition* sp, const std::string& name) {
    for (auto* pipe : sp->pipelines) {
        if (pipe->pipeline_name == name) {
            return pipe->pipeline_throughput;
        }
    }
    return 1;
}

static bool is_load_like_op(const DAGNode* node) {
    return node->op_type == OpType::LD_SRAM2REG ||
           node->op_type == OpType::LD_SRAM2REG_DSM ||
           node->op_type == OpType::LD_DRAM2REG ||
           node->op_type == OpType::CP_DRAM2SRAM ||
           node->op_type == OpType::CP_SRAM2TMEM;
}

static bool is_store_like_op(const DAGNode* node) {
    return node->op_type == OpType::ST_REG2SRAM ||
           node->op_type == OpType::ST_REG2SRAM_DSM ||
           node->op_type == OpType::ST_REG2DRAM ||
           node->op_type == OpType::TCGEN05_ST ||
           node->op_type == OpType::CP_SRAM2DRAM;
}

static int get_memory_pipeline_throughput(const Subpartition* sp, const DAGNode* node) {
    if (is_load_like_op(node)) {
        return get_pipeline_throughput(sp, "LD");
    }
    if (is_store_like_op(node)) {
        return get_pipeline_throughput(sp, "ST");
    }
    return get_pipeline_throughput(sp, node->pipeline_type);
}

void Subpartition::complete_node(DAGNode* node, Cycle current_cycle) {
    if (node->finished) {
        return;
    }
    // Completion may invalidate an old ready-queue entry even without children.
    host_issue_certificate.invalidate();
    if (cycle_overlap::active()) cycle_overlap::on_node_complete(node, subpartition_id, current_cycle);
    (*sp_scheduler->scoreboard)[node->id] = 1;
    node->finished = true;
    node->end = current_cycle;
    if (completed_node_count) {
        ++(*completed_node_count);
    }
    if (sp_scheduler->tmem_unit) {
        sp_scheduler->tmem_unit->on_complete(node);
    }

    // Count register usage
    sp_register->register_totally_used += node->register_change;

    for (auto* child : node->children) {
        if (child->remaining_deps > 0) {
            child->remaining_deps--;
            if (child->remaining_deps == 0) {
                child->ready_cycle = current_cycle + 1;
                if (node_id_to_sp && child->id >= 0 &&
                    child->id < static_cast<int>(node_id_to_sp->size())) {
                    if (auto* target_sp = (*node_id_to_sp)[child->id]) {
                        target_sp->sp_scheduler->enqueue_ready(child);
                    }
                }
            }
        }
    }
}

void Subpartition::resolve_issue_dependencies(DAGNode* node, Cycle current_cycle) {
    if (node->issue_deps_resolved) {
        return;
    }
    node->issue_deps_resolved = true;

    for (auto* child : node->issue_children) {
        if (child->remaining_deps > 0) {
            child->remaining_deps--;
            if (child->remaining_deps == 0) {
                child->ready_cycle = current_cycle + 1;
                if (node_id_to_sp && child->id >= 0 &&
                    child->id < static_cast<int>(node_id_to_sp->size())) {
                    if (auto* target_sp = (*node_id_to_sp)[child->id]) {
                        target_sp->sp_scheduler->enqueue_ready(child);
                    }
                }
            }
        }
    }
}

void Subpartition::begin_async_shared(DAGNode* node, Cycle current_cycle) {
    if (!node->explicit_async_shared_service_v1 || node->async_copy_phase!=1 ||
        node->pending_transactions!=0 || node->async_copy_shared_subops.empty())
        throw std::logic_error("invalid async global-to-shared transition");
    node->async_copy_phase=2;
    node->pending_transactions=static_cast<int>(node->async_copy_shared_subops.size());
    async_shared_ready.push_back({node->id,0});
    pending_memory_ops[node->id]=node->op;
    wake_event(current_cycle);
}

bool Subpartition::handle_memory_completion(int node_id, Cycle current_cycle) {
    if (event_mode) wake_event(current_cycle);
    auto it = sp_scheduler->node_dict.find(node_id);
    if (it != sp_scheduler->node_dict.end()) {
        DAGNode* node = it->second;
        if (node->pending_transactions > 0) {
            node->pending_transactions--;
            if (node->pending_transactions > 0) {
                return false;
            }
        }
        if(node->explicit_async_shared_service_v1) {
            if(node->async_copy_phase==1) {begin_async_shared(node,current_cycle);return false;}
            if(node->async_copy_phase!=2)throw std::logic_error("unexpected async shared response");
            node->async_copy_phase=3;
        }
        complete_node(node, current_cycle);
        pending_memory_ops.erase(node_id);
        return true;
    }
    return false;
}

void Subpartition::handle_tma_issue_complete(int node_id, Cycle current_cycle) {
    auto it = sp_scheduler->node_dict.find(node_id);
    if (it == sp_scheduler->node_dict.end()) {
        return;
    }
    DAGNode* node = it->second;
    if(node->explicit_async_shared_service_v1) {
        node->tma_issue_complete_cycle=current_cycle;
        if(node->total_transactions==0)begin_async_shared(node,current_cycle);
    }
    if (!node->issue_done) {
        node->issue_done = true;
        node->tma_issue_complete_cycle = current_cycle;
        resolve_issue_dependencies(node, current_cycle);
    }
}

bool Subpartition::try_enqueue_memory(DAGNode* node, int bytes_val, Cycle current_cycle,
                                       int bank_conflict_factor, int subop_index,
                                       const CacheLineKey& line_key, bool use_line_key,
                                       Memory* sram, Memory* tmem_mem, L2Cache* l2, L1ReadMissMemo* host_memo) {
    // Dispatch based on operation type
    if (node->op_type == OpType::LD_SRAM2REG) {
        // Check for remote SRAM access via NoC
        if (noc && node->target_sm_id >= 0 && node->target_sm_id != owning_sm_id) {
            // Cross-cluster remote access is not supported (DSMEM only within GPC)
            if (!noc->same_cluster(owning_sm_id, node->target_sm_id)) {
                static bool warned_cross_cluster = false;
                if (!warned_cross_cluster) {
                    std::fprintf(stderr, "[NoC] Warning: cross-cluster remote SRAM access "
                                 "rejected (SM %d -> SM %d, clusters %d vs %d). "
                                 "Falling back to local SRAM.\n",
                                 owning_sm_id, node->target_sm_id,
                                 noc->cluster_of(owning_sm_id),
                                 noc->cluster_of(node->target_sm_id));
                    warned_cross_cluster = true;
                }
                // Fall through to local SRAM path
            } else {
                return noc->enqueue(node->id, owning_sm_id, node->target_sm_id,
                                    bytes_val, false, bank_conflict_factor, current_cycle);
            }
        }
        // ld.sram2reg uses read queue with bank conflicts
        if (!sram->is_full(false)) {
            return sram->enqueue(node->id, bytes_val, current_cycle,
                               bank_conflict_factor, false);
        }
    } else if (node->op_type == OpType::CP_SRAM2TMEM) {
        // cp.sram2tmem models one-way shared-memory read into Tensor Memory.
        int write_extra = tmem_write_extra_latency_cycles;
        if (!tmem_mem->is_full(true)) {
            return tmem_mem->enqueue(node->id, bytes_val, current_cycle,
                                     write_extra, true);
        }
    } else if (node->op_type == OpType::TCGEN05_LD) {
        if (!tmem_mem->is_full(false)) {
            return tmem_mem->enqueue(node->id, bytes_val, current_cycle,
                                     bank_conflict_factor, false);
        }
    } else if (node->op_type == OpType::TCGEN05_ST) {
        int write_extra = tmem_write_extra_latency_cycles;
        if (!tmem_mem->is_full(true)) {
            return tmem_mem->enqueue(node->id, bytes_val, current_cycle,
                                     write_extra, true);
        }
    } else if (node->op_type == OpType::LD_SRAM2REG_DSM) {
        if (noc != nullptr) {
            int dst_sm_id = (node->target_sm_id >= 0) ? node->target_sm_id : owning_sm_id;
            return noc->enqueue(node->id, owning_sm_id, dst_sm_id,
                                bytes_val, false, bank_conflict_factor, current_cycle);
        }
    } else if (node->op_type == OpType::ST_REG2SRAM) {
        // Check for remote SRAM access via NoC
        if (noc && node->target_sm_id >= 0 && node->target_sm_id != owning_sm_id) {
            // Cross-cluster remote access is not supported (DSMEM only within GPC)
            if (!noc->same_cluster(owning_sm_id, node->target_sm_id)) {
                static bool warned_cross_cluster_st = false;
                if (!warned_cross_cluster_st) {
                    std::fprintf(stderr, "[NoC] Warning: cross-cluster remote SRAM store "
                                 "rejected (SM %d -> SM %d, clusters %d vs %d). "
                                 "Falling back to local SRAM.\n",
                                 owning_sm_id, node->target_sm_id,
                                 noc->cluster_of(owning_sm_id),
                                 noc->cluster_of(node->target_sm_id));
                    warned_cross_cluster_st = true;
                }
                // Fall through to local SRAM path
            } else {
                return noc->enqueue(node->id, owning_sm_id, node->target_sm_id,
                                    bytes_val, true, bank_conflict_factor, current_cycle);
            }
        }
        // st.reg2sram uses write queue
        if (!sram->is_full(true)) {
            return sram->enqueue(node->id, bytes_val, current_cycle,
                                 bank_conflict_factor, true);
        }
    } else if (node->op_type == OpType::ST_REG2SRAM_DSM) {
        if (noc != nullptr) {
            int dst_sm_id = (node->target_sm_id >= 0) ? node->target_sm_id : owning_sm_id;
            return noc->enqueue(node->id, owning_sm_id, dst_sm_id,
                                bytes_val, true, bank_conflict_factor, current_cycle);
        }
    } else if (is_l2_dram_op(node)) {
        if (l2->uses_whole_tiles()) return l2->enqueue_whole_tile(*node,current_cycle,owning_sm_id);
        if (l2_issue_cycle != current_cycle) {
            l2_issue_cycle = current_cycle;
            l2_issue_count = 0;
        }
        const bool allow_l2_forward =
            l2_max_transactions_per_cycle <= 0 ||
            l2_issue_count < l2_max_transactions_per_cycle;
        bool forwarded_to_l2 = false;
        bool is_write = (node->op_type == OpType::ST_REG2DRAM || node->op_type == OpType::CP_SRAM2DRAM);
        if (use_line_key) {
            if (l2->enqueue_transaction_key(*node, line_key, is_write,
                                            current_cycle, owning_sm_id,
                                            allow_l2_forward,
                                            &forwarded_to_l2, host_memo, subop_index)) {
                if (forwarded_to_l2) l2_issue_count++;
                return true;
            }
            return false;
        }
        if (l2->enqueue_transaction(*node, is_write, subop_index,
                                    current_cycle, owning_sm_id,
                                    allow_l2_forward,
                                    &forwarded_to_l2, host_memo)) {
            if (forwarded_to_l2) l2_issue_count++;
            return true;
        }
    }
    return false;
}

std::vector<int> Subpartition::step(Cycle current_cycle, Memory* sram, Memory* tmem_mem,
                                    L2Cache* l2) {
    if (l2_issue_cycle != current_cycle) {
        l2_issue_cycle = current_cycle;
        l2_issue_count = 0;
    }
    std::vector<int> non_memory_completions;

    if (event_mode && current_cycle < event_due) return {};
    ++host_service_calls;
    host_issue_certificate_active=TILEGEN_HOST_ISSUE_CERTIFICATE && event_mode &&
        noc==nullptr && !l2->uses_whole_tiles();
    if(!host_issue_certificate_active)host_issue_certificate.invalidate();
    // SRAM retries can raise the local issue throttle. Recompute conservatively.
    if(!retry_queue_sram.empty())host_issue_certificate.invalidate();
    // Step -1: Retry deferred 2-CTA UMMA completions when peer completion arrives.
    if (!deferred_tmem_pair_completions.empty() && sp_scheduler->tmem_unit) {
        std::vector<int> still_deferred;
        still_deferred.reserve(deferred_tmem_pair_completions.size());
        for (int node_id : deferred_tmem_pair_completions) {
            auto it = sp_scheduler->node_dict.find(node_id);
            if (it == sp_scheduler->node_dict.end()) {
                continue;
            }
            if (sp_scheduler->tmem_unit->consume_release_token(node_id)) {
                complete_node(it->second, current_cycle);
                non_memory_completions.push_back(node_id);
            } else {
                still_deferred.push_back(node_id);
            }
        }
        deferred_tmem_pair_completions.swap(still_deferred);
    }

    // Step 0: Complete barrier nodes
    while (!barrier_queue.empty()) {
        const auto& [node_id, complete_cycle] = barrier_queue.front();
        if (current_cycle < complete_cycle) break;
        auto it = sp_scheduler->node_dict.find(node_id);
        if (it != sp_scheduler->node_dict.end()) {
            complete_node(it->second, current_cycle);
            non_memory_completions.push_back(node_id);
        }
        barrier_queue.pop_front();
    }
    // Step 1: Retry any pending memory enqueues that failed due to full queue
    // Use remove_if to eliminate successful retries in-place (avoids temporary vector allocation)
    retry_queue_sram.erase(
        std::remove_if(retry_queue_sram.begin(), retry_queue_sram.end(),
            [&](const auto& retry) {
                const auto& [node_id, bytes_val, bank_conflict_factor, subop_index,
                             is_l2, ready_cycle, line_key, use_line_key, host_negative_memo] = retry;

                if (current_cycle < ready_cycle) {
                    return false;  // Not ready to retry yet
                }

                auto it = sp_scheduler->node_dict.find(node_id);
                if (it == sp_scheduler->node_dict.end()) {
                    return true;  // Remove invalid node
                }

                DAGNode* node = it->second;

                // Try to enqueue using helper function
                bool success = try_enqueue_memory(node, bytes_val, current_cycle,
                                                 bank_conflict_factor,
                                                 is_l2 ? subop_index : 0,
                                                 line_key, use_line_key, sram, tmem_mem, l2);

                if (success) {
                    pending_memory_ops[node->id] = node->op;
                    if (node->op_type == OpType::LD_SRAM2REG ||
                        node->op_type == OpType::LD_SRAM2REG_DSM) {
                        sp_scheduler->throttle_sram_issue(current_cycle + bank_conflict_factor);
                    }
                    return true;  // Remove from retry queue
                }
                return false;  // Keep in retry queue
            }),
        retry_queue_sram.end()
    );

    auto prefix_stamp=[&](){
        const bool allowed=l2->host_prefix_domain_allowed(owning_sm_id);
        return retry_host::PrefixStamp{l2,l2->host_prefix_domain_generation(),
            allowed?l2->host_prefix_ready_epoch(owning_sm_id):0,
            sp_scheduler->host_node_membership_generation,owning_sm_id,allowed};
    };
    auto blocked=[&](){return (l2_max_transactions_per_cycle>0 && l2_issue_count>=l2_max_transactions_per_cycle) || !l2->can_accept_transaction();};
    host_negative_prefix.run(retry_queue_l2,current_cycle,prefix_stamp,blocked,
        [&](RetryEntry& retry)->std::pair<bool,bool>{
            auto& [node_id,bytes_val,bank_conflict_factor,subop_index,
                   is_l2,ready_cycle,line_key,use_line_key,host_negative_memo]=retry;
            ++retry_host::counts.retry_entries_visited;
            if(current_cycle<ready_cycle)return {false,false};
            ++retry_host::counts.ready_retry_attempts;
            auto it=sp_scheduler->node_dict.find(node_id);
            if(it==sp_scheduler->node_dict.end())return {true,false};
            DAGNode* node=it->second;
            const bool success=try_enqueue_memory(node,bytes_val,current_cycle,
                bank_conflict_factor,is_l2?subop_index:0,line_key,use_line_key,
                sram,tmem_mem,l2,&host_negative_memo);
            if(success){
                pending_memory_ops[node->id]=node->op;
                if(node->op_type==OpType::LD_SRAM2REG || node->op_type==OpType::LD_SRAM2REG_DSM)
                    sp_scheduler->throttle_sram_issue(current_cycle+bank_conflict_factor);
                return {true,false};
            }
            // Certification is only a validated original native-line read
            // rejection. Writes, future entries and every other route stop it.
            const bool negative=retry_host::prefix_enabled && is_l2 && use_line_key &&
                node->op_type==OpType::LD_DRAM2REG && blocked() &&
                l2->host_current_negative_read(host_negative_memo,owning_sm_id,line_key);
            return {false,negative};
        });

    // Copy destinations use the existing SRAM write queue after every source
    // response is complete. No ST pipeline issue or invented DAG node.
    while(!async_shared_ready.empty()) {
        auto& q=async_shared_ready.front();
        auto it=sp_scheduler->node_dict.find(q.first);
        if(it==sp_scheduler->node_dict.end())throw std::logic_error("copy shared queue lost node");
        auto* node=it->second;
        if(node->async_copy_phase!=2)throw std::logic_error("copy shared queue phase");
        const auto svc=describe_explicit_sram_ranges(node->async_copy_shared_subops.at(q.second));
        if(sram->is_full(true) || !sram->enqueue(node->id,static_cast<int>(svc.service_bytes),
                    current_cycle,svc.extra_wavefronts,true))break;
        if(++q.second==node->async_copy_shared_subops.size())async_shared_ready.pop_front();
    }

    // Step 2: Step pipelines
    std::vector<std::pair<int, int>> all_completed;
    if(TILEGEN_HOST_PIPELINE_FRONT_CACHE && host_pipeline_front.dirty)
        host_pipeline_front.rebuild(pipelines);
    if(!TILEGEN_HOST_PIPELINE_FRONT_CACHE || current_cycle>=host_pipeline_front.earliest) {
        // Retain the original two phases: drain ALL pipeline FIFOs in vector
        // order before processing any completion or issuing new work.
        for (auto* pipe : pipelines) {
            auto completed = pipe->step(current_cycle);
            all_completed.insert(all_completed.end(), completed.begin(), completed.end());
        }
        if(TILEGEN_HOST_PIPELINE_FRONT_CACHE)host_pipeline_front.rebuild(pipelines);
    }

    // Step 3: Handle completed pipeline executions
    for (const auto& [node_id, subop_index] : all_completed) {
        auto it = sp_scheduler->node_dict.find(node_id);
        if (it == sp_scheduler->node_dict.end()) continue;

        DAGNode* node = it->second;

        // Use enum for fast dispatch
        if (is_memory_op(node)) {
            int bytes_val = 0;
            if (node->op_type == OpType::LD_SRAM2REG ||
                node->op_type == OpType::ST_REG2SRAM ||
                node->op_type == OpType::LD_SRAM2REG_DSM ||
                node->op_type == OpType::ST_REG2SRAM_DSM) {
                bytes_val = compute_sram_subop_bytes(*node, get_access_granularity_bytes(*node),
                                                     subop_index);
            } else if (node->op_type == OpType::CP_SRAM2TMEM ||
                       node->op_type == OpType::TCGEN05_LD ||
                       node->op_type == OpType::TCGEN05_ST) {
                // Bulk one-shot TMEM transfer size in bytes.
                bytes_val = node->tile.r * node->tile.c * node->element_size_bytes();
            } else {
                int throughput_bytes = get_memory_pipeline_throughput(this, node);
                bytes_val = compute_chunk_bytes(node, subop_index, throughput_bytes);
            }

            int bank_conflict_factor = 0;
            if (node->op_type == OpType::LD_SRAM2REG ||
                node->op_type == OpType::LD_SRAM2REG_DSM ||
                (node->op_type == OpType::ST_REG2SRAM &&
                 node->explicit_sram_bank_service_v1)) {
                bank_conflict_factor = compute_sram_subop_conflict(*node,
                                                                  get_access_granularity_bytes(*node),
                                                                  subop_index);
            }
            int setup_latency = 0;
            if (is_memory_op(node)) {
                setup_latency = std::max(0, node->setup_latency);
            }

            if (is_l2_dram_op(node) && l2->uses_whole_tiles()) {
                auto& count = whole_tile_frontend_completions[node_id];
                if (++count > node->total_transactions) throw std::logic_error("duplicate LS subop completion");
                if (count != node->total_transactions) continue;
                if (setup_latency > 0 || !try_enqueue_memory(node,bytes_val,current_cycle,
                        bank_conflict_factor,0,CacheLineKey{0,0},false,sram,tmem_mem,l2)) {
                    retry_queue_l2.push_back({node_id,bytes_val,bank_conflict_factor,0,true,
                                             current_cycle+setup_latency,CacheLineKey{0,0},false});
                } else pending_memory_ops[node_id]=node->op;
                continue;
            }
            if (is_l2_dram_op(node)) {
                auto lines = l2->get_native_subop_lines(*node, subop_index);
                if (lines.empty()) {
                    lines.push_back({node->matrix_id, 0});
                }
                for (const auto& line_key : lines) {
                    bool success = false;
                    if (setup_latency > 0) {
                        retry_queue_l2.push_back({node_id, bytes_val, bank_conflict_factor,
                                                  subop_index, true, current_cycle + setup_latency,
                                                  line_key, true});
                    } else {
                        success = try_enqueue_memory(node, bytes_val, current_cycle,
                                                     bank_conflict_factor, subop_index,
                                                     line_key, true, sram, tmem_mem, l2);
                    }

                    if (success) {
                        pending_memory_ops[node->id] = node->op;
                    } else {
                        if (setup_latency == 0) {
                            retry_queue_l2.push_back({node_id, bytes_val, bank_conflict_factor,
                                                      subop_index, true, current_cycle,
                                                      line_key, true});
                        }
                    }
                }
                continue;
            }

            // Try to enqueue using helper function (SRAM paths)
            bool success = false;
            if (setup_latency > 0) {
                retry_queue_sram.push_back({node_id, bytes_val, bank_conflict_factor,
                                            subop_index, false, current_cycle + setup_latency,
                                            CacheLineKey{0, 0}, false});
            } else {
                success = try_enqueue_memory(node, bytes_val, current_cycle,
                                             bank_conflict_factor, subop_index,
                                             CacheLineKey{0, 0}, false, sram, tmem_mem, l2);
            }

            if (success) {
                pending_memory_ops[node->id] = node->op;
                if (node->op_type == OpType::LD_SRAM2REG ||
                    node->op_type == OpType::LD_SRAM2REG_DSM ||
                    node->op_type == OpType::CP_SRAM2TMEM) {
                    sp_scheduler->throttle_sram_issue(current_cycle + bank_conflict_factor);
                }
            } else {
                if (setup_latency == 0) {
                    retry_queue_sram.push_back({node_id, bytes_val, bank_conflict_factor,
                                                subop_index, false, current_cycle,
                                                CacheLineKey{0, 0}, false});
                }
            }
        } else {
            if (node->pending_transactions > 0) {
                node->pending_transactions--;
            }
            if (node->pending_transactions <= 0) {
                if (sp_scheduler->tmem_unit &&
                    node->op_type == OpType::TCGEN05_MMA &&
                    !sp_scheduler->tmem_unit->on_mma_completion_arrival(node)) {
                    deferred_tmem_pair_completions.push_back(node_id);
                    continue;
                }
                complete_node(node, current_cycle);
                non_memory_completions.push_back(node_id);
            }
        }
    }

    // Schedule new nodes (uses O(1) event-driven warp residency checks)
    std::vector<DAGNode*> issued;
    if(host_issue_certificate_active && host_issue_certificate.excludes(current_cycle)) {
        // L2/SRAM retries and pipeline completions above still ran in source order.
        // Only a search known to be unable to issue is omitted.
        ++host_schedule_certificate_skips;
    } else {
        host_issue_certificate.invalidate();
        issued=sp_scheduler->schedule(current_cycle, pipelines, sram, l2);
    }
    for (auto* node : issued) {
        if (node->op_type == OpType::BARRIER) {
            int latency = std::max(0, node->setup_latency);
            barrier_queue.push_back({node->id, current_cycle + latency});
        }
        if (node->issue_done) {
            resolve_issue_dependencies(node, current_cycle);
        }
    }

    if (event_mode) event_due = next_event_cycle(current_cycle);
    return non_memory_completions;
}

Cycle Subpartition::next_event_cycle(Cycle current_cycle) {
    const Cycle floor = cycle_add(current_cycle, 1);
    Cycle next;
    if(host_issue_certificate_active && host_issue_certificate.excludes(current_cycle)) {
        next=host_issue_certificate.earliest;
        ++host_forecast_certificate_reuses;
    } else {
        next=sp_scheduler->next_possible_issue_cycle(current_cycle);
        if(host_issue_certificate_active) {
            host_issue_certificate.publish(next);
            ++host_forecast_certificate_refreshes;
        }
    }
    auto due = [&](Cycle c) { next = std::min(next, std::max(floor, c)); };
    if (!deferred_tmem_pair_completions.empty() || !async_shared_ready.empty()) return floor;
    if(TILEGEN_HOST_FORECAST_READY_FLOOR && retry_host::ready_front_enabled &&
       !retry_queue_l2.empty() && retry_queue_l2.front().ready_cycle<=current_cycle) {
        // Every remaining term is max(floor,deadline). The original retry
        // shortcut already pins min to floor; retain its exact counter.
        ++retry_host::counts.next_event_ready_front_shortcuts;
        return floor;
    }
    for (const auto& q : barrier_queue) due(q.second);
    for (const auto& q : retry_queue_sram) due(q.ready_cycle);
    if(retry_host::ready_front_enabled && !retry_queue_l2.empty() && retry_queue_l2.front().ready_cycle<=current_cycle){
        ++retry_host::counts.next_event_ready_front_shortcuts;due(retry_queue_l2.front().ready_cycle);
    }else for(const auto& q:retry_queue_l2){++retry_host::counts.next_event_retry_entries_visited;due(q.ready_cycle);}
    if(TILEGEN_HOST_PIPELINE_FRONT_CACHE) {
        if(host_pipeline_front.dirty)host_pipeline_front.rebuild(pipelines);
        if(host_pipeline_front.earliest!=std::numeric_limits<Cycle>::max())
            due(host_pipeline_front.earliest);
    } else {
        for (auto* p : pipelines) if (!p->pipeline_executing_nodes.empty())
            due(std::get<2>(p->pipeline_executing_nodes.front()));
    }
    return next;
}

} // namespace GTSim
