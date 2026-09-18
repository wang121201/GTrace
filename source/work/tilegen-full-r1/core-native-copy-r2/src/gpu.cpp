#include "gpu.h"

namespace GTSim {

// Route a completed remote access back to the originating subpartition
static void route_completion_to_origin(int node_id, int src_sm_id,
                                        Cycle current_cycle,
                                        CtaSparseSlots<Subpartition*>* node_id_to_sp,
                                        std::vector<SM*>& sms) {
    if (!node_id_to_sp || node_id < 0 ||
        node_id >= static_cast<int>(node_id_to_sp->size())) {
        return;
    }
    auto* sp = (*node_id_to_sp)[node_id];
    if (!sp) return;

    bool finished = sp->handle_memory_completion(node_id, current_cycle);
    if (finished && src_sm_id >= 0 &&
        src_sm_id < static_cast<int>(sms.size())) {
        sms[src_sm_id]->on_node_complete(node_id);
    }
}

static bool enqueue_forward_to_destination_sram(
    const NoCRequest& req, Cycle current_cycle, std::vector<SM*>& sms,
    std::unordered_multimap<int, NoCPendingEntry>& pending_remote_reads,
    std::unordered_multimap<int, NoCPendingEntry>& pending_remote_writes) {
    int dst = req.dst_sm_id;
    if (dst < 0 || dst >= static_cast<int>(sms.size())) {
        return true;
    }

    bool enqueued = sms[dst]->sram->enqueue(req.node_id, req.bytes, current_cycle,
                                            req.bank_conflict, req.is_write);
    if (!enqueued) {
        return false;
    }

    NoCPendingEntry entry{req.node_id, req.src_sm_id, req.dst_sm_id,
                          req.bytes, req.bank_conflict};
    if (req.is_write) {
        pending_remote_writes.insert({req.node_id, entry});
    } else {
        pending_remote_reads.insert({req.node_id, entry});
    }
    return true;
}

void GPU::step(Cycle current_cycle) {
    if (event_mode) { event_clock = current_cycle; event_last_sm = -1; }
    // Step 1: Step L2/DRAM and handle completions (unchanged)
    auto completed = l2->step(current_cycle);
    for (int node_id : completed) {
        if (node_id_to_sp && node_id >= 0 &&
            node_id < static_cast<int>(node_id_to_sp->size())) {
            if (auto* sp = (*node_id_to_sp)[node_id]) {
                bool finished = sp->handle_memory_completion(node_id, current_cycle);
                if (finished && node_id_to_sm &&
                    node_id < static_cast<int>(node_id_to_sm->size())) {
                    int sm_id = (*node_id_to_sm)[node_id];
                    if (sm_id >= 0 && sm_id < static_cast<int>(sms.size())) {
                        sms[sm_id]->on_node_complete(node_id);
                    }
                }
            }
        }
    }

    // Step 2: Retry forward->SRAM failures (destination SRAM backpressure).
    if (noc) {
        size_t forward_retry_count = forward_to_sram_retry_queue.size();
        while (forward_retry_count-- > 0) {
            NoCForwardRetryEntry retry = forward_to_sram_retry_queue.front();
            forward_to_sram_retry_queue.pop_front();
            if (!enqueue_forward_to_destination_sram(
                    retry.req, current_cycle, sms,
                    pending_remote_reads, pending_remote_writes)) {
                forward_to_sram_retry_queue.push_back(retry);
            }
        }
    }

    // Step 3: Step NoC and handle completed NoC requests
    if (noc) {
        auto noc_completed = noc->step(current_cycle);
        for (const auto& req : noc_completed) {
            if (req.phase == NoCPhase::FORWARD) {
                // Forward complete: enqueue to target SM's SRAM.
                // If SRAM is full, keep request and retry next cycle.
                if (!enqueue_forward_to_destination_sram(
                        req, current_cycle, sms,
                        pending_remote_reads, pending_remote_writes)) {
                    forward_to_sram_retry_queue.push_back({req});
                }
            } else {
                // RETURN complete: read data arrived back at source SM
                route_completion_to_origin(req.node_id, req.src_sm_id,
                                           current_cycle, node_id_to_sp, sms);
            }
        }

        // Retry return-hop enqueue after NoC step so freed queue slots are
        // visible before new forward traffic is injected by SM pipelines.
        size_t return_retry_count = return_retry_queue.size();
        while (return_retry_count-- > 0) {
            NoCReturnRetryEntry retry = return_retry_queue.front();
            return_retry_queue.pop_front();
            const auto& entry = retry.entry;
            if (!noc->enqueue_return(entry.node_id, entry.src_sm_id,
                                     entry.dst_sm_id, entry.bytes,
                                     entry.bank_conflict, current_cycle)) {
                return_retry_queue.push_back(retry);
            }
        }
    }

    // Step 4: Step each SM
    // If NoC is enabled, collect unhandled SRAM completions (remote accesses)
    if (noc) {
        for (auto* sm_inst : sms) {
            std::vector<int> unhandled;
            sm_inst->step(current_cycle, l2, &unhandled);

            for (int node_id : unhandled) {
                // Check pending_remote_reads: need NoC return hop
                auto read_range = pending_remote_reads.equal_range(node_id);
                auto read_it = read_range.first;
                if (read_it != pending_remote_reads.end()) {
                    const auto& entry = read_it->second;
                    // Enqueue return: data travels from dst_sm back to src_sm
                    if (!noc->enqueue_return(entry.node_id, entry.src_sm_id,
                                             entry.dst_sm_id, entry.bytes,
                                             entry.bank_conflict, current_cycle)) {
                        return_retry_queue.push_back({entry});
                    }
                    pending_remote_reads.erase(read_it);
                    continue;
                }

                // Check pending_remote_writes: route directly to origin
                auto write_range = pending_remote_writes.equal_range(node_id);
                auto write_it = write_range.first;
                if (write_it != pending_remote_writes.end()) {
                    const auto& entry = write_it->second;
                    route_completion_to_origin(entry.node_id, entry.src_sm_id,
                                               current_cycle, node_id_to_sp, sms);
                    pending_remote_writes.erase(write_it);
                }
            }
        }
    } else if (event_mode) {
        // Preserve the original increasing-SM order within each cycle.
        while (!event_sms.empty() && event_sms.begin()->first <= current_cycle) {
            const int id = event_sms.begin()->second;
            event_sms.erase(event_sms.begin());
            event_sm_due[id] = std::numeric_limits<Cycle>::max();
            event_last_sm = id;
            sms[id]->step(current_cycle, l2);
            event_sms.erase({event_sm_due[id], id});
            event_sm_due[id] = std::numeric_limits<Cycle>::max();
            wake_sm(id, sms[id]->next_event_cycle(current_cycle));
        }
    } else {
        // No NoC: original path
        for (auto* sm_inst : sms) {
            sm_inst->step(current_cycle, l2);
        }
    }
    l2->end_cycle(current_cycle);
    if (event_mode) refresh_event_statistics();
}

void GPU::wake_sm(int id, Cycle when) {
    if (!event_dirty[id]) { event_dirty[id] = true; event_dirty_ids.push_back(id); }
    if (when == std::numeric_limits<Cycle>::max()) return;
    when = std::max(when, event_clock);
    if (id <= event_last_sm) when = std::max(when, cycle_add(event_clock, 1));
    if (when >= event_sm_due[id]) return;
    event_sms.erase({event_sm_due[id], id});
    event_sm_due[id] = when;
    event_sms.insert({when, id});
}

void GPU::initialize_events(Cycle current_cycle) {
    if (event_mode) return;
    if (noc || !l2->whole_tile_port) throw std::logic_error("local event driver requires whole-tile path without NoC");
    event_mode = true;
    event_clock = current_cycle;
    event_sm_due.assign(sms.size(), std::numeric_limits<Cycle>::max());
    event_sm_blocked.assign(sms.size(), 0);
    event_dirty.assign(sms.size(), false);
    for (std::size_t id = 0; id < sms.size(); ++id) {
        auto* sm = sms[id];
        sm->event_mode = true;
        sm->event_owner_wakeup = [this,id](Cycle c) { wake_sm(static_cast<int>(id),c); };
        sm->tma->event_mode = true;
        for (auto* sp : sm->sps) {
            sp->event_mode = true;
            sp->event_owner_wakeup = sm->event_owner_wakeup;
            sp->sp_scheduler->event_wakeup = [sp](Cycle c) { sp->wake_event(c); };
        }
        wake_sm(static_cast<int>(id), cycle_add(current_cycle,1));
    }
    refresh_event_statistics();
}

void GPU::refresh_event_statistics() {
    auto* port = l2->whole_tile_port;
    for (int id : event_dirty_ids) {
        std::uint64_t count = 0;
        for (auto* sp : sms[id]->sps) for (int w : sp->sp_scheduler->resident_warps)
            if (sp->sp_scheduler->ready_warp[w].empty() && port->warp_waiting(id,w)) ++count;
        event_blocked_warps -= event_sm_blocked[id];
        event_blocked_warps += count;
        event_sm_blocked[id] = count;
        event_dirty[id] = false;
    }
    event_dirty_ids.clear();
}

} // namespace GTSim
