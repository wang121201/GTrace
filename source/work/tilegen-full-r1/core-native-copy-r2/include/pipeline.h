#include "cycle.h"
#ifndef PIPELINE_H
#define PIPELINE_H

#include "model_semantics.h"
#include "host_pipeline_front.h"
#include <string>
#include <vector>
#include <deque>
#include <tuple>
#include <utility>
#include <cstdint>

namespace GTSim {

// Pipeline for executing operations (Tensor, SIMD, SFU, LS)
class Pipeline {
public:
    std::string pipeline_name;
    int pipeline_latency;
    int pipeline_throughput;
    int pipeline_width;
    TensorIssueWorkSemantics tensor_issue_work_semantics;
    std::deque<std::tuple<int, int, Cycle>> pipeline_executing_nodes;  // (node_id, subop_index, complete_cycle)
    Cycle pipeline_next_issue_cycle;
    host_pipeline::FrontMinimum<Cycle>* host_front_owner=nullptr;

    Pipeline(const std::string& name, int latency, int throughput, int width,
             TensorIssueWorkSemantics tensor_work_semantics =
                 TensorIssueWorkSemantics::LEGACY_OUTPUT_ELEMENTS)
        : pipeline_name(name), pipeline_latency(latency),
          pipeline_throughput(throughput),
          pipeline_width(width),
          tensor_issue_work_semantics(tensor_work_semantics),
          pipeline_next_issue_cycle(0) {}

    // Step function: returns list of completed (node_id, subop_index) pairs
    // Optimized: exploits FIFO property, only checks front of deque
    std::vector<std::pair<int, int>> step(Cycle current_cycle) {
        std::vector<std::pair<int, int>> completed;

        // Pop completed nodes from front (FIFO order guaranteed by fixed latency)
        while (!pipeline_executing_nodes.empty()) {
            const auto& [node_id, subop_index, complete_cycle] = pipeline_executing_nodes.front();

            if (current_cycle >= complete_cycle) {
                // Node completed, pop from front
                completed.push_back({node_id, subop_index});
                pipeline_executing_nodes.pop_front();
            } else {
                // Front node not ready, all subsequent nodes also not ready (FIFO)
                break;
            }
        }

        if(!completed.empty() && host_front_owner)host_front_owner->invalidate();
        return completed;
    }

    // Check if pipeline can issue new operation
    bool can_issue(Cycle current_cycle) const {
        return current_cycle >= pipeline_next_issue_cycle;
    }

    // Issue a new operation (appends to back, maintains FIFO order)
    void issue_node(int node_id, int subop_index, Cycle current_cycle, int issue_span_override = -1) {
        int issue_span;
        if (issue_span_override > 0) {
            issue_span = issue_span_override;
        } else {
            // Positive int inputs fit the original result domain, but their
            // ceil-division numerator can exceed INT_MAX (the SHFL profile
            // uses 2^30 for both rates). Preserve the formula in a wider type.
            issue_span = static_cast<int>((static_cast<std::int64_t>(pipeline_throughput) +
                                          pipeline_width - 1) / pipeline_width);
        }
        if (issue_span < 1) issue_span = 1;

        Cycle start_cycle = current_cycle + issue_span;
        Cycle complete_cycle = start_cycle + pipeline_latency;
        const bool was_empty=pipeline_executing_nodes.empty();
        pipeline_executing_nodes.push_back({node_id, subop_index, complete_cycle});
        if(was_empty && host_front_owner)host_front_owner->first_entry(complete_cycle);
        pipeline_next_issue_cycle = current_cycle + issue_span;
    }
};

} // namespace GTSim

#endif // PIPELINE_H
