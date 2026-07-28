#ifndef MEMORY_H
#define MEMORY_H

#include "dag_node.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <array>
#include <list>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GTSim {

// Memory subsystem (SRAM) with queues and bandwidth modeling
class Memory {
public:
    int memory_latency;
    int memory_bandwidth;
    int memory_queue_depth;

    // Read queue (for ld.sram2reg and cp.dram2sram)
    std::vector<std::tuple<std::vector<int>, int, int>> memory_queue;  // (node_ids, bytes, complete_cycle)
    int memory_occupied_until;

    // Write queue (for st.reg2sram - separate port)
    std::vector<std::tuple<std::vector<int>, int, int>> memory_write_queue;
    int memory_write_queue_depth;
    int memory_write_bandwidth;
    int memory_write_occupied_until;

    // Bank conflict statistics
    int total_bank_conflict_cycles;
    int num_loads_with_conflicts;

    Memory(int latency, int bandwidth, int depth)
        : memory_latency(latency), memory_bandwidth(bandwidth),
          memory_queue_depth(depth), memory_occupied_until(0),
          memory_write_queue_depth(depth), memory_write_bandwidth(bandwidth),
          memory_write_occupied_until(0), total_bank_conflict_cycles(0),
          num_loads_with_conflicts(0) {}

    // Check if queue is full
    bool is_full(bool is_write = false) const {
        if (is_write) {
            return memory_write_queue.size() >= static_cast<size_t>(memory_write_queue_depth);
        } else {
            return memory_queue.size() >= static_cast<size_t>(memory_queue_depth);
        }
    }

    // Enqueue a memory request
    bool enqueue(int node_id, int bytes_val, int current_cycle,
                 int bank_conflict_factor = 0, bool is_write = false) {
        if (is_write) {
            return enqueue_write({node_id}, bytes_val, current_cycle, bank_conflict_factor);
        }
        // Track bank conflict statistics (only for reads)
        if (bank_conflict_factor > 0) {
            total_bank_conflict_cycles += bank_conflict_factor;
            num_loads_with_conflicts++;
        }
        return enqueue_read({node_id}, bytes_val, current_cycle, bank_conflict_factor);
    }

    // Process queues and return completed node IDs
    std::vector<int> step(int current_cycle) {
        std::vector<int> completed;

        // Process read queue
        std::vector<std::tuple<std::vector<int>, int, int>> remaining_reads;
        for (const auto& [node_ids, bytes_val, complete_cycle] : memory_queue) {
            if (current_cycle >= complete_cycle) {
                completed.insert(completed.end(), node_ids.begin(), node_ids.end());
            } else {
                remaining_reads.push_back({node_ids, bytes_val, complete_cycle});
            }
        }
        memory_queue = std::move(remaining_reads);

        // Process write queue
        std::vector<std::tuple<std::vector<int>, int, int>> remaining_writes;
        for (const auto& [node_ids, bytes_val, complete_cycle] : memory_write_queue) {
            if (current_cycle >= complete_cycle) {
                completed.insert(completed.end(), node_ids.begin(), node_ids.end());
            } else {
                remaining_writes.push_back({node_ids, bytes_val, complete_cycle});
            }
        }
        memory_write_queue = std::move(remaining_writes);

        return completed;
    }

private:
    // Enqueue read request
    bool enqueue_read(const std::vector<int>& node_ids, int bytes_val,
                      int current_cycle, int bank_conflict_factor = 0) {
        if (is_full(false)) return false;

        // Calculate timing: T_sum = T_frontend + T_queuing + T_data + T_bank_conflict
        int T_frontend = memory_latency;
        int T_queuing = 0;

        if (current_cycle + T_frontend < memory_occupied_until) {
            T_queuing = memory_occupied_until - (current_cycle + T_frontend);
        }

        int T_data = static_cast<int>(std::ceil(static_cast<double>(bytes_val) / memory_bandwidth));
        int T_bank_conflict = bank_conflict_factor;

        int T_sum = T_frontend + T_queuing + T_data + T_bank_conflict;
        int complete_cycle = current_cycle + T_sum;

        memory_occupied_until = current_cycle + T_frontend + T_queuing + T_data + T_bank_conflict;
        memory_queue.push_back({node_ids, bytes_val, complete_cycle});

        return true;
    }

    // Enqueue write request (no bank conflicts)
    bool enqueue_write(const std::vector<int>& node_ids, int bytes_val,
                       int current_cycle, int bank_conflict_factor = 0) {
        if (is_full(true)) return false;

        int T_frontend = memory_latency;
        int T_queuing = 0;

        if (current_cycle + T_frontend < memory_write_occupied_until) {
            T_queuing = memory_write_occupied_until - (current_cycle + T_frontend);
        }

        int T_data = static_cast<int>(std::ceil(static_cast<double>(bytes_val) / memory_write_bandwidth));
        int T_bank_conflict = bank_conflict_factor;
        int T_sum = T_frontend + T_queuing + T_data + T_bank_conflict;
        int complete_cycle = current_cycle + T_sum;

        memory_write_occupied_until = current_cycle + T_frontend + T_queuing + T_data + T_bank_conflict;
        memory_write_queue.push_back({node_ids, bytes_val, complete_cycle});

        return true;
    }
};

inline int tile_ndims(const Tile& tile) {
    return tile.ndims > 0 ? tile.ndims : 2;
}

inline bool layout_is_row_major(const DAGNode& node) {
    return node.layout == Layout::RowMajor;
}

inline int tile_total_elements(const Tile& tile) {
    if (tile.empty()) {
        return 0;
    }
    return tile.element_count();
}

inline void compute_matrix_strides(const DAGNode& node, int ndims, int* strides) {
    for (int i = 0; i < Tile::kMaxDims; ++i) {
        strides[i] = 0;
    }

    bool have_all = true;
    for (int i = 0; i < ndims; ++i) {
        if (node.matrix_strides[i] <= 0) {
            have_all = false;
            break;
        }
    }

    if (have_all) {
        for (int i = 0; i < ndims; ++i) {
            strides[i] = node.matrix_strides[i];
        }
        return;
    }

    if (ndims == 2 && node.matrix_leading_dim > 0) {
        if (layout_is_row_major(node)) {
            strides[0] = node.matrix_leading_dim;
            strides[1] = 1;
        } else {
            strides[0] = 1;
            strides[1] = node.matrix_leading_dim;
        }
        return;
    }

    if (layout_is_row_major(node)) {
        strides[ndims - 1] = 1;
        for (int i = ndims - 2; i >= 0; --i) {
            strides[i] = strides[i + 1] * node.tile.dim(i + 1);
        }
    } else {
        strides[0] = 1;
        for (int i = 1; i < ndims; ++i) {
            strides[i] = strides[i - 1] * node.tile.dim(i - 1);
        }
    }
}

inline bool has_explicit_strides(const DAGNode& node, int ndims) {
    for (int i = 0; i < ndims; ++i) {
        if (node.matrix_strides[i] <= 0) return false;
    }
    return ndims > 0;
}

inline void compute_dim_order(const DAGNode& node, const int* strides, int ndims, int* order) {
    for (int i = 0; i < ndims; ++i) {
        order[i] = i;
    }
    if (has_explicit_strides(node, ndims)) {
        // Smallest stride is the fastest-changing dimension.
        std::stable_sort(order, order + ndims, [&](int a, int b) {
            int sa = strides[a];
            int sb = strides[b];
            if (sa <= 0 && sb <= 0) return a < b;
            if (sa <= 0) return false;
            if (sb <= 0) return true;
            if (sa != sb) return sa < sb;
            return a < b;
        });
    } else {
        // Fall back to layout-based order.
        if (layout_is_row_major(node)) {
            std::reverse(order, order + ndims);
        }
    }
}

inline void compute_sram_address_params(const DAGNode& node, int& elem_bytes,
                                        int* strides, int& ndims) {
    elem_bytes = node.element_size_bytes();
    ndims = tile_ndims(node.tile);
    compute_matrix_strides(node, ndims, strides);
}

inline std::uint64_t compute_sram_element_addr(const DAGNode& node, int linear_index,
                                               int elem_bytes, const int* strides,
                                               int ndims) {
    std::uint64_t elem_index = 0;
    int remaining = linear_index;
    int order[Tile::kMaxDims];
    compute_dim_order(node, strides, ndims, order);
    for (int oi = 0; oi < ndims; ++oi) {
        int i = order[oi];
        int dim = node.tile.dim(i);
        int idx = (dim > 0) ? (remaining % dim) : 0;
        remaining = (dim > 0) ? (remaining / dim) : 0;
        elem_index += static_cast<std::uint64_t>(node.tile.off(i) + idx) *
                      static_cast<std::uint64_t>(strides[i]);
    }
    return elem_index * static_cast<std::uint64_t>(elem_bytes);
}

inline int get_access_granularity_bytes(const DAGNode& node) {
    if (node.memory_access_granularity_bytes > 0) {
        return node.memory_access_granularity_bytes;
    }
    return 4;
}

inline int compute_sram_subops(const DAGNode& node, int access_granularity_bytes,
                               std::vector<int>* subop_bytes = nullptr,
                               std::vector<int>* subop_conflicts = nullptr) {
    constexpr int kBankWidthBytes = 4;
    constexpr int kBankCount = 32;

    if (subop_bytes) {
        subop_bytes->clear();
    }
    if (subop_conflicts) {
        subop_conflicts->clear();
    }

    if (node.tile.empty()) {
        if (subop_bytes) {
            subop_bytes->push_back(0);
        }
        if (subop_conflicts) {
            subop_conflicts->push_back(0);
        }
        return 1;
    }

    int elem_bytes = 0;
    int ndims = 0;
    int strides[Tile::kMaxDims];
    compute_sram_address_params(node, elem_bytes, strides, ndims);
    int total_elements = tile_total_elements(node.tile);

    int access_bytes = access_granularity_bytes > 0 ? access_granularity_bytes : 4;
    int elems_per_thread = std::max(1, (access_bytes + elem_bytes - 1) / elem_bytes);
    int elems_per_subop = 32 * elems_per_thread;
    int subops = (total_elements + elems_per_subop - 1) / elems_per_subop;

    if (!subop_bytes && !subop_conflicts) {
        return std::max(1, subops);
    }

    for (int subop = 0; subop < subops; ++subop) {
        int base_idx = subop * elems_per_subop;
        int remaining = total_elements - base_idx;
        int elems_in_subop = std::min(remaining, elems_per_subop);
        if (subop_bytes) {
            subop_bytes->push_back(elems_in_subop * elem_bytes);
        }
        if (subop_conflicts) {
            int conflicts = 0;
            std::array<bool, kBankCount> bank_used{};
            bank_used.fill(false);
            std::unordered_map<std::uint64_t, int> word_counts;

            if (node.layout == Layout::Swizzled) {
                conflicts = 0;
            } else {
                for (int t = 0; t < 32; ++t) {
                    for (int e = 0; e < elems_per_thread; ++e) {
                        int linear = base_idx + t * elems_per_thread + e;
                        if (linear >= total_elements) {
                            break;
                        }
                        std::uint64_t addr =
                            compute_sram_element_addr(node, linear, elem_bytes, strides, ndims);
                        if (elem_bytes < kBankWidthBytes) {
                            std::uint64_t word_addr = addr / kBankWidthBytes;
                            int capacity = kBankWidthBytes / elem_bytes;
                            if (word_counts[word_addr] >= capacity) {
                                conflicts++;
                            }
                            word_counts[word_addr] += 1;
                        } else {
                            int words = (elem_bytes + kBankWidthBytes - 1) / kBankWidthBytes;
                            for (int w = 0; w < words; ++w) {
                                int bank = static_cast<int>(((addr + static_cast<std::uint64_t>(w) *
                                                             kBankWidthBytes) /
                                                             kBankWidthBytes) % kBankCount);
                                if (bank_used[bank]) {
                                    conflicts++;
                                } else {
                                    bank_used[bank] = true;
                                }
                            }
                        }
                    }
                }
            }
            subop_conflicts->push_back(conflicts);
        }
    }

    return std::max(1, subops);
}

inline int compute_sram_subop_bytes(const DAGNode& node, int access_granularity_bytes,
                                    int subop_index) {
    if (subop_index < 0) {
        return 0;
    }
    std::vector<int> subop_bytes;
    compute_sram_subops(node, access_granularity_bytes, &subop_bytes);
    if (subop_index >= static_cast<int>(subop_bytes.size())) {
        return 0;
    }
    return subop_bytes[static_cast<size_t>(subop_index)];
}

inline int compute_sram_subop_conflict(const DAGNode& node, int access_granularity_bytes,
                                       int subop_index) {
    if (subop_index < 0) {
        return 0;
    }
    if (node.layout == Layout::Swizzled) {
        return 0;
    }
    std::vector<int> subop_conflicts;
    compute_sram_subops(node, access_granularity_bytes, nullptr, &subop_conflicts);
    if (subop_index >= static_cast<int>(subop_conflicts.size())) {
        return 0;
    }
    return subop_conflicts[static_cast<size_t>(subop_index)];
}

struct CacheLineKey {
    int matrix_id;
    std::uint64_t line_addr;

    bool operator==(const CacheLineKey& other) const {
        return matrix_id == other.matrix_id && line_addr == other.line_addr;
    }
};

struct CacheLineKeyHash {
    std::size_t operator()(const CacheLineKey& key) const {
        std::size_t h1 = std::hash<int>{}(key.matrix_id);
        std::size_t h2 = std::hash<std::uint64_t>{}(key.line_addr);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

inline int compute_dram_subops(const DAGNode& node, int access_granularity_bytes) {
    if (node.tile.empty()) {
        return 1;
    }
    int elem_bytes = node.element_size_bytes();
    int access_bytes = access_granularity_bytes > 0 ? access_granularity_bytes : 4;
    int elems_per_thread = std::max(1, (access_bytes + elem_bytes - 1) / elem_bytes);
    int elems_per_subop = 32 * elems_per_thread;
    int total_elements = tile_total_elements(node.tile);
    int subops = (total_elements + elems_per_subop - 1) / elems_per_subop;
    return std::max(1, subops);
}

inline std::vector<CacheLineKey> compute_dram_subop_lines(const DAGNode& node,
                                                          int access_granularity_bytes,
                                                          int subop_index,
                                                          int line_size_bytes) {
    std::vector<CacheLineKey> lines;
    if (node.tile.empty() || subop_index < 0) {
        return lines;
    }
    int elem_bytes = 0;
    int ndims = 0;
    int strides[Tile::kMaxDims];
    compute_sram_address_params(node, elem_bytes, strides, ndims);
    int access_bytes = access_granularity_bytes > 0 ? access_granularity_bytes : 4;
    int elems_per_thread = std::max(1, (access_bytes + elem_bytes - 1) / elem_bytes);
    int elems_per_subop = 32 * elems_per_thread;
    int total_elements = tile_total_elements(node.tile);
    int base_idx = subop_index * elems_per_subop;
    if (base_idx >= total_elements) {
        return lines;
    }

    std::unordered_set<std::uint64_t> seen;
    for (int t = 0; t < 32; ++t) {
        for (int e = 0; e < elems_per_thread; ++e) {
            int linear = base_idx + t * elems_per_thread + e;
            if (linear >= total_elements) {
                break;
            }
            std::uint64_t addr =
                compute_sram_element_addr(node, linear, elem_bytes, strides, ndims);
            std::uint64_t line_addr = (addr / line_size_bytes) * line_size_bytes;
            if (seen.insert(line_addr).second) {
                lines.push_back({node.matrix_id, line_addr});
            }
        }
    }
    return lines;
}

inline int compute_dram_total_transactions(const DAGNode& node,
                                           int access_granularity_bytes,
                                           int line_size_bytes,
                                           int coalesce_factor) {
    int subops = compute_dram_subops(node, access_granularity_bytes);
    int total = 0;
    for (int subop = 0; subop < subops; ++subop) {
        int lines = static_cast<int>(
            compute_dram_subop_lines(node, access_granularity_bytes, subop, line_size_bytes).size());
        total += lines * std::max(1, coalesce_factor);
    }
    return std::max(1, total);
}

class DRAMModel {
public:
    struct Request {
        CacheLineKey key;
        int ready_cycle;
        bool is_writeback;
    };

    DRAMModel(int bandwidth_bytes_per_cycle, int latency_cycles, int line_size_bytes,
              double core_freq_mhz, double dram_freq_mhz)
        : bandwidth_bytes_per_cycle(bandwidth_bytes_per_cycle),
          latency_cycles(latency_cycles),
          line_size_bytes(line_size_bytes),
          freq_ratio(dram_freq_mhz / core_freq_mhz),
          budget_bytes(0.0) {}

    void enqueue(const CacheLineKey& key, int current_cycle, bool is_writeback) {
        requests.push_back({key, current_cycle + latency_cycles, is_writeback});
    }

    std::vector<CacheLineKey> step(int current_cycle) {
        budget_bytes += static_cast<double>(bandwidth_bytes_per_cycle) * freq_ratio;
        std::vector<CacheLineKey> completed;

        std::deque<Request> remaining;
        for (const auto& req : requests) {
            if (req.ready_cycle > current_cycle) {
                remaining.push_back(req);
                continue;
            }
            if (budget_bytes < line_size_bytes) {
                remaining.push_back(req);
                continue;
            }
            budget_bytes -= line_size_bytes;
            if (!req.is_writeback) {
                completed.push_back(req.key);
            }
        }
        requests = std::move(remaining);
        return completed;
    }

private:
    int bandwidth_bytes_per_cycle;
    int latency_cycles;
    int line_size_bytes;
    double freq_ratio;
    double budget_bytes;
    std::deque<Request> requests;
};

class L2Cache {
public:
    struct Transaction {
        int node_id;
        CacheLineKey key;
        bool is_write;
    };


    struct CacheLineState {
        bool dirty;
        std::list<CacheLineKey>::iterator lru_it;
    };

    struct MSHREntry {
        std::vector<Transaction> waiting;
        bool any_write;
        bool any_read;
    };

    L2Cache(int cache_size_bytes, int line_size_bytes, int hit_latency_cycles,
            int l2_bandwidth_bytes_per_cycle,
            int l2_write_bandwidth_bytes_per_cycle, int l2_queue_depth,
            bool bypass_cache,
            int dram_bandwidth_bytes_per_cycle, int dram_latency_cycles,
            double core_freq_mhz, double dram_freq_mhz)
        : line_size_bytes(line_size_bytes),
          hit_latency_cycles(hit_latency_cycles),
          l2_bandwidth_bytes_per_cycle(l2_bandwidth_bytes_per_cycle),
          l2_write_bandwidth_bytes_per_cycle(l2_write_bandwidth_bytes_per_cycle),
          l2_queue_depth(l2_queue_depth),
          bypass_cache(bypass_cache),
          max_lines(static_cast<size_t>(cache_size_bytes / line_size_bytes)),
          dram(dram_bandwidth_bytes_per_cycle, dram_latency_cycles, line_size_bytes,
               core_freq_mhz, dram_freq_mhz) {}


    bool can_accept_transaction() const {
        return l2_queue_read.size() + l2_queue_write.size() <
               static_cast<size_t>(l2_queue_depth);
    }

    int get_line_size_bytes() const {
        return line_size_bytes;
    }

    int get_transaction_count(const DAGNode& node) const {
        int access_bytes = get_access_granularity_bytes(node);
        int factor = get_coalesce_factor(node);
        return compute_dram_total_transactions(node, access_bytes, line_size_bytes, factor);
    }

    CacheLineKey get_transaction_key(const DAGNode& node, int index) const {
        if (index < 0) {
            return {node.matrix_id, 0};
        }
        int access_bytes = get_access_granularity_bytes(node);
        int subops = compute_dram_subops(node, access_bytes);
        int offset = index;
        for (int subop = 0; subop < subops; ++subop) {
            auto lines = get_subop_lines(node, subop);
            if (offset < static_cast<int>(lines.size())) {
                return lines[static_cast<size_t>(offset)];
            }
            offset -= static_cast<int>(lines.size());
        }
        return {node.matrix_id, 0};
    }

    bool enqueue_transaction(const DAGNode& node, bool is_write, int index) {
        if (!can_accept_transaction()) return false;
        CacheLineKey key = get_transaction_key(node, index);
        if (is_write) {
            l2_queue_write.push_back({node.id, key, is_write});
        } else {
            l2_queue_read.push_back({node.id, key, is_write});
        }
        return true;
    }

    bool enqueue_transaction_key(int node_id, const CacheLineKey& key, bool is_write) {
        if (!can_accept_transaction()) return false;
        if (is_write) {
            l2_queue_write.push_back({node_id, key, is_write});
        } else {
            l2_queue_read.push_back({node_id, key, is_write});
        }
        return true;
    }


    int get_subop_count(const DAGNode& node) const {
        int access_bytes = get_access_granularity_bytes(node);
        return compute_dram_subops(node, access_bytes);
    }

    int get_total_transaction_count(const DAGNode& node) const {
        int access_bytes = get_access_granularity_bytes(node);
        int factor = get_coalesce_factor(node);
        return compute_dram_total_transactions(node, access_bytes, line_size_bytes, factor);
    }

    std::vector<CacheLineKey> get_subop_lines(const DAGNode& node, int subop_index) const {
        int access_bytes = get_access_granularity_bytes(node);
        auto lines = compute_dram_subop_lines(node, access_bytes, subop_index, line_size_bytes);
        int factor = get_coalesce_factor(node);
        if (factor <= 1 || lines.empty()) {
            return lines;
        }
        std::vector<CacheLineKey> expanded;
        expanded.reserve(lines.size() * static_cast<size_t>(factor));
        for (const auto& line : lines) {
            for (int i = 0; i < factor; ++i) {
                expanded.push_back(line);
            }
        }
        return expanded;
    }

    std::vector<int> step(int current_cycle) {
        std::vector<int> completed;

        // Drain completion queue
        std::vector<std::pair<int, int>> remaining_completions;
        for (const auto& [node_id, complete_cycle] : completion_queue) {
            if (current_cycle >= complete_cycle) {
                completed.push_back(node_id);
            } else {
                remaining_completions.push_back({node_id, complete_cycle});
            }
        }
        completion_queue = std::move(remaining_completions);

        // Process L2 queue within bandwidth (read can borrow unused write budget)
        int total_budget_tx = l2_bandwidth_bytes_per_cycle / line_size_bytes;
        int write_budget_tx = l2_write_bandwidth_bytes_per_cycle / line_size_bytes;
        if (write_budget_tx > total_budget_tx) {
            write_budget_tx = total_budget_tx;
        }
        int read_budget_tx = total_budget_tx - write_budget_tx;

        const bool cache_enabled = (!bypass_cache && max_lines > 0);
        int writes_processed = 0;
        while (!l2_queue_write.empty() && writes_processed < write_budget_tx) {
            Transaction tx = l2_queue_write.front();
            l2_queue_write.pop_front();
            writes_processed++;

            if (cache_enabled) {
                auto cache_it = cache.find(tx.key);
                if (cache_it != cache.end()) {
                    touch_lru(tx.key);
                    if (tx.is_write) {
                        cache_it->second.dirty = true;
                    }
                    completion_queue.push_back({tx.node_id, current_cycle + hit_latency_cycles});
                    continue;
                }
            }

            auto mshr_it = mshr.find(tx.key);
            if (mshr_it == mshr.end()) {
                MSHREntry entry;
                entry.any_write = tx.is_write;
                entry.any_read = !tx.is_write;
                entry.waiting.push_back(tx);
                mshr.emplace(tx.key, entry);
                dram.enqueue(tx.key, current_cycle, false);
            } else {
                mshr_it->second.waiting.push_back(tx);
                if (tx.is_write) {
                    mshr_it->second.any_write = true;
                } else {
                    mshr_it->second.any_read = true;
                }
            }
        }

        int remaining_write_tx = write_budget_tx - writes_processed;
        int read_budget = read_budget_tx + remaining_write_tx;
        int reads_processed = 0;
        while (!l2_queue_read.empty() && reads_processed < read_budget) {
            Transaction tx = l2_queue_read.front();
            l2_queue_read.pop_front();
            reads_processed++;

            if (cache_enabled) {
                auto cache_it = cache.find(tx.key);
                if (cache_it != cache.end()) {
                    touch_lru(tx.key);
                    if (tx.is_write) {
                        cache_it->second.dirty = true;
                    }
                    completion_queue.push_back({tx.node_id, current_cycle + hit_latency_cycles});
                    continue;
                }
            }

            auto mshr_it = mshr.find(tx.key);
            if (mshr_it == mshr.end()) {
                MSHREntry entry;
                entry.any_write = tx.is_write;
                entry.any_read = !tx.is_write;
                entry.waiting.push_back(tx);
                mshr.emplace(tx.key, entry);
                dram.enqueue(tx.key, current_cycle, false);
            } else {
                mshr_it->second.waiting.push_back(tx);
                if (tx.is_write) {
                    mshr_it->second.any_write = true;
                } else {
                    mshr_it->second.any_read = true;
                }
            }
        }

        // Process DRAM completions
        auto dram_completed = dram.step(current_cycle);
        for (const auto& key : dram_completed) {
            auto mshr_it = mshr.find(key);
            if (mshr_it == mshr.end()) {
                continue;
            }

            if (cache_enabled && mshr_it->second.any_read) {
                insert_line(key, mshr_it->second.any_write, current_cycle);
            }
            for (const auto& tx : mshr_it->second.waiting) {
                completion_queue.push_back({tx.node_id, current_cycle + hit_latency_cycles});
            }
            mshr.erase(mshr_it);
        }

        return completed;
    }

private:
    int line_size_bytes;
    int hit_latency_cycles;
    int l2_bandwidth_bytes_per_cycle;
    int l2_write_bandwidth_bytes_per_cycle;
    int l2_queue_depth;
    bool bypass_cache;
    size_t max_lines;

    std::list<CacheLineKey> lru_list;
    std::unordered_map<CacheLineKey, CacheLineState, CacheLineKeyHash> cache;
    std::unordered_map<CacheLineKey, MSHREntry, CacheLineKeyHash> mshr;
    std::deque<Transaction> l2_queue_read;
    std::deque<Transaction> l2_queue_write;
    std::vector<std::pair<int, int>> completion_queue;
    DRAMModel dram;
    std::vector<CacheLineKey> build_transaction_lines(const DAGNode& node) const {
        std::vector<CacheLineKey> lines;
        if (node.tile.empty()) {
            return lines;
        }

        int elem_bytes = node.element_size_bytes();
        int ndims = 0;
        int strides[Tile::kMaxDims];
        compute_sram_address_params(node, elem_bytes, strides, ndims);

        std::unordered_set<std::uint64_t> seen;
        auto push_line = [&](std::uint64_t line_addr) {
            if (seen.insert(line_addr).second) {
                lines.push_back({node.matrix_id, line_addr});
            }
        };

        if (ndims <= 2) {
            int ld = layout_is_row_major(node) ? strides[0] : strides[1];
            if (layout_is_row_major(node)) {
                int row_bytes = node.tile.c * elem_bytes;
                for (int r = 0; r < node.tile.r; ++r) {
                    std::uint64_t base_elem = static_cast<std::uint64_t>(node.tile.r_off + r) * ld +
                                              static_cast<std::uint64_t>(node.tile.c_off);
                    std::uint64_t base_addr = base_elem * static_cast<std::uint64_t>(elem_bytes);
                    std::uint64_t line_start = (base_addr / line_size_bytes) * line_size_bytes;
                    std::uint64_t offset = base_addr - line_start;
                    std::uint64_t total_bytes = offset + static_cast<std::uint64_t>(row_bytes);
                    int num_lines = static_cast<int>((total_bytes + line_size_bytes - 1) / line_size_bytes);
                    for (int i = 0; i < num_lines; ++i) {
                        push_line(line_start + static_cast<std::uint64_t>(i) * line_size_bytes);
                    }
                }
            } else {
                int col_bytes = node.tile.r * elem_bytes;
                for (int c = 0; c < node.tile.c; ++c) {
                    std::uint64_t base_elem = static_cast<std::uint64_t>(node.tile.c_off + c) * ld +
                                              static_cast<std::uint64_t>(node.tile.r_off);
                    std::uint64_t base_addr = base_elem * static_cast<std::uint64_t>(elem_bytes);
                    std::uint64_t line_start = (base_addr / line_size_bytes) * line_size_bytes;
                    std::uint64_t offset = base_addr - line_start;
                    std::uint64_t total_bytes = offset + static_cast<std::uint64_t>(col_bytes);
                    int num_lines = static_cast<int>((total_bytes + line_size_bytes - 1) / line_size_bytes);
                    for (int i = 0; i < num_lines; ++i) {
                        push_line(line_start + static_cast<std::uint64_t>(i) * line_size_bytes);
                    }
                }
            }
        } else {
            int total_elements = tile_total_elements(node.tile);
            for (int linear = 0; linear < total_elements; ++linear) {
                std::uint64_t addr = compute_sram_element_addr(node, linear, elem_bytes, strides, ndims);
                std::uint64_t line_addr = (addr / line_size_bytes) * line_size_bytes;
                push_line(line_addr);
            }
        }
        return lines;
    }


    std::uint64_t calculate_base_address(const DAGNode& node) const {
        int elem_bytes = node.element_size_bytes();
        int ndims = 0;
        int strides[Tile::kMaxDims];
        compute_sram_address_params(node, elem_bytes, strides, ndims);
        std::uint64_t base_element = 0;
        for (int i = 0; i < ndims; ++i) {
            base_element += static_cast<std::uint64_t>(node.tile.off(i)) *
                            static_cast<std::uint64_t>(strides[i]);
        }
        return base_element * static_cast<std::uint64_t>(elem_bytes);
    }

    void touch_lru(const CacheLineKey& key) {
        auto it = cache.find(key);
        if (it == cache.end()) return;
        lru_list.erase(it->second.lru_it);
        lru_list.push_front(key);
        it->second.lru_it = lru_list.begin();
    }

    void insert_line(const CacheLineKey& key, bool dirty, int current_cycle) {
        auto it = cache.find(key);
        if (it != cache.end()) {
            it->second.dirty = it->second.dirty || dirty;
            touch_lru(key);
            return;
        }

        if (cache.size() >= max_lines && !lru_list.empty()) {
            CacheLineKey victim = lru_list.back();
            lru_list.pop_back();
            auto victim_it = cache.find(victim);
            if (victim_it != cache.end()) {
                if (victim_it->second.dirty) {
                    dram.enqueue(victim, current_cycle, true);
                }
                cache.erase(victim_it);
            }
        }

        lru_list.push_front(key);
        cache.emplace(key, CacheLineState{dirty, lru_list.begin()});
    }

    int get_coalesce_factor(const DAGNode& node) const {
        int coalesce = node.memory_coalesce_bytes;
        if (coalesce <= 0) {
            coalesce = 128;
        }
        if (coalesce != 128 && coalesce != 64 && coalesce != 32) {
            std::cerr << "Invalid memory_coalesce_bytes=" << coalesce
                      << " for node " << node.name
                      << "; expected 128/64/32, defaulting to 128.\n";
            assert(false && "memory_coalesce_bytes must be 128/64/32");
            coalesce = 128;
        }
        if (coalesce > 128) {
            coalesce = 128;
        }
        int factor = 128 / coalesce;
        return std::max(1, factor);
    }
};

} // namespace GTSim

#endif // MEMORY_H
