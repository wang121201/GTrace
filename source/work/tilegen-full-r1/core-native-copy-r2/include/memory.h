#include "cycle.h"
#ifndef MEMORY_H
#define MEMORY_H

#include "dag_node.h"
#include "source_metadata.h"
#include "service_source.h"
#include "ef_insertion.h"
#include "shared_cache_policy.h"
#include "model_semantics.h"
#include "per_sm_l1.h"
#include "requested_l1_sectors.h"
#include "cache_geometry.h"
#include "p32_observation.h"
#include "dirty_sector_eval.h"
#include "writer_observer.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <array>
#include <list>
#include <limits>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GTSim {

// Memory subsystem (SRAM) with queues and bandwidth modeling
class Memory {
public:
    Cycle next_completion_cycle = std::numeric_limits<Cycle>::max();
    int memory_latency;
    int memory_bandwidth;
    int memory_queue_depth;

    // Read queue (for ld.sram2reg and cp.dram2sram)
    std::vector<std::tuple<std::vector<int>, int, Cycle>> memory_queue;  // (node_ids, bytes, complete_cycle)
    Cycle memory_occupied_until;

    // Write queue (for st.reg2sram - separate port)
    std::vector<std::tuple<std::vector<int>, int, Cycle>> memory_write_queue;
    int memory_write_queue_depth;
    int memory_write_bandwidth;
    Cycle memory_write_occupied_until;

    // Bank conflict statistics
    Cycle total_bank_conflict_cycles;
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
    bool enqueue(int node_id, int bytes_val, Cycle current_cycle,
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
    std::vector<int> step(Cycle current_cycle) {
        std::vector<int> completed;
        if (current_cycle < next_completion_cycle) return completed;
        next_completion_cycle = std::numeric_limits<Cycle>::max();

        // Process read queue
        std::vector<std::tuple<std::vector<int>, int, Cycle>> remaining_reads;
        for (const auto& [node_ids, bytes_val, complete_cycle] : memory_queue) {
            if (current_cycle >= complete_cycle) {
                completed.insert(completed.end(), node_ids.begin(), node_ids.end());
            } else {
                remaining_reads.push_back({node_ids, bytes_val, complete_cycle});
                next_completion_cycle = std::min(next_completion_cycle, complete_cycle);
            }
        }
        memory_queue = std::move(remaining_reads);

        // Process write queue
        std::vector<std::tuple<std::vector<int>, int, Cycle>> remaining_writes;
        for (const auto& [node_ids, bytes_val, complete_cycle] : memory_write_queue) {
            if (current_cycle >= complete_cycle) {
                completed.insert(completed.end(), node_ids.begin(), node_ids.end());
            } else {
                remaining_writes.push_back({node_ids, bytes_val, complete_cycle});
                next_completion_cycle = std::min(next_completion_cycle, complete_cycle);
            }
        }
        memory_write_queue = std::move(remaining_writes);

        return completed;
    }

private:
    // Enqueue read request
    bool enqueue_read(const std::vector<int>& node_ids, int bytes_val,
                      Cycle current_cycle, int bank_conflict_factor = 0) {
        if (is_full(false)) return false;

        // Calculate timing: T_sum = T_frontend + T_queuing + T_data + T_bank_conflict
        int T_frontend = memory_latency;
        Cycle T_queuing = 0;

        if (current_cycle + T_frontend < memory_occupied_until) {
            T_queuing = memory_occupied_until - (current_cycle + T_frontend);
        }

        int T_data = static_cast<int>(std::ceil(static_cast<double>(bytes_val) / memory_bandwidth));
        int T_bank_conflict = bank_conflict_factor;

        Cycle T_sum = T_frontend + T_queuing + T_data + T_bank_conflict;
        Cycle complete_cycle = current_cycle + T_sum;

        memory_occupied_until = current_cycle + T_frontend + T_queuing + T_data + T_bank_conflict;
        memory_queue.push_back({node_ids, bytes_val, complete_cycle});
        next_completion_cycle = std::min(next_completion_cycle, complete_cycle);

        return true;
    }

    // Enqueue write request (no bank conflicts)
    bool enqueue_write(const std::vector<int>& node_ids, int bytes_val,
                       Cycle current_cycle, int bank_conflict_factor = 0) {
        if (is_full(true)) return false;

        int T_frontend = memory_latency;
        Cycle T_queuing = 0;

        if (current_cycle + T_frontend < memory_write_occupied_until) {
            T_queuing = memory_write_occupied_until - (current_cycle + T_frontend);
        }

        int T_data = static_cast<int>(std::ceil(static_cast<double>(bytes_val) / memory_write_bandwidth));
        int T_bank_conflict = bank_conflict_factor;
        Cycle T_sum = T_frontend + T_queuing + T_data + T_bank_conflict;
        Cycle complete_cycle = current_cycle + T_sum;

        memory_write_occupied_until = current_cycle + T_frontend + T_queuing + T_data + T_bank_conflict;
        memory_write_queue.push_back({node_ids, bytes_val, complete_cycle});
        next_completion_cycle = std::min(next_completion_cycle, complete_cycle);

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

// Local SRAM service description. One explicit subop stays one warp LS entry;
// bank waves are serialized service, never fabricated scheduler instructions.
// Whole-warp bank scheduling is an approximation, not recovered Ada phases.
struct ExplicitSRAMService {
    std::uint64_t logical_lane_bytes = 0;
    std::uint64_t service_bytes = 0;
    std::uint64_t unique_words = 0;
    std::uint64_t word_accesses = 0;
    std::uint64_t same_word_collapses = 0;
    int wavefronts = 0;
    int ideal_wavefronts = 0;
    int extra_wavefronts = 0;
};

inline ExplicitSRAMService describe_explicit_sram_ranges(const ExplicitMemorySubop& subop) {
    if (subop.ranges.empty() || subop.ranges.size() > 32)
        throw std::invalid_argument("explicit SRAM service requires 1..32 lane ranges");
    std::array<bool,32> lanes{};
    // At most 32 lanes * 4 words; fixed storage avoids per-word heap work.
    std::array<std::uint64_t,128> words;
    std::array<int,32> banks{};
    ExplicitSRAMService out;
    for (const auto& range : subop.ranges) {
        const int lane = range.source_member_ordinal;
        if (lane < 0 || lane >= 32 || lanes[lane])
            throw std::invalid_argument("explicit SRAM service requires unique actual lane ordinals");
        lanes[lane] = true;
        if (range.offset_bytes % 4 ||
            (range.byte_count != 4 && range.byte_count != 8 && range.byte_count != 16) ||
            range.offset_bytes > std::numeric_limits<std::uint64_t>::max() - range.byte_count)
            throw std::invalid_argument("explicit SRAM service requires aligned 4/8/16-byte lane ranges");
        out.logical_lane_bytes += range.byte_count;
        for (std::uint64_t offset = 0; offset < range.byte_count; offset += 4) {
            const std::uint64_t word = (range.offset_bytes + offset) / 4;
            words[out.word_accesses++] = word;
        }
    }
    if (subop.requested_bytes != out.logical_lane_bytes)
        throw std::invalid_argument("explicit SRAM logical byte count differs from lane ranges");
    std::sort(words.begin(), words.begin() + out.word_accesses);
    for (std::uint64_t i = 0; i < out.word_accesses; ++i)
        if (i == 0 || words[i] != words[i-1]) {
            ++out.unique_words;
            out.wavefronts = std::max(out.wavefronts, ++banks[words[i] % 32]);
        }
    out.service_bytes = out.unique_words * 4;
    out.same_word_collapses = out.word_accesses - out.unique_words;
    out.ideal_wavefronts = static_cast<int>((out.service_bytes + 127) / 128);
    out.extra_wavefronts = std::max(0, out.wavefronts - out.ideal_wavefronts);
    return out;
}

inline ExplicitSRAMService describe_explicit_sram_service(const DAGNode& node,
                                                         int subop_index) {
    if (!node.explicit_sram_bank_service_v1 ||
        (node.op_type != OpType::LD_SRAM2REG && node.op_type != OpType::ST_REG2SRAM) ||
        (node.target_sm_id >= 0 && node.target_sm_id != node.sm_id))
        throw std::invalid_argument("explicit SRAM service requires opt-in local shared load/store");
    if (subop_index < 0 || subop_index >= static_cast<int>(node.explicit_memory_subops.size()))
        throw std::invalid_argument("explicit SRAM service subop index");
    return describe_explicit_sram_ranges(node.explicit_memory_subops[static_cast<std::size_t>(subop_index)]);
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

    if (node.explicit_sram_bank_service_v1) {
        if (node.explicit_memory_subops.empty())
            throw std::invalid_argument("explicit SRAM service requires explicit subops");
        for (std::size_t i = 0; i < node.explicit_memory_subops.size(); ++i) {
            const auto service = describe_explicit_sram_service(node, static_cast<int>(i));
            if (subop_bytes) subop_bytes->push_back(static_cast<int>(service.service_bytes));
            if (subop_conflicts) subop_conflicts->push_back(service.extra_wavefronts);
        }
        return static_cast<int>(node.explicit_memory_subops.size());
    }

    if (!node.explicit_memory_subops.empty()) {
        for (const auto& subop : node.explicit_memory_subops) {
            if (subop.requested_bytes > INT32_MAX) throw std::overflow_error("explicit SRAM bytes overflow");
            if (subop_bytes) subop_bytes->push_back(static_cast<int>(subop.requested_bytes));
            if (subop_conflicts) {
                int conflicts=0;std::array<bool,kBankCount> bank_used{};
                std::unordered_map<std::uint64_t,int> word_counts;
                if (node.layout != Layout::Swizzled) {
                    const int elem=node.element_size_bytes();
                    for(const auto& range:subop.ranges)for(std::uint64_t i=0;i<range.byte_count;i+=elem) {
                        const auto addr=range.offset_bytes+i;
                        if(elem<kBankWidthBytes) {auto&count=word_counts[addr/kBankWidthBytes];if(count>=kBankWidthBytes/elem)++conflicts;++count;}
                        else for(int w=0;w<(elem+kBankWidthBytes-1)/kBankWidthBytes;++w) {
                            const auto bank=(addr+std::uint64_t(w)*kBankWidthBytes)/kBankWidthBytes%kBankCount;
                            if(bank_used[bank])++conflicts;else bank_used[bank]=true;
                        }
                    }
                }
                subop_conflicts->push_back(conflicts);
            }
        }
        return static_cast<int>(node.explicit_memory_subops.size());
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
    if (!node.explicit_sram_bank_service_v1 && node.layout == Layout::Swizzled) {
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

// A synchronous borrowed view into immutable template subops. No DAGNode is
// allocated or retained. All source_subops/ranges need live only through enqueue.
struct L2LineRequest {
    int completion_token = -1;
    CacheLineKey native_key{-1,0}, cache_key{-1,0};
    bool is_write = false;
    Cycle cycle = 0;
    int sm_id = -1, subpartition_id = -1;
    bool bypass_l1 = false, allow_l2_forward = true;
    int source_matrix_id = -1, memory_coalesce_bytes = 128;
    std::span<const ExplicitMemorySubop> source_subops;
    int source_subop_index = -1;
};

// Metadata access remains at the same points in the shared enqueue body.
// The legacy facade preserves the exact old DAG path, including null-node and
// P32 observation behavior; the light facade explicitly rejects P32 observation.
struct DagLineMetadata {
    const DAGNode* node;
    int subop_index;
    void begin(bool) const noexcept {}
    bool bypass_l1() const { return node && node->async_copy_bypass_l1; }
    requested_l1_sectors::Coverage l1_requested(int matrix,std::uint64_t line,int line_bytes) const {
        return node ? requested_l1_sectors::clip(node->matrix_id,node->memory_coalesce_bytes,
            node->explicit_memory_subops,subop_index,matrix,line,line_bytes) : requested_l1_sectors::Coverage{};
    }
    source_memory::View source_semantics() const noexcept {
        return source_memory::select(node ? std::span<const ExplicitMemorySubop>(node->explicit_memory_subops) : std::span<const ExplicitMemorySubop>{},subop_index);
    }
    native_p32_observability::RequestMask request_mask(int matrix,
            std::uint64_t line,int line_bytes) const {
        return node ? native_p32_observability::request_mask(*node,matrix,line,line_bytes)
                    : native_p32_observability::RequestMask{};
    }
    bool first_request(native_p32_observability::RequestMask mask,
            std::uint64_t line) const {
        return node && mask.known && line ==
            (node->explicit_memory_subops[0].ranges.front().offset_bytes/128)*128;
    }
    std::uint8_t store_mask(int matrix,std::uint64_t line,
            DirtySectorEvalStatistics& stats) const {
        if (!node) {
            ++stats.unknown_store_rejections;
            throw std::invalid_argument("sector store requires explicit source byte ranges");
        }
        return explicit_store_sector_mask(*node,matrix,line,subop_index,stats);
    }
};
struct LightLineMetadata {
    const L2LineRequest& request;
    void begin(bool observes_p32) const {
        if (observes_p32) throw std::logic_error(
            "light line request does not support active P32 metadata observation");
    }
    bool bypass_l1() const { return request.bypass_l1; }
    requested_l1_sectors::Coverage l1_requested(int matrix,std::uint64_t line,int line_bytes) const {
        return requested_l1_sectors::clip(request.source_matrix_id,request.memory_coalesce_bytes,
            request.source_subops,request.source_subop_index,matrix,line,line_bytes);
    }
    source_memory::View source_semantics() const noexcept {
        return source_memory::select(request.source_subops,request.source_subop_index);
    }
    native_p32_observability::RequestMask request_mask(int,std::uint64_t,int) const {
        throw std::logic_error("light line request does not support active P32 metadata observation");
    }
    bool first_request(native_p32_observability::RequestMask,std::uint64_t) const {
        throw std::logic_error("light line request does not support active P32 metadata observation");
    }
    std::uint8_t store_mask(int matrix,std::uint64_t line,
            DirtySectorEvalStatistics& stats) const {
        return explicit_store_sector_mask_program(request.source_matrix_id,
            request.memory_coalesce_bytes,request.source_subops,matrix,line,
            request.source_subop_index,stats);
    }
};

// Optional session-level address transform. Existing workloads leave this
// unset and retain the native (matrix_id, line_addr) cache key. A continuous
// multi-kernel driver can instead map each native key into one canonical
// allocation namespace before lookup, avoiding false aliases between kernels.
class L2AddressTransform {
public:
    virtual ~L2AddressTransform() = default;
    virtual CacheLineKey transform(const DAGNode& node,
                                   const CacheLineKey& native_key) const = 0;
};

enum class L2DecisionKind : std::uint8_t {
    HIT = 1,
    MISS_ALLOCATE = 2,
    PENDING_FILL_MERGE = 3,
};

enum class L2EvictionKind : std::uint8_t {
    NONE = 0,
    CLEAN = 1,
    DIRTY = 2,
};

struct L2DecisionEvent {
    std::uint64_t decision_sequence;
    std::uint64_t accept_sequence;
    std::uint64_t accept_cycle;
    std::uint64_t decision_cycle;
    std::int64_t node_id;
    std::int64_t matrix_id;
    std::uint64_t line_addr;
    std::uint64_t fill_sequence;
    bool is_write;
    L2DecisionKind outcome;
    std::uint32_t read_queue_depth_after_accept;
    std::uint32_t write_queue_depth_after_accept;
    std::uint32_t mshr_entries_after_decision;
    std::uint32_t resident_lines_after_decision;
    std::uint32_t dram_queue_depth_after_decision;
    // The cache-facing key above may have been transformed into a stable
    // session allocation namespace.  Preserve the pre-transform key as
    // provenance only; it never participates in lookup, queueing, or timing.
    std::int64_t native_matrix_id;
    std::uint64_t native_line_addr;
};

struct L2FillEvent {
    std::uint64_t fill_sequence;
    std::uint64_t allocate_decision_sequence;
    std::uint64_t allocate_cycle;
    std::uint64_t complete_cycle;
    std::int64_t matrix_id;
    std::uint64_t line_addr;
    std::uint64_t waiter_count;
    bool any_read;
    bool any_write;
    bool inserted;
    L2EvictionKind eviction_kind;
    std::int64_t victim_matrix_id;
    std::uint64_t victim_line_addr;
    std::uint32_t resident_lines_after_completion;
    std::uint32_t mshr_entries_after_completion;
    std::uint32_t dram_queue_depth_after_completion;
};

class L2RuntimeObserver {
public:
    virtual ~L2RuntimeObserver() = default;
    virtual void on_l2_decision(const L2DecisionEvent& event) = 0;
    virtual void on_l2_fill(const L2FillEvent& event) = 0;
};

struct L2RuntimeStatistics {
    // pre_l1 is the scheduler-issued post-coalescing request population.  The
    // legacy accepted_* counters remain the exact L2-input population.
    std::uint64_t pre_l1_transactions = 0;
    std::uint64_t pre_l1_reads = 0;
    std::uint64_t pre_l1_writes = 0;
    std::uint64_t l1_bypassed_transactions = 0;
    std::uint64_t l1_read_hits = 0;
    std::uint64_t l1_read_misses = 0;
    std::uint64_t l1_write_hits = 0;
    std::uint64_t l1_write_misses = 0;
    std::uint64_t l1_filtered_reads = 0;
    std::uint64_t l1_evictions = 0;
    std::uint64_t l1_kernel_flushes = 0;
    std::uint64_t l1_flushed_lines = 0;
    std::uint64_t peak_l1_resident_lines = 0;
    std::uint64_t final_l1_resident_lines = 0;
    std::uint64_t l1_decision_order_fnv1a64 = 14695981039346656037ULL;
    std::uint64_t accepted_transactions = 0;
    std::uint64_t accepted_reads = 0;
    std::uint64_t accepted_writes = 0;
    std::uint64_t processed_transactions = 0;
    std::uint64_t processed_reads = 0;
    std::uint64_t processed_writes = 0;
    std::uint64_t read_hits = 0;
    std::uint64_t write_hits = 0;
    std::uint64_t read_miss_allocates = 0;
    std::uint64_t write_miss_allocates = 0;
    std::uint64_t read_pending_fill_merges = 0;
    std::uint64_t write_pending_fill_merges = 0;
    std::uint64_t fill_completions = 0;
    std::uint64_t cache_inserts = 0;
    std::uint64_t clean_evictions = 0;
    std::uint64_t dirty_evictions = 0;
    std::uint64_t dram_fill_bytes = 0, dram_writeback_bytes = 0;
    std::uint64_t dram_fill_completed_bytes = 0, dram_writeback_completed_bytes = 0;
    std::uint64_t dram_fill_requests = 0;
    std::uint64_t dram_fill_completions = 0;
    std::uint64_t dram_writeback_requests = 0;
    std::uint64_t dram_writeback_completions = 0;
    std::uint64_t peak_l2_read_queue = 0;
    std::uint64_t peak_l2_write_queue = 0;
    std::uint64_t peak_l2_combined_queue = 0;
    std::uint64_t peak_completion_queue = 0;
    std::uint64_t peak_mshr_entries = 0;
    std::uint64_t peak_resident_lines = 0;
    std::uint64_t peak_dram_queue = 0;
    std::uint64_t final_l2_read_queue = 0;
    std::uint64_t final_l2_write_queue = 0;
    std::uint64_t final_completion_queue = 0;
    std::uint64_t final_mshr_entries = 0;
    std::uint64_t final_resident_lines = 0;
    std::uint64_t final_dram_queue = 0;
    std::uint64_t decision_order_fnv1a64 = 14695981039346656037ULL;
    std::uint64_t fill_order_fnv1a64 = 14695981039346656037ULL;
};

inline int compute_dram_subops(const DAGNode& node, int access_granularity_bytes) {
    if (!node.explicit_memory_subops.empty()) return static_cast<int>(node.explicit_memory_subops.size());
    if (node.has_explicit_global_line_span) {
        if (node.explicit_line_count == 0 ||
            node.explicit_line_stride_bytes == 0 ||
            node.explicit_row_length_lines == 0 ||
            node.explicit_row_stride_bytes == 0 ||
            node.explicit_lines_per_subop == 0) {
            throw std::invalid_argument("invalid explicit global-line span");
        }
        const std::uint64_t subops =
            (node.explicit_line_count + node.explicit_lines_per_subop - 1) /
            node.explicit_lines_per_subop;
        if (subops > static_cast<std::uint64_t>(
                         std::numeric_limits<int>::max())) {
            throw std::overflow_error("explicit global-line subop count overflow");
        }
        return static_cast<int>(subops);
    }
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
    if (subop_index < 0) return lines;
    if (!node.explicit_memory_subops.empty()) {
        if (line_size_bytes <= 0) throw std::invalid_argument("invalid explicit line width");
        if (subop_index >= static_cast<int>(node.explicit_memory_subops.size())) return lines;
        std::unordered_set<std::uint64_t> seen;
        for (const auto& range : node.explicit_memory_subops[subop_index].ranges) {
            if (!range.byte_count) continue;
            if (range.offset_bytes > UINT64_MAX - (range.byte_count - 1)) throw std::overflow_error("explicit range overflow");
            auto first=range.offset_bytes/std::uint64_t(line_size_bytes),last=(range.offset_bytes+range.byte_count-1)/std::uint64_t(line_size_bytes);
            for (auto line=first;line<=last;++line) if(seen.insert(line).second)
                lines.push_back({node.matrix_id,line*std::uint64_t(line_size_bytes)});
        }
        return lines;
    }
    if (node.has_explicit_global_line_span) {
        if (node.explicit_line_count == 0 ||
            node.explicit_line_stride_bytes == 0 ||
            node.explicit_row_length_lines == 0 ||
            node.explicit_row_stride_bytes == 0 ||
            node.explicit_lines_per_subop == 0 || line_size_bytes <= 0 ||
            node.explicit_first_line %
                    static_cast<std::uint64_t>(line_size_bytes) != 0 ||
            node.explicit_line_stride_bytes %
                    static_cast<std::uint64_t>(line_size_bytes) != 0 ||
            node.explicit_row_stride_bytes %
                    static_cast<std::uint64_t>(line_size_bytes) != 0) {
            throw std::invalid_argument("invalid explicit global-line span");
        }
        const std::uint64_t begin =
            static_cast<std::uint64_t>(subop_index) *
            node.explicit_lines_per_subop;
        if (begin >= node.explicit_line_count) return lines;
        const std::uint64_t count = std::min<std::uint64_t>(
            node.explicit_lines_per_subop,
            node.explicit_line_count - begin);
        lines.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index) {
            const std::uint64_t ordinal = begin + index;
            const std::uint64_t row =
                ordinal / node.explicit_row_length_lines;
            const std::uint64_t column =
                ordinal % node.explicit_row_length_lines;
            if (row >
                    (std::numeric_limits<std::uint64_t>::max() -
                     node.explicit_first_line) /
                        node.explicit_row_stride_bytes) {
                throw std::overflow_error("explicit global-line row address overflow");
            }
            const std::uint64_t row_address =
                node.explicit_first_line +
                row * node.explicit_row_stride_bytes;
            if (column >
                    (std::numeric_limits<std::uint64_t>::max() - row_address) /
                        node.explicit_line_stride_bytes) {
                throw std::overflow_error("explicit global-line column address overflow");
            }
            lines.push_back({
                node.matrix_id,
                row_address + column * node.explicit_line_stride_bytes});
        }
        return lines;
    }
    if (node.tile.empty()) return lines;
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

struct L2DramRuntimeStatistics {
    std::uint64_t fill_bytes = 0, writeback_bytes = 0;
    std::uint64_t fill_completed_bytes = 0, writeback_completed_bytes = 0;
    std::uint64_t fill_requests = 0;
    std::uint64_t fill_completions = 0;
    std::uint64_t writeback_requests = 0;
    std::uint64_t writeback_completions = 0;
    std::uint64_t peak_queue_depth = 0;
};

enum class L2DramRequestCause : std::uint8_t {
    FILL_READ = 0,
    DIRTY_WRITEBACK = 1,
};

struct L2DramRequest {
    std::uint64_t request_id = 0;
    std::uint64_t source_sequence = 0;
    std::uint64_t issue_cycle = 0;
    std::uint64_t issue_time_ps = 0;
    CacheLineKey key{-1, 0};
    std::uint64_t address = 0;
    std::uint32_t bytes = 0;
    std::int64_t node_id = 0;
    std::int32_t sm_id = 0;
    std::int32_t l2_subpartition_id = 0;
    L2DramRequestCause cause = L2DramRequestCause::FILL_READ;
};

struct L2DramCompletion {
    std::uint64_t request_id = 0;
    std::uint64_t source_sequence = 0;
    std::uint64_t issue_cycle = 0;
    std::uint64_t issue_time_ps = 0;
    std::uint64_t completion_cycle = 0;
    CacheLineKey key{-1, 0};
    bool is_writeback = false;
};

class L2DramAddressMapper {
public:
    virtual ~L2DramAddressMapper() = default;
    virtual std::uint64_t map(const CacheLineKey& key) const = 0;
};

// Exactly one completion backend owns every L2 miss and dirty writeback in a
// run. Passing a backend into L2Cache suppresses construction of the Internal
// DRAMModel entirely. Request identities and absolute issue time are assigned
// once at the L2 decision boundary and must be echoed by every completion.
class L2DramCompletionBackend {
public:
    virtual ~L2DramCompletionBackend() = default;
    virtual std::uint64_t issue_cycle_to_ps(
        std::uint64_t issue_cycle) const = 0;
    virtual void enqueue(const L2DramRequest& request) = 0;
    // False means no request/physical service was consumed. The caller keeps
    // the original identity and retries at a later real native cycle.
    // Legacy implementations remain source-compatible and never reject.
    virtual bool try_enqueue(const L2DramRequest& request,
                             std::uint64_t current_cycle) {
        (void)current_cycle;
        enqueue(request);
        return true;
    }
    virtual std::vector<L2DramCompletion> step(
        std::uint64_t current_cycle) = 0;
    virtual const L2DramRuntimeStatistics& statistics() const = 0;
    virtual std::size_t queue_depth() const = 0;
    // Zero means no finite bound is declared. Sector mode rejects that contract.
    virtual std::size_t admission_capacity() const { return 0; }
};

class DRAMModel final : public L2DramCompletionBackend {
public:
    using Statistics = L2DramRuntimeStatistics;

    struct Request {
        L2DramRequest request;
        std::uint64_t ready_cycle;
    };

    DRAMModel(int bandwidth_bytes_per_cycle, int latency_cycles, int line_size_bytes,
              double core_freq_mhz, double dram_freq_mhz,
              const MemoryModelSemantics& model_semantics = MemoryModelSemantics())
        : bandwidth_bytes_per_cycle(bandwidth_bytes_per_cycle),
          latency_cycles(latency_cycles),
          line_size_bytes(line_size_bytes),
          freq_ratio(model_semantics.dram_bandwidth ==
                         DramBandwidthSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE
                     ? 0.0
                     : dram_freq_mhz / core_freq_mhz),
          budget_bytes(0.0),
          model_semantics_(model_semantics) {
        model_semantics_.validate(line_size_bytes);
        if (model_semantics_.dram_bandwidth ==
            DramBandwidthSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE) {
            exact_budget_.configure(model_semantics_.dram_rate, line_size_bytes);
        } else if (bandwidth_bytes_per_cycle <= 0 || core_freq_mhz <= 0.0 ||
                   dram_freq_mhz <= 0.0) {
            throw std::invalid_argument(
                "legacy DRAM semantics require positive bandwidth and frequencies");
        }
    }

    std::uint64_t issue_cycle_to_ps(
            std::uint64_t issue_cycle) const override {
        // Internal DRAM does not consume physical picoseconds. Keeping the
        // monotonic cycle value here still preserves the common request
        // identity contract without inventing a calibrated clock.
        return issue_cycle;
    }

    void enqueue(const L2DramRequest& request) override {
        const bool is_writeback =
            request.cause == L2DramRequestCause::DIRTY_WRITEBACK;
        const int request_latency =
            !is_writeback && model_semantics_.l2_miss_latency ==
                    L2MissLatencySemantics::END_TO_END_FROM_MISS_DECISION
                ? model_semantics_.end_to_end_miss_latency_cycles
                : latency_cycles;
        requests.push_back({
            request,
            request.issue_cycle + static_cast<std::uint64_t>(request_latency)});
        if (is_writeback) {
            statistics_.writeback_requests += 1;
            statistics_.writeback_bytes += request.bytes;
        } else {
            statistics_.fill_requests += 1;
            statistics_.fill_bytes += request.bytes;
        }
        statistics_.peak_queue_depth = std::max(
            statistics_.peak_queue_depth,
            static_cast<std::uint64_t>(requests.size()));
    }

    // Preserve the direct DRAMModel test/API surface while all L2Cache traffic
    // uses the identity-bearing backend contract above.
    void enqueue(const CacheLineKey& key, Cycle current_cycle,
                 bool is_writeback) {
        L2DramRequest request;
        request.request_id = next_compat_request_id_;
        request.source_sequence = next_compat_request_id_;
        request.issue_cycle = static_cast<std::uint64_t>(
            std::max<Cycle>(0, current_cycle));
        request.issue_time_ps = issue_cycle_to_ps(request.issue_cycle);
        request.key = key;
        request.address = key.line_addr;
        request.bytes = static_cast<std::uint32_t>(line_size_bytes);
        request.cause = is_writeback
            ? L2DramRequestCause::DIRTY_WRITEBACK
            : L2DramRequestCause::FILL_READ;
        ++next_compat_request_id_;
        enqueue(request);
    }

    std::vector<L2DramCompletion> step(
            std::uint64_t current_cycle) override {
        std::vector<L2DramCompletion> completed;

        const bool exact_service =
            model_semantics_.dram_bandwidth ==
            DramBandwidthSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE;
        if (exact_service) {
            bool has_ready_request = false;
            for (const auto& req : requests) {
                if (req.ready_cycle <= current_cycle) {
                    has_ready_request = true;
                    break;
                }
            }
            if (!has_ready_request) {
                exact_budget_.reset();
                return completed;
            }
            exact_budget_.accrue_active_cycle();
        } else {
            // Historical path intentionally preserves fractional credit across
            // idle cycles for byte-identical compatibility with sealed runs.
            budget_bytes +=
                static_cast<double>(bandwidth_bytes_per_cycle) * freq_ratio;
        }

        std::deque<Request> remaining;
        for (const auto& req : requests) {
            if (req.ready_cycle > current_cycle) {
                remaining.push_back(req);
                continue;
            }
            const bool service_available = exact_service
                ? exact_budget_.can_consume_quantum()
                : budget_bytes >= line_size_bytes;
            if (!service_available) {
                remaining.push_back(req);
                continue;
            }
            if (exact_service) {
                exact_budget_.consume_quantum();
            } else {
                budget_bytes -= line_size_bytes;
            }
            const bool is_writeback = req.request.cause ==
                L2DramRequestCause::DIRTY_WRITEBACK;
            if (is_writeback) {
                statistics_.writeback_completions += 1;
                statistics_.writeback_completed_bytes += req.request.bytes;
            } else {
                statistics_.fill_completions += 1;
                statistics_.fill_completed_bytes += req.request.bytes;
            }
            completed.push_back({
                req.request.request_id,
                req.request.source_sequence,
                req.request.issue_cycle,
                req.request.issue_time_ps,
                current_cycle,
                req.request.key,
                is_writeback,
            });
        }
        requests = std::move(remaining);
        return completed;
    }

    const Statistics& statistics() const override {
        return statistics_;
    }

    std::size_t queue_depth() const override {
        return requests.size();
    }

private:
    int bandwidth_bytes_per_cycle;
    int latency_cycles;
    int line_size_bytes;
    double freq_ratio;
    double budget_bytes;
    MemoryModelSemantics model_semantics_;
    ExactRationalByteBudget exact_budget_;
    std::deque<Request> requests;
    Statistics statistics_;
    std::uint64_t next_compat_request_id_ = 0;
};

// Whole-node memory completion path. The ordinary line cache remains the default.
class WholeTileMemoryPort {
public:
    virtual ~WholeTileMemoryPort() = default;
    virtual bool enqueue(const DAGNode&, Cycle cycle, int sm) = 0;
    virtual std::vector<int> step(Cycle cycle) = 0;
    virtual void end_cycle(Cycle cycle) = 0;
    virtual bool is_quiescent() const = 0;
    virtual Cycle next_event_cycle() const = 0;
    virtual Cycle next_idle_event_cycle(Cycle limit) const { return std::min(limit,next_event_cycle()); }
    virtual bool skip_idle_cycles() const { return true; }
    virtual bool warp_waiting(int, int) const { return false; }
    virtual void account_memory_blocked_warps(std::uint64_t) {}
};

class L2Cache {
public:
    WholeTileMemoryPort* whole_tile_port = nullptr;
    bool uses_whole_tiles() const { return whole_tile_port != nullptr; }
    bool enqueue_whole_tile(const DAGNode& n, Cycle c, int sm) {
        if (!whole_tile_port) throw std::logic_error("whole tile port is absent");
        return whole_tile_port->enqueue(n,c,sm);
    }
    void end_cycle(Cycle c) { if (whole_tile_port) whole_tile_port->end_cycle(c); }

    struct Transaction {
        int node_id;
        CacheLineKey native_key;
        CacheLineKey key;
        bool is_write;
        std::uint64_t accept_sequence;
        Cycle accept_cycle;
        int sm_id;
        int l2_subpartition_id;
        std::uint32_t read_queue_depth_after_accept;
        std::uint32_t write_queue_depth_after_accept;
        ReadFillTicket read_ticket;
        native_p32_observability::RequestMask observation_mask;
        std::uint8_t dirty_mask = 0;
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
        source_memory::View source_evidence;
#endif
    };

    struct ReadCompletion {
        int node_id;
        Cycle complete_cycle;
        ReadFillTicket read_ticket;
    };

    struct CacheLineState {
        // Mode 0 stores 0/15; modes 1/2 retain exact four-sector dirtiness.
        std::uint8_t dirty;
        std::list<CacheLineKey>::iterator lru_it;
    };

    struct MSHREntry {
        std::vector<Transaction> waiting;
        bool any_write;
        bool any_read;
        std::uint8_t dirty_mask = 0;
        std::uint64_t fill_sequence;
        std::uint64_t allocate_decision_sequence;
        Cycle allocate_cycle;
        std::uint64_t dram_request_id;
    };

    L2Cache(int cache_size_bytes, int line_size_bytes, int hit_latency_cycles,
            int l2_bandwidth_bytes_per_cycle,
            int l2_write_bandwidth_bytes_per_cycle, int l2_queue_depth,
            bool bypass_cache,
            int dram_bandwidth_bytes_per_cycle, int dram_latency_cycles,
            double core_freq_mhz, double dram_freq_mhz,
            const MemoryModelSemantics& model_semantics = MemoryModelSemantics(),
            const PerSmL1Config& l1_config = PerSmL1Config(),
            L2DramCompletionBackend* completion_backend = nullptr,
            const L2DramAddressMapper* dram_address_mapper = nullptr,
            const L2GeometryConfig& l2_geometry = L2GeometryConfig())
        : line_size_bytes(line_size_bytes),
          hit_latency_cycles(hit_latency_cycles),
          l2_bandwidth_bytes_per_cycle(l2_bandwidth_bytes_per_cycle),
          l2_write_bandwidth_bytes_per_cycle(l2_write_bandwidth_bytes_per_cycle),
          l2_queue_depth(l2_queue_depth),
          bypass_cache(bypass_cache),
          max_lines(static_cast<size_t>(cache_size_bytes / line_size_bytes)),
          model_semantics_(model_semantics),
          l1_cache_(l1_config),
          lru_list(l2_geometry,cache_size_bytes,line_size_bytes),
          owned_dram_backend(nullptr),
          dram_backend(completion_backend),
          dram_address_mapper(dram_address_mapper),
          address_transform(nullptr),
          runtime_observer(nullptr),
          next_accept_sequence(0),
          next_decision_sequence(0),
          next_fill_sequence(0),
          next_dram_request_id(0) {
        if constexpr (kTilegenDirtySectorMode != 0) {
            if (line_size_bytes != 128 || bypass_cache || max_lines == 0)
                throw std::invalid_argument("dirty-sector evaluation requires enabled128B L2");
        }
        if constexpr (kTilegenDirtySectorMode == 2) {
            if (!dram_backend || dram_backend->admission_capacity() == 0 ||
                dram_backend->admission_capacity() > 4096)
                throw std::invalid_argument("sector writeback requires finite external backend capacity<=4096");
            // One rejected fill plus the remaining admitted fills can each
            // generate at most four independent 32B writes. No frontend service
            // resumes until this FIFO empties. This also bounds resident IDs.
            pending_dram_capacity_ = 4 * (dram_backend->admission_capacity() + 1);
        }
        if (dram_backend == nullptr) {
            owned_dram_backend = std::make_unique<DRAMModel>(
                dram_bandwidth_bytes_per_cycle, dram_latency_cycles,
                line_size_bytes, core_freq_mhz, dram_freq_mhz,
                model_semantics);
            dram_backend = owned_dram_backend.get();
        }
        model_semantics_.validate(line_size_bytes);
        if (l1_config.mode == PerSmL1Mode::MODELED_SET_ASSOCIATIVE &&
            l1_config.line_bytes != static_cast<std::uint32_t>(line_size_bytes)) {
            throw std::invalid_argument(
                "modeled per-SM L1 and L2 must use the same line size");
        }
        if (model_semantics_.l2_service ==
            L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE) {
            exact_l2_total_budget_.configure(
                model_semantics_.l2_total_rate, line_size_bytes);
            exact_l2_write_budget_.configure(
                model_semantics_.l2_write_rate, line_size_bytes);
        }
    }


    bool can_accept_transaction() const {
        return pending_dram_requests.empty() &&
               l2_queue_read.size() + l2_queue_write.size() <
               static_cast<size_t>(l2_queue_depth);
    }

    // External writer labels: quiescent entry/exit only, no modeled fields.
    void decode_writer_begin() const {
        auto* o=decode_writer::attached(this);
        if(!o || !is_quiescent() || kTilegenDirtySectorMode!=2)
            throw std::logic_error("writer begin needs attached mode2 quiescent L2");
        const auto d=dirty_sector_snapshot();const auto r=runtime_statistics();
        o->begin(max_lines+dram_backend->admission_capacity()+pending_dram_capacity_,
            {r.processed_writes,d.dirty_sector_creations,d.evicted_dirty_sectors,
             r.dram_writeback_bytes,r.dram_writeback_completed_bytes});
        for(const auto& [key,line]:cache)if(line.dirty)o->seed(key.matrix_id,key.line_addr,line.dirty);
        for(const auto& [key,line]:mshr)if(line.dirty_mask)o->seed(key.matrix_id,key.line_addr,line.dirty_mask);
    }
    void decode_writer_end() const {
        auto* o=decode_writer::attached(this);
        if(!o || !is_quiescent())throw std::logic_error("writer end needs attached quiescent L2");
        o->check_begin();
        for(const auto& [key,line]:cache)if(line.dirty)o->check_line(key.matrix_id,key.line_addr,line.dirty);
        for(const auto& [key,line]:mshr)if(line.dirty_mask)o->check_line(key.matrix_id,key.line_addr,line.dirty_mask);
        const auto d=dirty_sector_snapshot();const auto r=runtime_statistics();
        o->end({r.processed_writes,d.dirty_sector_creations,d.evicted_dirty_sectors,
            r.dram_writeback_bytes,r.dram_writeback_completed_bytes});
    }

    // Explicit whole-history owner: enable once at cold/quiescent entry.
    void enable_shared_cache(unsigned numerator=288) {
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
        if(shared_cache_state_||!is_quiescent()||statistics_.accepted_transactions!=0||
           !l1_cache_.config().sector_validity)
            throw std::logic_error("shared cache needs fresh quiescent sector-L1 history");
        shared_cache_state_=std::make_unique<shared_cache_policy::State>(numerator);
#else
        (void)numerator;throw std::logic_error("shared cache requires source evidence transport");
#endif
    }
    bool shared_cache_enabled() const {return bool(shared_cache_state_);}
    template<class J> J shared_cache_report() const {
        if(shared_cache_state_)return shared_cache_state_->template report<J>();
        return shared_cache_policy::State(0).template report<J>();
    }
    const DirtySectorEvalStatistics& dirty_sector_statistics() const { return dirty_sector_stats_; }
    DirtySectorEvalStatistics dirty_sector_snapshot() const {
        auto snapshot=dirty_sector_stats_;
        if constexpr (kTilegenDirtySectorMode != 0) {
            for(const auto& item:cache) {
                snapshot.resident_dirty_lines+=item.second.dirty!=0;
                snapshot.resident_dirty_sectors+=sector_popcount(item.second.dirty);
            }
            for(const auto& item:mshr) {
                snapshot.pending_dirty_lines+=item.second.dirty_mask!=0;
                snapshot.pending_dirty_sectors+=sector_popcount(item.second.dirty_mask);
            }
            for(const auto& item:outstanding_dram_requests)
                if(item.second.cause==L2DramRequestCause::DIRTY_WRITEBACK)
                    snapshot.outstanding_writeback_bytes+=item.second.bytes;
            for(const auto id:pending_dram_requests) {
                const auto& r=outstanding_dram_requests.at(id);
                if(r.cause==L2DramRequestCause::DIRTY_WRITEBACK)
                    snapshot.unadmitted_writeback_bytes+=r.bytes;
            }
            snapshot.dirty_sector_ledger_closed=snapshot.dirty_sector_creations==
                snapshot.resident_dirty_sectors+snapshot.pending_dirty_sectors+snapshot.evicted_dirty_sectors;
            const auto& backend=dram_backend->statistics();
            snapshot.writeback_byte_ledger_closed=backend.writeback_bytes>=backend.writeback_completed_bytes&&
                backend.writeback_bytes-backend.writeback_completed_bytes+snapshot.unadmitted_writeback_bytes==
                snapshot.outstanding_writeback_bytes;
        }
        return snapshot;
    }
    std::size_t pending_dram_admission_capacity() const { return pending_dram_capacity_; }
    std::size_t pending_dram_admission_count() const { return pending_dram_requests.size(); }
    std::uint64_t dram_admission_rejections() const { return dram_admission_rejections_; }
    std::uint64_t peak_pending_dram_admissions() const { return peak_pending_dram_admissions_; }

    int get_line_size_bytes() const {
        return line_size_bytes;
    }

    bool host_prefix_domain_allowed(int sm) const {
        const auto& c=l1_cache_.config();
        return !uses_whole_tiles() && address_transform==nullptr && c.mode==PerSmL1Mode::MODELED_SET_ASSOCIATIVE && sm>=0 && std::uint64_t(sm)<c.num_sms;
    }
    std::uint64_t host_prefix_domain_generation() const { return host_transform_generation_; }
    std::uint64_t host_prefix_ready_epoch(int sm) const { return l1_cache_.host_ready_epoch(sm); }
    bool host_current_negative_read(const L1ReadMissMemo& memo,int sm,const CacheLineKey& key) const {
        // The sole production caller invokes this immediately after the same
        // RetryEntry failed a real enqueue. That enqueue either remembered its
        // validated exact mask, or matched this memo against that exact mask.
        // This is certification of that rejection, not a general key lookup.
        if(l1_cache_.config().sector_validity&&!retry_host::enabled)return false;
        const auto mask=l1_cache_.config().sector_validity?memo.requested_sector_mask:std::uint8_t{15};
        return host_prefix_domain_allowed(sm) && l1_cache_.same_negative_read({sm,key.matrix_id,key.line_addr,false,0,false,mask},memo);
    }
    void set_address_transform(const L2AddressTransform* transform) {
        if(host_transform_generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("host transform generation overflow");
        ++host_transform_generation_;
        address_transform = transform;
    }

    void set_runtime_observer(L2RuntimeObserver* observer) {
        runtime_observer = observer;
    }

    // Independent observation sink. It cannot accept/complete requests or
    // modify modeled cache state, and cannot be attached to a warm history.
    void set_p32_observation(native_p32_observability::Observation* observer) {
        if constexpr (kTilegenDirtySectorMode == 2)
            throw std::logic_error("128B single-parent observer cannot observe split sector writebacks");
        if (!observer || p32_observation_ || line_size_bytes != 128 || bypass_cache ||
            max_lines == 0 || max_lines > 327680 || !is_quiescent() || !cache.empty() ||
            statistics_.processed_transactions != 0)
            throw std::logic_error("P32 observer requires one cold enabled128B cache attach");
        p32_observation_ = observer;
    }
    void audit_p32_observation() const {
        if (!p32_observation_) return;
        std::uint64_t pending=0,resident=0,inflight=0;
        for (const auto& x:mshr) pending += x.second.any_write;
        for (const auto& x:cache) resident += x.second.dirty != 0;
        for (const auto& x:outstanding_dram_requests)
            inflight += x.second.cause == L2DramRequestCause::DIRTY_WRITEBACK;
        p32_observation_->audit(pending,resident,inflight);
    }

    void begin_kernel() { l1_cache_.begin_kernel(); }

    const PerSmL1Config& per_sm_l1_config() const {
        return l1_cache_.config();
    }

    PerSmL1Statistics per_sm_l1_statistics() const {
        return l1_cache_.statistics();
    }

    ReadinessStatistics per_sm_l1_readiness() const {
        return l1_cache_.readiness();
    }

    ReadinessLine inspect_per_sm_l1(const PerSmL1Access& access) const {
        return l1_cache_.inspect(access);
    }

    L2RuntimeStatistics runtime_statistics() const {
        L2RuntimeStatistics result = statistics_;
        const auto l1_stats = l1_cache_.statistics();
        result.pre_l1_transactions = l1_stats.pre_l1_transactions;
        result.pre_l1_reads = l1_stats.pre_l1_reads;
        result.pre_l1_writes = l1_stats.pre_l1_writes;
        result.l1_bypassed_transactions = l1_stats.bypassed_transactions;
        result.l1_read_hits = l1_stats.read_hits;
        result.l1_read_misses = l1_stats.read_misses;
        result.l1_write_hits = l1_stats.write_hits;
        result.l1_write_misses = l1_stats.write_misses;
        result.l1_filtered_reads = l1_stats.filtered_reads;
        result.l1_evictions = l1_stats.evictions;
        result.l1_kernel_flushes = l1_stats.kernel_flushes;
        result.l1_flushed_lines = l1_stats.flushed_lines;
        result.peak_l1_resident_lines = l1_stats.peak_resident_lines;
        result.final_l1_resident_lines = l1_stats.final_resident_lines;
        result.l1_decision_order_fnv1a64 =
            l1_stats.decision_order_fnv1a64;
        const auto& dram_stats = dram_backend->statistics();
        result.dram_fill_bytes = dram_stats.fill_bytes;
        result.dram_writeback_bytes = dram_stats.writeback_bytes;
        result.dram_fill_completed_bytes = dram_stats.fill_completed_bytes;
        result.dram_writeback_completed_bytes = dram_stats.writeback_completed_bytes;
        result.dram_fill_requests = dram_stats.fill_requests;
        result.dram_fill_completions = dram_stats.fill_completions;
        result.dram_writeback_requests = dram_stats.writeback_requests;
        result.dram_writeback_completions = dram_stats.writeback_completions;
        result.peak_dram_queue = dram_stats.peak_queue_depth;
        result.final_l2_read_queue = l2_queue_read.size();
        result.final_l2_write_queue = l2_queue_write.size();
        result.final_completion_queue = completion_queue.size();
        result.final_mshr_entries = mshr.size();
        result.final_resident_lines = cache.size();
        result.final_dram_queue = dram_backend->queue_depth();
        return result;
    }

    bool is_quiescent() const {
        if (whole_tile_port) return whole_tile_port->is_quiescent();
        return l2_queue_read.empty() && l2_queue_write.empty() &&
               completion_queue.empty() && mshr.empty() &&
               outstanding_dram_requests.empty() &&
               l1_cache_.live_read_tickets() == 0 &&
               dram_backend->queue_depth() == 0;
    }

    bool uses_internal_dram_backend() const {
        return owned_dram_backend != nullptr &&
               dram_backend == owned_dram_backend.get();
    }

    int get_transaction_count(const DAGNode& node) const {
        int access_bytes = get_access_granularity_bytes(node);
        int factor = get_coalesce_factor(node);
        if(whole_tile_port && node.has_explicit_global_line_span) {
            const auto multiplier=static_cast<std::uint64_t>(std::max(1,factor));
            if(node.explicit_line_count>static_cast<std::uint64_t>(std::numeric_limits<int>::max())/multiplier)
                throw std::overflow_error("whole-tile frontend work overflow");
            return std::max(1,static_cast<int>(node.explicit_line_count*multiplier));
        }
        return compute_dram_total_transactions(node, access_bytes, line_size_bytes, factor);
    }

    CacheLineKey get_native_transaction_key(const DAGNode& node, int index) const {
        if (index < 0) {
            return {node.matrix_id, 0};
        }
        int access_bytes = get_access_granularity_bytes(node);
        int subops = compute_dram_subops(node, access_bytes);
        int offset = index;
        for (int subop = 0; subop < subops; ++subop) {
            auto lines = get_native_subop_lines(node, subop);
            if (offset < static_cast<int>(lines.size())) {
                return lines[static_cast<size_t>(offset)];
            }
            offset -= static_cast<int>(lines.size());
        }
        return {node.matrix_id, 0};
    }

    CacheLineKey get_transaction_key(const DAGNode& node, int index) const {
        return transform_key(node, get_native_transaction_key(node, index));
    }

    bool enqueue_transaction(const DAGNode& node, bool is_write, int index,
                             Cycle current_cycle = -1,
                             int request_sm_id = -1,
                             bool allow_l2_forward = true,
                             bool* forwarded_to_l2 = nullptr,
                             L1ReadMissMemo* host_memo = nullptr) {
        const CacheLineKey native_key = get_native_transaction_key(node, index);
        int explicit_subop_index = -1;
        if constexpr (kTilegenDirtySectorMode != 0) {
            if (is_write) {
                int remaining = index;
                for (int s = 0; s < static_cast<int>(node.explicit_memory_subops.size()); ++s) {
                    const auto count = get_native_subop_lines(node, s).size();
                    if (remaining >= 0 && static_cast<std::size_t>(remaining) < count) {
                        explicit_subop_index = s; break;
                    }
                    remaining -= static_cast<int>(count);
                }
            }
        }
        const CacheLineKey key = transform_key(node, native_key);
        const int sm_id = request_sm_id >= 0 ? request_sm_id : node.sm_id;
        const int subpartition_id = node.warp_id >= 0 ? node.warp_id % 4 : 0;
        return enqueue_pre_l1(node.id, native_key, key, is_write,
                              current_cycle, sm_id, subpartition_id,
                              allow_l2_forward,
                              forwarded_to_l2, &node, host_memo, explicit_subop_index);
    }

    bool enqueue_transaction_key(int node_id, const CacheLineKey& key, bool is_write,
                                 Cycle current_cycle = -1,
                                 int request_sm_id = -1,
                                 bool allow_l2_forward = true,
                                 bool* forwarded_to_l2 = nullptr,
                             L1ReadMissMemo* host_memo = nullptr) {
        return enqueue_pre_l1(node_id, key, key, is_write, current_cycle,
                              request_sm_id, 0, allow_l2_forward,
                              forwarded_to_l2, nullptr, host_memo);
    }

    bool enqueue_transaction_key(const DAGNode& node,
                                 const CacheLineKey& native_key,
                                 bool is_write,
                                 Cycle current_cycle = -1,
                                 int request_sm_id = -1,
                                 bool allow_l2_forward = true,
                                 bool* forwarded_to_l2 = nullptr,
                             L1ReadMissMemo* host_memo = nullptr,
                             int explicit_subop_index = -1) {
        const CacheLineKey key = transform_key(node, native_key);
        const int sm_id = request_sm_id >= 0 ? request_sm_id : node.sm_id;
        const int subpartition_id = node.warp_id >= 0 ? node.warp_id % 4 : 0;
        return enqueue_pre_l1(node.id, native_key, key, is_write,
                              current_cycle, sm_id, subpartition_id,
                              allow_l2_forward,
                              forwarded_to_l2, &node, host_memo, explicit_subop_index);
    }


    // One 128B cache-line request, not a DRAM burst or whole-packet request.
    // False creates no completion obligation. Accepted requests produce one
    // token occurrence from step(), after all original L1/L2 fill semantics.
    bool enqueue_line_request(const L2LineRequest& request,
                              bool* forwarded_to_l2 = nullptr,
                              L1ReadMissMemo* host_memo = nullptr) {
        if (forwarded_to_l2) *forwarded_to_l2 = false;
        if (whole_tile_port || address_transform)
            throw std::logic_error("light line request requires native line path without DAG address transform");
        if (line_size_bytes != 128 || request.completion_token < 0 ||
            request.cycle < 0 || request.sm_id < 0 ||
            static_cast<std::uint32_t>(request.sm_id) >= l1_cache_.config().num_sms ||
            request.subpartition_id < 0 || request.subpartition_id >= 4 ||
            request.native_key.matrix_id < 0 || request.cache_key.matrix_id < 0 ||
            request.native_key.line_addr % 128 || request.cache_key.line_addr % 128)
            throw std::invalid_argument("invalid light128B line request identity/placement/cycle");
        return enqueue_pre_l1_metadata(request.completion_token,
            request.native_key,request.cache_key,request.is_write,request.cycle,
            request.sm_id,request.subpartition_id,request.allow_l2_forward,
            forwarded_to_l2,LightLineMetadata{request},host_memo);
    }

    int get_subop_count(const DAGNode& node) const {
        if(!whole_tile_port && std::any_of(node.explicit_memory_subops.begin(),node.explicit_memory_subops.end(),[](const auto&s){return s.ranges.empty();}))
            throw std::logic_error("explicit masked memory requires whole-tile completion path");
        int access_bytes = get_access_granularity_bytes(node);
        return compute_dram_subops(node, access_bytes);
    }

    int get_total_transaction_count(const DAGNode& node) const {
        if (whole_tile_port) return 1;
        int access_bytes = get_access_granularity_bytes(node);
        int factor = get_coalesce_factor(node);
        return compute_dram_total_transactions(node, access_bytes, line_size_bytes, factor);
    }

    std::vector<CacheLineKey> get_native_subop_lines(
            const DAGNode& node, int subop_index) const {
        int access_bytes = get_access_granularity_bytes(node);
        auto lines = compute_dram_subop_lines(node, access_bytes, subop_index, line_size_bytes);
        int factor = get_coalesce_factor(node);
        if (factor > 1 && !lines.empty()) {
            std::vector<CacheLineKey> expanded;
            expanded.reserve(lines.size() * static_cast<size_t>(factor));
            for (const auto& line : lines) {
                for (int i = 0; i < factor; ++i) {
                    expanded.push_back(line);
                }
            }
            lines = std::move(expanded);
        }
        return lines;
    }

    std::vector<CacheLineKey> get_subop_lines(const DAGNode& node, int subop_index) const {
        auto lines = get_native_subop_lines(node, subop_index);
        for (auto& line : lines) {
            line = transform_key(node, line);
        }
        return lines;
    }

    std::vector<int> step(Cycle current_cycle) {
        if (whole_tile_port) return whole_tile_port->step(current_cycle);
        std::vector<int> completed;

        // All writers are private and update the minimum after a successful
        // push. Before that minimum, the original drain has no effects except
        // copying its unchanged survivors. Service and backend still run.
        if (current_cycle >= host_completion_due_) {
            Cycle next_due=std::numeric_limits<Cycle>::max();
            std::vector<ReadCompletion> remaining_completions;
            for (const auto& completion : completion_queue) {
                if (current_cycle >= completion.complete_cycle) {
                    l1_cache_.complete_read(completion.read_ticket);
                    completed.push_back(completion.node_id);
                } else {
                    remaining_completions.push_back(completion);
                    next_due=std::min(next_due,completion.complete_cycle);
                }
            }
            completion_queue = std::move(remaining_completions);
    
            host_completion_due_=next_due;
        }

        if (model_semantics_.l2_service ==
            L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE) {
            // Total service and the write sub-budget are independent exact
            // token buckets. Writes retain priority; reads borrow every total
            // token not consumed by a write, matching the legacy arbitration
            // policy without flooring bytes/cycle to whole lines per cycle.
            if (l2_queue_read.empty() && l2_queue_write.empty()) {
                exact_l2_total_budget_.reset();
                exact_l2_write_budget_.reset();
            } else {
                exact_l2_total_budget_.accrue_active_cycle();
                if (l2_queue_write.empty()) {
                    exact_l2_write_budget_.reset();
                } else {
                    exact_l2_write_budget_.accrue_active_cycle();
                }

                while (pending_dram_requests.empty() && !l2_queue_write.empty() &&
                       exact_l2_total_budget_.can_consume_quantum() &&
                       exact_l2_write_budget_.can_consume_quantum()) {
                    Transaction tx = l2_queue_write.front();
                    l2_queue_write.pop_front();
                    exact_l2_total_budget_.consume_quantum();
                    exact_l2_write_budget_.consume_quantum();
                    process_transaction(tx, current_cycle);
                }
                while (pending_dram_requests.empty() && !l2_queue_read.empty() &&
                       exact_l2_total_budget_.can_consume_quantum()) {
                    Transaction tx = l2_queue_read.front();
                    l2_queue_read.pop_front();
                    exact_l2_total_budget_.consume_quantum();
                    process_transaction(tx, current_cycle);
                }
            }
        } else {
            // Historical service path retained for sealed H100 evidence.
            int total_budget_tx = l2_bandwidth_bytes_per_cycle / line_size_bytes;
            int write_budget_tx =
                l2_write_bandwidth_bytes_per_cycle / line_size_bytes;
            if (write_budget_tx > total_budget_tx) {
                write_budget_tx = total_budget_tx;
            }
            int read_budget_tx = total_budget_tx - write_budget_tx;

            int writes_processed = 0;
            while (pending_dram_requests.empty() && !l2_queue_write.empty() && writes_processed < write_budget_tx) {
                Transaction tx = l2_queue_write.front();
                l2_queue_write.pop_front();
                writes_processed++;
                process_transaction(tx, current_cycle);
            }

            int remaining_write_tx = write_budget_tx - writes_processed;
            int read_budget = read_budget_tx + remaining_write_tx;
            int reads_processed = 0;
            while (pending_dram_requests.empty() && !l2_queue_read.empty() && reads_processed < read_budget) {
                Transaction tx = l2_queue_read.front();
                l2_queue_read.pop_front();
                reads_processed++;
                process_transaction(tx, current_cycle);
            }
        }

        // Process DRAM completions
        auto dram_completed = dram_backend->step(
            static_cast<std::uint64_t>(std::max<Cycle>(0, current_cycle)));
        for (const auto& completion : dram_completed) {
            auto outstanding = outstanding_dram_requests.find(
                completion.request_id);
            if (outstanding == outstanding_dram_requests.end()) {
                throw std::logic_error(
                    "DRAM completion names an unknown or duplicate request id");
            }
            // FIFO admission means every unadmitted ID is >= its front.
            // A backend must not wake a request it previously rejected.
            if (!pending_dram_requests.empty() &&
                completion.request_id >= pending_dram_requests.front())
                throw std::logic_error("DRAM completion before request admission");
            // Copy before erasing the outstanding entry. The completion path
            // continues to use the request identity/key for MSHR feedback.
            const auto request = outstanding->second;
            const bool request_is_writeback = request.cause ==
                L2DramRequestCause::DIRTY_WRITEBACK;
            if (completion.source_sequence != request.source_sequence ||
                completion.issue_cycle != request.issue_cycle ||
                completion.issue_time_ps != request.issue_time_ps ||
                completion.completion_cycle < request.issue_cycle ||
                completion.completion_cycle >
                    static_cast<std::uint64_t>(std::max<Cycle>(0, current_cycle)) ||
                !(completion.key == request.key) ||
                completion.is_writeback != request_is_writeback) {
                throw std::logic_error(
                    "DRAM completion diverges from its accepted request");
            }
            outstanding_dram_requests.erase(outstanding);
            if (request_is_writeback) {
                if (p32_observation_) p32_observation_->writeback_complete(
                    request.key.matrix_id, request.key.line_addr, request.request_id);
                continue;
            }

            auto mshr_it = mshr.find(request.key);
            if (mshr_it == mshr.end() ||
                mshr_it->second.dram_request_id != completion.request_id) {
                throw std::logic_error(
                    "fill completion does not match the live MSHR request");
            }
            const auto& key = request.key;

            InsertResult insert_result;
            const bool cache_enabled = (!bypass_cache && max_lines > 0);
            if (cache_enabled && (mshr_it->second.any_read || mshr_it->second.any_write)) {
                insert_result = insert_line(
                    key, mshr_it->second.dirty_mask, current_cycle,
                    mshr_it->second.waiting.front(),
                    (shared_cache_state_?shared_cache_state_->fill_at_lru(mshr_it->second.waiting):ef_insertion::pure_completed_read_fill(this,mshr_it->second.waiting)));
            }
            if (p32_observation_ && mshr_it->second.any_write)
                p32_observation_->filled(key.matrix_id, key.line_addr);
            const std::uint64_t waiter_count = mshr_it->second.waiting.size();
            const bool any_read = mshr_it->second.any_read;
            const bool any_write = mshr_it->second.any_write;
            const std::uint64_t fill_sequence = mshr_it->second.fill_sequence;
            const std::uint64_t allocate_decision_sequence =
                mshr_it->second.allocate_decision_sequence;
            const Cycle allocate_cycle = mshr_it->second.allocate_cycle;
            for (const auto& tx : mshr_it->second.waiting) {
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
                observe_source(source_service::Stage::FillWaiter,tx,current_cycle,
                               0,mshr_it->second.fill_sequence);
#endif
                if (model_semantics_.l2_miss_latency ==
                    L2MissLatencySemantics::END_TO_END_FROM_MISS_DECISION) {
                    // completion_queue was drained earlier in this step. Emit
                    // directly so a zero residual delay does not add a hidden
                    // extra cycle after the end-to-end miss interval.
                    l1_cache_.complete_read(tx.read_ticket);
                    completed.push_back(tx.node_id);
                } else {
                    host_append_completion(
                        {tx.node_id, current_cycle + hit_latency_cycles, tx.read_ticket});
                }
            }
            mshr.erase(mshr_it);
            statistics_.fill_completions += 1;
            if (p32_observation_) ++p32_observation_->fill_completions;
            update_peak(statistics_.peak_completion_queue, completion_queue.size());
            emit_fill_event(fill_sequence, allocate_decision_sequence, allocate_cycle,
                            current_cycle, key, waiter_count, any_read, any_write,
                            insert_result);
        }

        // Completions always progress, including when new front-end issue is
        // stalled. Retry only at this real cycle, after due completions free
        // credits; never poll a backend's future to make admission succeed.
        retry_dram_admissions(static_cast<std::uint64_t>(std::max<Cycle>(0, current_cycle)));
        return completed;
    }

    // The original step is the q1/fine path. This explicit interface is only
    // for a serial tiny caller which collected one old physical-return batch.
    struct EpochServiceStatistics {
        std::uint64_t service_calls=0,virtual_slots=0,blocked_epochs=0,blocked_prefix_slots=0;
        void record(unsigned span,bool blocked) {
            const auto prefix=blocked?span-1:0;
            if(service_calls==UINT64_MAX || virtual_slots>UINT64_MAX-span ||
               (blocked&&blocked_epochs==UINT64_MAX) || blocked_prefix_slots>UINT64_MAX-prefix)
                throw std::overflow_error("memory epoch diagnostic overflow");
            ++service_calls;virtual_slots+=span;
            if(blocked)++blocked_epochs;
            blocked_prefix_slots+=prefix;
        }
    };
    void require_memory_epoch_profile(const L2DramCompletionBackend& source) const {
        if (whole_tile_port || owned_dram_backend || dram_backend != &source ||
            model_semantics_.l2_service != L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE)
            throw std::logic_error("memory epoch requires this exact external line L2 backend");
    }
    void require_memory_epoch_span(Cycle current_cycle, unsigned span,
                                   const L2DramCompletionBackend& source) const {
        require_memory_epoch_profile(source);
        if (span<1 || span>8 || current_cycle<static_cast<Cycle>(span))
            throw std::logic_error("memory epoch invalid absolute span");
        const Cycle previous=current_cycle-static_cast<Cycle>(span);
        // FIFO admission order is monotonic. No transaction accepted inside
        // this epoch may claim the resource slots preceding its arrival.
        if ((!l2_queue_read.empty() && l2_queue_read.back().accept_cycle>previous) ||
            (!l2_queue_write.empty() && l2_queue_write.back().accept_cycle>previous))
            throw std::logic_error("memory epoch L2 arrival inside a sealed budget interval");
    }
    std::vector<int> service_epoch(Cycle current_cycle, unsigned span,
                                  const L2DramCompletionBackend& source,
                                  const std::vector<L2DramCompletion>& dram_completed,
                                  EpochServiceStatistics* epoch_statistics=nullptr) {
        require_memory_epoch_span(current_cycle,span,source);
        std::vector<int> completed;
        // A known pending native FIFO blocked L2 at the prior boundary.
        // Credits released by this boundary's old returns cannot retroactively
        // make earlier resource slots eligible. Only the final slot may spend.
        const bool native_blocked_at_entry=!pending_dram_requests.empty();
        if(epoch_statistics)epoch_statistics->record(span,native_blocked_at_entry);
        // All writers are private and update the minimum after a successful
        // push. Before that minimum, the original drain has no effects except
        // copying its unchanged survivors. Service and backend still run.
        if (current_cycle >= host_completion_due_) {
            Cycle next_due=std::numeric_limits<Cycle>::max();
            std::vector<ReadCompletion> remaining_completions;
            for (const auto& completion : completion_queue) {
                if (current_cycle >= completion.complete_cycle) {
                    l1_cache_.complete_read(completion.read_ticket);
                    completed.push_back(completion.node_id);
                } else {
                    remaining_completions.push_back(completion);
                    next_due=std::min(next_due,completion.complete_cycle);
                }
            }
            completion_queue = std::move(remaining_completions);
    
            host_completion_due_=next_due;
        }

        for (const auto& completion : dram_completed) {
            auto outstanding = outstanding_dram_requests.find(
                completion.request_id);
            if (outstanding == outstanding_dram_requests.end()) {
                throw std::logic_error(
                    "DRAM completion names an unknown or duplicate request id");
            }
            // FIFO admission means every unadmitted ID is >= its front.
            // A backend must not wake a request it previously rejected.
            if (!pending_dram_requests.empty() &&
                completion.request_id >= pending_dram_requests.front())
                throw std::logic_error("DRAM completion before request admission");
            // Copy before erasing the outstanding entry. The completion path
            // continues to use the request identity/key for MSHR feedback.
            const auto request = outstanding->second;
            const bool request_is_writeback = request.cause ==
                L2DramRequestCause::DIRTY_WRITEBACK;
            if (completion.source_sequence != request.source_sequence ||
                completion.issue_cycle != request.issue_cycle ||
                completion.issue_time_ps != request.issue_time_ps ||
                completion.completion_cycle < request.issue_cycle ||
                completion.completion_cycle >
                    static_cast<std::uint64_t>(std::max<Cycle>(0, current_cycle)) ||
                !(completion.key == request.key) ||
                completion.is_writeback != request_is_writeback) {
                throw std::logic_error(
                    "DRAM completion diverges from its accepted request");
            }
            outstanding_dram_requests.erase(outstanding);
            if (request_is_writeback) {
                if (p32_observation_) p32_observation_->writeback_complete(
                    request.key.matrix_id, request.key.line_addr, request.request_id);
                continue;
            }

            auto mshr_it = mshr.find(request.key);
            if (mshr_it == mshr.end() ||
                mshr_it->second.dram_request_id != completion.request_id) {
                throw std::logic_error(
                    "fill completion does not match the live MSHR request");
            }
            const auto& key = request.key;

            InsertResult insert_result;
            const bool cache_enabled = (!bypass_cache && max_lines > 0);
            if (cache_enabled && (mshr_it->second.any_read || mshr_it->second.any_write)) {
                insert_result = insert_line(
                    key, mshr_it->second.dirty_mask, current_cycle,
                    mshr_it->second.waiting.front(),
                    (shared_cache_state_?shared_cache_state_->fill_at_lru(mshr_it->second.waiting):ef_insertion::pure_completed_read_fill(this,mshr_it->second.waiting)));
            }
            if (p32_observation_ && mshr_it->second.any_write)
                p32_observation_->filled(key.matrix_id, key.line_addr);
            const std::uint64_t waiter_count = mshr_it->second.waiting.size();
            const bool any_read = mshr_it->second.any_read;
            const bool any_write = mshr_it->second.any_write;
            const std::uint64_t fill_sequence = mshr_it->second.fill_sequence;
            const std::uint64_t allocate_decision_sequence =
                mshr_it->second.allocate_decision_sequence;
            const Cycle allocate_cycle = mshr_it->second.allocate_cycle;
            for (const auto& tx : mshr_it->second.waiting) {
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
                observe_source(source_service::Stage::FillWaiter,tx,current_cycle,
                               0,mshr_it->second.fill_sequence);
#endif
                if (model_semantics_.l2_miss_latency ==
                    L2MissLatencySemantics::END_TO_END_FROM_MISS_DECISION) {
                    // completion_queue was drained earlier in this step. Emit
                    // directly so a zero residual delay does not add a hidden
                    // extra cycle after the end-to-end miss interval.
                    l1_cache_.complete_read(tx.read_ticket);
                    completed.push_back(tx.node_id);
                } else {
                    host_append_completion(
                        {tx.node_id, current_cycle + hit_latency_cycles, tx.read_ticket});
                }
            }
            mshr.erase(mshr_it);
            statistics_.fill_completions += 1;
            if (p32_observation_) ++p32_observation_->fill_completions;
            update_peak(statistics_.peak_completion_queue, completion_queue.size());
            emit_fill_event(fill_sequence, allocate_decision_sequence, allocate_cycle,
                            current_cycle, key, waiter_count, any_read, any_write,
                            insert_result);
        }

        // One old return batch was captured before this method. New dirty
        // writebacks and misses may enqueue, but never cause another pump.
        retry_dram_admissions(static_cast<std::uint64_t>(current_cycle));
        for (unsigned slot=0; slot<span; ++slot) {
            if (model_semantics_.l2_service ==
                L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE) {
                // Total service and the write sub-budget are independent exact
                // token buckets. Writes retain priority; reads borrow every total
                // token not consumed by a write, matching the legacy arbitration
                // policy without flooring bytes/cycle to whole lines per cycle.
                if (l2_queue_read.empty() && l2_queue_write.empty()) {
                    exact_l2_total_budget_.reset();
                    exact_l2_write_budget_.reset();
                } else {
                    exact_l2_total_budget_.accrue_active_cycle();
                    if (l2_queue_write.empty()) {
                        exact_l2_write_budget_.reset();
                    } else {
                        exact_l2_write_budget_.accrue_active_cycle();
                    }

                    while ((!native_blocked_at_entry || slot+1==span) && pending_dram_requests.empty() && !l2_queue_write.empty() &&
                           exact_l2_total_budget_.can_consume_quantum() &&
                           exact_l2_write_budget_.can_consume_quantum()) {
                        Transaction tx = l2_queue_write.front();
                        l2_queue_write.pop_front();
                        exact_l2_total_budget_.consume_quantum();
                        exact_l2_write_budget_.consume_quantum();
                        process_transaction(tx, current_cycle);
                    }
                    while ((!native_blocked_at_entry || slot+1==span) && pending_dram_requests.empty() && !l2_queue_read.empty() &&
                           exact_l2_total_budget_.can_consume_quantum()) {
                        Transaction tx = l2_queue_read.front();
                        l2_queue_read.pop_front();
                        exact_l2_total_budget_.consume_quantum();
                        process_transaction(tx, current_cycle);
                    }
                }
            }
        }
        return completed;
    }


    // Cache-local proof only: the caller must also forbid new upstream issue.
    // This query does not reset credit, advance time, sort completions, or
    // complete any ticket. The serial caller first executes one real idle step
    // and rechecks, so all three original exact budgets have been reset.
    // Legacy idle credit and every external/whole-tile backend are excluded.
    Cycle quiet_delivery_horizon(Cycle current_cycle) const {
        if (current_cycle < 0 ||
            current_cycle >= std::numeric_limits<Cycle>::max() - 1 ||
            whole_tile_port != nullptr || !uses_internal_dram_backend() ||
            model_semantics_.l2_service !=
                L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE ||
            model_semantics_.dram_bandwidth !=
                DramBandwidthSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE ||
            !l2_queue_read.empty() || !l2_queue_write.empty() || !mshr.empty() ||
            !outstanding_dram_requests.empty() || dram_backend->queue_depth() != 0 ||
            completion_queue.empty()) return -1;
        Cycle earliest = std::numeric_limits<Cycle>::max();
        for (const auto& completion : completion_queue) {
            earliest = std::min(earliest, completion.complete_cycle);
        }
        // Even a single due/overdue completion makes a skipped tick unsafe.
        return earliest > current_cycle + 1 ? earliest : -1;
    }

    std::size_t cta_pending_node_references(int first,int end) const {
        std::size_t count=0;
        auto add=[&](int id){if(id>=first && id<end)++count;};
        for(const auto& tx:l2_queue_read)add(tx.node_id);
        for(const auto& tx:l2_queue_write)add(tx.node_id);
        for(const auto& c:completion_queue)add(c.node_id);
        for(const auto& item:mshr)for(const auto& tx:item.second.waiting)add(tx.node_id);
        return count;
    }
private:
    struct InsertResult {
        bool inserted = false;
        L2EvictionKind eviction_kind = L2EvictionKind::NONE;
        CacheLineKey victim{-1, 0};
    };

    int line_size_bytes;
    int hit_latency_cycles;
    int l2_bandwidth_bytes_per_cycle;
    int l2_write_bandwidth_bytes_per_cycle;
    int l2_queue_depth;
    bool bypass_cache;
    size_t max_lines;
    MemoryModelSemantics model_semantics_;
    PerSmL1Cache l1_cache_;
    ExactRationalByteBudget exact_l2_total_budget_;
    ExactRationalByteBudget exact_l2_write_budget_;

    L2GroupedLru<CacheLineKey> lru_list;
    std::unordered_map<CacheLineKey, CacheLineState, CacheLineKeyHash> cache;
    std::unordered_map<CacheLineKey, MSHREntry, CacheLineKeyHash> mshr;
    std::deque<Transaction> l2_queue_read;
    std::deque<Transaction> l2_queue_write;
    std::vector<ReadCompletion> completion_queue;
    Cycle host_completion_due_=std::numeric_limits<Cycle>::max();
    void host_append_completion(const ReadCompletion& completion) {
        completion_queue.push_back(completion);
        host_completion_due_=std::min(host_completion_due_,completion.complete_cycle);
    }
    // Only IDs are retained here; the canonical request lives in the existing
    // outstanding map. With an external backend bound B, pausing service on
    // the first rejection bounds the baseline FIFO by B+1. Sector mode
    // creates at most four individual32B writes per victim and enforces the
    // conservative 4*(B+1) bound. It is not an unbounded ingress.
    std::deque<std::uint64_t> pending_dram_requests;
    std::size_t pending_dram_capacity_ = 0;
    DirtySectorEvalStatistics dirty_sector_stats_;
    std::unique_ptr<shared_cache_policy::State> shared_cache_state_;
    std::uint64_t dram_admission_rejections_ = 0, peak_pending_dram_admissions_ = 0;
    std::unique_ptr<DRAMModel> owned_dram_backend;
    L2DramCompletionBackend* dram_backend;
    const L2DramAddressMapper* dram_address_mapper;
    const L2AddressTransform* address_transform;
    std::uint64_t host_transform_generation_=0;
    L2RuntimeObserver* runtime_observer;
    native_p32_observability::Observation* p32_observation_ = nullptr;
    std::uint64_t next_accept_sequence;
    std::uint64_t next_decision_sequence;
    std::uint64_t next_fill_sequence;
    std::uint64_t next_dram_request_id;
    std::unordered_map<std::uint64_t, L2DramRequest>
        outstanding_dram_requests;
    L2RuntimeStatistics statistics_;

    CacheLineKey transform_key(const DAGNode& node,
                               const CacheLineKey& native_key) const {
        return address_transform == nullptr
                   ? native_key
                   : address_transform->transform(node, native_key);
    }

    bool enqueue_pre_l1(int node_id,
                        const CacheLineKey& native_key,
                        const CacheLineKey& key,
                        bool is_write,
                        Cycle current_cycle,
                        int request_sm_id,
                        int request_subpartition_id,
                        bool allow_l2_forward,
                        bool* forwarded_to_l2,
                        const DAGNode* observation_node = nullptr,
                        L1ReadMissMemo* host_memo = nullptr,
                        int explicit_subop_index = -1) {
        return enqueue_pre_l1_metadata(node_id,native_key,key,is_write,current_cycle,
            request_sm_id,request_subpartition_id,allow_l2_forward,forwarded_to_l2,
            DagLineMetadata{observation_node,explicit_subop_index},host_memo);
    }

    template<class Metadata>
    bool enqueue_pre_l1_metadata(int node_id,
                        const CacheLineKey& native_key,
                        const CacheLineKey& key,
                        bool is_write,
                        Cycle current_cycle,
                        int request_sm_id,
                        int request_subpartition_id,
                        bool allow_l2_forward,
                        bool* forwarded_to_l2,
                        const Metadata& metadata,
                        L1ReadMissMemo* host_memo) {
        if (forwarded_to_l2 != nullptr) *forwarded_to_l2 = false;
        metadata.begin(p32_observation_ != nullptr);
        std::uint8_t requested_sector_mask=15;
        if(l1_cache_.config().sector_validity) {
            // Pure validation precedes negative-memo counters, classify,
            // reservations, dirty-store statistics and accepted transactions.
            if(!(native_key==key))throw std::invalid_argument("sector L1 requires exact native/cache key");
            const auto coverage=metadata.l1_requested(native_key.matrix_id,native_key.line_addr,line_size_bytes);
            if(!coverage.known||!coverage.mask||coverage.mask>15)
                throw std::invalid_argument("sector L1 requires known exact requested byte ranges");
            requested_sector_mask=coverage.mask;
        }
        const PerSmL1Access access{
            request_sm_id, key.matrix_id, key.line_addr, is_write, node_id,
            metadata.bypass_l1(),requested_sector_mask};
        const bool forwarding_blocked=!allow_l2_forward || !can_accept_transaction();
        if(retry_host::enabled && host_memo && forwarding_blocked && l1_cache_.same_negative_read(access,*host_memo)) {
            ++retry_host::counts.negative_memo_hits;
            return false;
        }
        const auto outcome = l1_cache_.classify(access);
        const bool filtered = outcome == PerSmL1Outcome::READ_HIT;
        if (!filtered && forwarding_blocked) {
            if(retry_host::enabled && host_memo)l1_cache_.remember_negative_read(access,*host_memo);
            return false;
        }

        native_p32_observability::RequestMask observation_mask;
        bool observation_first=false, observation_pending=false;
        if (p32_observation_) {
            observation_mask = metadata.request_mask(
                native_key.matrix_id,native_key.line_addr,line_size_bytes);
            observation_first = metadata.first_request(observation_mask,native_key.line_addr);
            if (outcome != PerSmL1Outcome::BYPASS) {
                const auto tag=l1_cache_.inspect(access);
                observation_pending=tag.occupied && !tag.ready;
            }
        }
        std::uint8_t dirty_mask = is_write ? 15 : 0;
        if constexpr (kTilegenDirtySectorMode != 0) {
            if (is_write) {
                dirty_mask = metadata.store_mask(native_key.matrix_id,
                    native_key.line_addr,dirty_sector_stats_);
            }
        }
        const auto decision = l1_cache_.access(access);
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
        // The original acceptance gate passed. Copy only a stable source view;
        // never keep the borrowed metadata/node/subop container.
        const auto source_evidence=metadata.source_semantics();
        const auto source_call_id=source_service::call_id(this);
#endif
        if (p32_observation_) p32_observation_->accepted(observation_mask,
            observation_first,is_write,request_sm_id,outcome,observation_pending);
        if (!decision.forwarded_to_l2) {
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
            source_service::emit(this,{source_service::Stage::Accepted,
                source_evidence,source_call_id,source_service::unknown_id,
                source_service::unknown_id,std::max<Cycle>(0,current_cycle),
                node_id,key.matrix_id,key.line_addr,is_write,false,0});
#endif
            host_append_completion(
                {node_id, std::max<Cycle>(0, current_cycle) +
                              l1_cache_.config().hit_latency_cycles, {}});
            update_peak(statistics_.peak_completion_queue,
                        completion_queue.size());
            return true;
        }

        Transaction tx{node_id, native_key, key, is_write,
                       next_accept_sequence++, std::max<Cycle>(0, current_cycle),
                       std::max(0, request_sm_id),
                       std::max(0, request_subpartition_id), 0, 0, decision.read_ticket, observation_mask, dirty_mask};
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
        tx.source_evidence=source_evidence;
#endif
        if (is_write) {
            l2_queue_write.push_back(tx);
            record_accept(l2_queue_write.back());
        } else {
            l2_queue_read.push_back(tx);
            record_accept(l2_queue_read.back());
        }
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
        observe_source(source_service::Stage::Accepted,tx,tx.accept_cycle,0,
                       source_service::unknown_id);
#endif
        if (forwarded_to_l2 != nullptr) *forwarded_to_l2 = true;
        return true;
    }

    static std::uint32_t checked_u32(std::size_t value) {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("native L2 runtime depth exceeds uint32_t");
        }
        return static_cast<std::uint32_t>(value);
    }

    static void update_peak(std::uint64_t& peak, std::size_t value) {
        peak = std::max(peak, static_cast<std::uint64_t>(value));
    }

    static void fnv1a_append_u64_le(std::uint64_t& hash, std::uint64_t value) {
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xffULL;
            hash *= kFnvPrime;
        }
    }

    static void append_decision_fingerprint(std::uint64_t& hash,
                                            const L2DecisionEvent& event) {
        const std::uint64_t values[] = {
            event.decision_sequence,
            event.accept_sequence,
            event.accept_cycle,
            event.decision_cycle,
            static_cast<std::uint64_t>(event.node_id),
            static_cast<std::uint64_t>(event.matrix_id),
            event.line_addr,
            event.fill_sequence,
            event.is_write ? 1ULL : 0ULL,
            static_cast<std::uint64_t>(event.outcome),
            event.read_queue_depth_after_accept,
            event.write_queue_depth_after_accept,
            event.mshr_entries_after_decision,
            event.resident_lines_after_decision,
            event.dram_queue_depth_after_decision,
        };
        for (std::uint64_t value : values) {
            fnv1a_append_u64_le(hash, value);
        }
    }

    static void append_fill_fingerprint(std::uint64_t& hash,
                                        std::uint64_t completion_sequence,
                                        const L2FillEvent& event) {
        const std::uint64_t values[] = {
            completion_sequence,
            event.fill_sequence,
            event.allocate_decision_sequence,
            event.allocate_cycle,
            event.complete_cycle,
            static_cast<std::uint64_t>(event.matrix_id),
            event.line_addr,
            event.waiter_count,
            event.any_read ? 1ULL : 0ULL,
            event.any_write ? 1ULL : 0ULL,
            event.inserted ? 1ULL : 0ULL,
            static_cast<std::uint64_t>(event.eviction_kind),
            static_cast<std::uint64_t>(event.victim_matrix_id),
            event.victim_line_addr,
            event.resident_lines_after_completion,
            event.mshr_entries_after_completion,
            event.dram_queue_depth_after_completion,
        };
        for (std::uint64_t value : values) {
            fnv1a_append_u64_le(hash, value);
        }
    }

    void record_accept(Transaction& tx) {
        statistics_.accepted_transactions += 1;
        if (tx.is_write) {
            statistics_.accepted_writes += 1;
        } else {
            statistics_.accepted_reads += 1;
        }
        tx.read_queue_depth_after_accept = checked_u32(l2_queue_read.size());
        tx.write_queue_depth_after_accept = checked_u32(l2_queue_write.size());
        update_peak(statistics_.peak_l2_read_queue, l2_queue_read.size());
        update_peak(statistics_.peak_l2_write_queue, l2_queue_write.size());
        update_peak(statistics_.peak_l2_combined_queue,
                    l2_queue_read.size() + l2_queue_write.size());
    }

#if TILEGEN_SOURCE_MEMORY_SEMANTICS
    void observe_source(source_service::Stage stage,const Transaction& tx,
                        Cycle cycle,std::uint8_t outcome,std::uint64_t fill) {
        source_service::emit(this,{stage,tx.source_evidence,source_service::call_id(this),
            tx.accept_sequence,fill,cycle,tx.node_id,tx.key.matrix_id,
            tx.key.line_addr,tx.is_write,true,outcome});
    }
#endif

    void emit_decision_event(const Transaction& tx,
                             std::uint64_t decision_sequence,
                             Cycle decision_cycle,
                             L2DecisionKind outcome,
                             std::uint64_t fill_sequence) {
        const L2DecisionEvent event{
            decision_sequence,
            tx.accept_sequence,
            static_cast<std::uint64_t>(std::max<Cycle>(0, tx.accept_cycle)),
            static_cast<std::uint64_t>(std::max<Cycle>(0, decision_cycle)),
            static_cast<std::int64_t>(tx.node_id),
            static_cast<std::int64_t>(tx.key.matrix_id),
            tx.key.line_addr,
            fill_sequence,
            tx.is_write,
            outcome,
            tx.read_queue_depth_after_accept,
            tx.write_queue_depth_after_accept,
            checked_u32(mshr.size()),
            checked_u32(cache.size()),
            checked_u32(dram_backend->queue_depth()),
            static_cast<std::int64_t>(tx.native_key.matrix_id),
            tx.native_key.line_addr,
        };
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
        observe_source(source_service::Stage::Decision,tx,decision_cycle,
                       static_cast<std::uint8_t>(outcome),fill_sequence);
#endif
        append_decision_fingerprint(statistics_.decision_order_fnv1a64, event);
        if (p32_observation_) p32_observation_->decided(tx.observation_mask,tx.is_write,
            tx.sm_id,static_cast<unsigned>(outcome));
        if (runtime_observer != nullptr) {
            runtime_observer->on_l2_decision(event);
        }
    }

    void process_transaction(const Transaction& tx, Cycle current_cycle) {
        const std::uint64_t decision_sequence = next_decision_sequence++;
        statistics_.processed_transactions += 1;
        if (tx.is_write) {
            statistics_.processed_writes += 1;
        } else {
            statistics_.processed_reads += 1;
        }

        const bool cache_enabled = (!bypass_cache && max_lines > 0);
        if (cache_enabled) {
            auto cache_it = cache.find(tx.key);
            if (cache_it != cache.end()) {
                if(shared_cache_state_&&shared_cache_state_->hit_to_lru(tx))
                    lru_list.touch_lru(tx.key.line_addr,cache_it->second.lru_it);
                else touch_lru(tx.key);
                if (tx.is_write) {
                    if (p32_observation_ && !cache_it->second.dirty)
                        p32_observation_->created(tx.key.matrix_id,tx.key.line_addr,false);
                    if constexpr (kTilegenDirtySectorMode != 0)
                        dirty_sector_stats_.dirty_sector_creations+=sector_popcount(
                            std::uint8_t(tx.dirty_mask & ~cache_it->second.dirty));
                    decode_writer::store(this,tx.key.matrix_id,tx.key.line_addr,cache_it->second.dirty,tx.dirty_mask);
                    cache_it->second.dirty |= tx.dirty_mask;
                    statistics_.write_hits += 1;
                } else {
                    statistics_.read_hits += 1;
                }
                host_append_completion({tx.node_id,
                                            current_cycle + hit_latency_cycles, tx.read_ticket});
                update_peak(statistics_.peak_completion_queue,
                            completion_queue.size());
                emit_decision_event(
                    tx, decision_sequence, current_cycle, L2DecisionKind::HIT,
                    std::numeric_limits<std::uint64_t>::max());
                return;
            }
        }

        auto mshr_it = mshr.find(tx.key);
        if (mshr_it == mshr.end()) {
            const std::uint64_t fill_sequence = next_fill_sequence++;
            MSHREntry entry;
            entry.any_write = tx.is_write;
            entry.dirty_mask = tx.dirty_mask;
            if constexpr (kTilegenDirtySectorMode != 0)
                dirty_sector_stats_.dirty_sector_creations+=sector_popcount(tx.dirty_mask);
            entry.any_read = !tx.is_write;
            entry.fill_sequence = fill_sequence;
            entry.allocate_decision_sequence = decision_sequence;
            entry.allocate_cycle = current_cycle;
            entry.waiting.push_back(tx);
            entry.dram_request_id = enqueue_dram_request(
                tx.key, current_cycle, L2DramRequestCause::FILL_READ, tx);
            mshr.emplace(tx.key, std::move(entry));
            if(tx.is_write)decode_writer::store(this,tx.key.matrix_id,tx.key.line_addr,0,tx.dirty_mask);
            if (p32_observation_ && tx.is_write)
                p32_observation_->created(tx.key.matrix_id,tx.key.line_addr,true);
            if (tx.is_write) {
                statistics_.write_miss_allocates += 1;
            } else {
                statistics_.read_miss_allocates += 1;
            }
            update_peak(statistics_.peak_mshr_entries, mshr.size());
            emit_decision_event(tx, decision_sequence, current_cycle,
                                L2DecisionKind::MISS_ALLOCATE, fill_sequence);
            return;
        }

        mshr_it->second.waiting.push_back(tx);
        if (tx.is_write) {
            if (p32_observation_ && !mshr_it->second.any_write)
                p32_observation_->created(tx.key.matrix_id,tx.key.line_addr,true);
            mshr_it->second.any_write = true;
            if constexpr (kTilegenDirtySectorMode != 0)
                dirty_sector_stats_.dirty_sector_creations+=sector_popcount(
                    std::uint8_t(tx.dirty_mask & ~mshr_it->second.dirty_mask));
            decode_writer::store(this,tx.key.matrix_id,tx.key.line_addr,mshr_it->second.dirty_mask,tx.dirty_mask);
            mshr_it->second.dirty_mask |= tx.dirty_mask;
            statistics_.write_pending_fill_merges += 1;
        } else {
            mshr_it->second.any_read = true;
            statistics_.read_pending_fill_merges += 1;
        }
        emit_decision_event(tx, decision_sequence, current_cycle,
                            L2DecisionKind::PENDING_FILL_MERGE,
                            mshr_it->second.fill_sequence);
    }

    void emit_fill_event(std::uint64_t fill_sequence,
                         std::uint64_t allocate_decision_sequence,
                         Cycle allocate_cycle,
                         Cycle complete_cycle,
                         const CacheLineKey& key,
                         std::uint64_t waiter_count,
                         bool any_read,
                         bool any_write,
                         const InsertResult& insert_result) {
        if (statistics_.fill_completions == 0) {
            throw std::logic_error("L2 fill event emitted before completion accounting");
        }
        const L2FillEvent event{
            fill_sequence,
            allocate_decision_sequence,
            static_cast<std::uint64_t>(std::max<Cycle>(0, allocate_cycle)),
            static_cast<std::uint64_t>(std::max<Cycle>(0, complete_cycle)),
            static_cast<std::int64_t>(key.matrix_id),
            key.line_addr,
            waiter_count,
            any_read,
            any_write,
            insert_result.inserted,
            insert_result.eviction_kind,
            static_cast<std::int64_t>(insert_result.victim.matrix_id),
            insert_result.victim.line_addr,
            checked_u32(cache.size()),
            checked_u32(mshr.size()),
            checked_u32(dram_backend->queue_depth()),
        };
        append_fill_fingerprint(statistics_.fill_order_fnv1a64,
                                statistics_.fill_completions - 1, event);
        if (runtime_observer != nullptr) {
            runtime_observer->on_l2_fill(event);
        }
    }

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
        lru_list.touch(key.line_addr,it->second.lru_it);
    }

    InsertResult insert_line(const CacheLineKey& key, std::uint8_t dirty,
                             Cycle current_cycle,
                             const Transaction& completion_context, bool insert_at_lru = false) {
        InsertResult result;
        auto it = cache.find(key);
        if (it != cache.end()) {
            if(shared_cache_state_)shared_cache_policy::State::add(shared_cache_state_->existing_line_fills);
            it->second.dirty |= dirty;
            touch_lru(key);
            return result;
        }

        if (const auto* selected = lru_list.victim(key.line_addr)) {
            CacheLineKey victim = *selected;
            auto victim_it = cache.find(victim);
            if (victim_it != cache.end()) {
                lru_list.erase(victim.line_addr,victim_it->second.lru_it);
                result.eviction_kind = victim_it->second.dirty
                                           ? L2EvictionKind::DIRTY
                                           : L2EvictionKind::CLEAN;
                result.victim = victim;
                if (victim_it->second.dirty) {
                    const auto mask = victim_it->second.dirty;
                    if constexpr (kTilegenDirtySectorMode != 0) {
                        dirty_sector_stats_.evicted_dirty_sectors+=sector_popcount(mask);
                        ++dirty_sector_stats_.eviction_masks.at(mask);
                        ++dirty_sector_stats_.eviction_popcounts.at(sector_popcount(mask));
                    }
                    if constexpr (kTilegenDirtySectorMode == 2) {
                        // Each dirty sector is one 32B backend request, including
                        // a fully dirty line. Completion keys retain the parent
                        // line identity while service addresses include offsets.
                        unsigned runs = 0;
                        for (unsigned first = 0; first < 4; ++first) {
                            if (!(mask & (1U << first))) continue;
                            enqueue_dram_request(victim, current_cycle,
                                L2DramRequestCause::DIRTY_WRITEBACK,
                                completion_context, first * 32, 32);
                            ++dirty_sector_stats_.writeback_run_lengths.at(1);
                            ++runs;
                        }
                        ++dirty_sector_stats_.eviction_run_counts.at(runs);
                    } else {
                        const auto observation_wb_id = enqueue_dram_request(
                            victim, current_cycle,
                            L2DramRequestCause::DIRTY_WRITEBACK,
                            completion_context);
                        if constexpr (kTilegenDirtySectorMode == 1) {
                            ++dirty_sector_stats_.writeback_run_lengths.at(4);
                            ++dirty_sector_stats_.eviction_run_counts.at(1);
                        }
                        if (p32_observation_) p32_observation_->writeback_issue(
                            victim.matrix_id,victim.line_addr,observation_wb_id);
                    }
                    decode_writer::evict(this,victim.matrix_id,victim.line_addr,mask);
                    statistics_.dirty_evictions += 1;
                } else {
                    statistics_.clean_evictions += 1;
                }
                cache.erase(victim_it);
            }
        }

        auto position = insert_at_lru ? lru_list.insert_lru(key.line_addr,key)
                                      : lru_list.insert_mru(key.line_addr,key);
        if (insert_at_lru) {if(shared_cache_state_)shared_cache_policy::State::add(shared_cache_state_->new_EF_lines_inserted_lru);else ef_insertion::inserted(this);}
        cache.emplace(key, CacheLineState{dirty, position});
        result.inserted = true;
        statistics_.cache_inserts += 1;
        update_peak(statistics_.peak_resident_lines, cache.size());
        return result;
    }

    std::uint64_t enqueue_dram_request(
            const CacheLineKey& key,
            Cycle current_cycle,
            L2DramRequestCause cause,
            const Transaction& context,
            std::uint32_t line_offset = 0, std::uint32_t request_bytes = 0) {
        if (next_dram_request_id ==
            std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("DRAM request identity overflow");
        }
        const auto issue_cycle = static_cast<std::uint64_t>(
            std::max<Cycle>(0, current_cycle));
        L2DramRequest request;
        request.request_id = next_dram_request_id;
        request.source_sequence = next_dram_request_id;
        request.issue_cycle = issue_cycle;
        request.issue_time_ps =
            dram_backend->issue_cycle_to_ps(issue_cycle);
        request.key = key;
        request.address = dram_address_mapper == nullptr
            ? key.line_addr : dram_address_mapper->map(key);
        if constexpr (kTilegenDirtySectorMode == 2) {
            if (request.address % 128 || line_offset % 32 || line_offset >= 128 ||
                request.address > UINT64_MAX - line_offset ||
                (request_bytes && (request_bytes % 32 || request_bytes > 128 - line_offset)) ||
                (cause == L2DramRequestCause::FILL_READ && (line_offset || request_bytes)))
                throw std::logic_error("invalid sector run or non-line-preserving DRAM mapper");
        }
        request.address += line_offset;
        request.bytes = request_bytes ? request_bytes : static_cast<std::uint32_t>(line_size_bytes);
        request.node_id = context.node_id;
        request.sm_id = context.sm_id;
        request.l2_subpartition_id = context.l2_subpartition_id;
        request.cause = cause;
        const auto inserted = outstanding_dram_requests.emplace(
            request.request_id, request);
        if (!inserted.second) {
            throw std::logic_error("duplicate live DRAM request identity");
        }
        ++next_dram_request_id;
        if (!pending_dram_requests.empty() || !dram_backend->try_enqueue(request, issue_cycle)) {
            if constexpr (kTilegenDirtySectorMode == 2) {
                if (pending_dram_requests.size() >= pending_dram_capacity_)
                    throw std::logic_error("sector writeback pending FIFO exceeded finite4(B+1) bound");
            }
            pending_dram_requests.push_back(request.request_id);
            ++dram_admission_rejections_;
            peak_pending_dram_admissions_ = std::max<std::uint64_t>(
                peak_pending_dram_admissions_, pending_dram_requests.size());
        }
        if (p32_observation_ && cause == L2DramRequestCause::FILL_READ)
            ++p32_observation_->fill_requests;
        return request.request_id;
    }

    void retry_dram_admissions(std::uint64_t current_cycle) {
        while (!pending_dram_requests.empty()) {
            const auto id = pending_dram_requests.front();
            const auto& request = outstanding_dram_requests.at(id);
            if (!dram_backend->try_enqueue(request, current_cycle)) {
                ++dram_admission_rejections_;
                return;
            }
            pending_dram_requests.pop_front();
        }
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
