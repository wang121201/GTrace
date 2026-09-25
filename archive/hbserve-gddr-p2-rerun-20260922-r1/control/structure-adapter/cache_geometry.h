#pragma once
// Shared functional L2 indexing and ordinary group-local LRU only. This does
// not model sector validity, write allocation, dirty admission, or GPU timing.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <stdexcept>
#include <vector>
#include "ada_address_mapping.h"

namespace GTSim {

enum class L2GeometryMode : std::uint8_t {
    FULLY_ASSOCIATIVE,
    PAPER_ADA_SET_ASSOCIATIVE,
    ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE
};

struct L2GeometryConfig {
    L2GeometryMode mode = L2GeometryMode::FULLY_ASSOCIATIVE;
    static L2GeometryConfig accelsim_rtx4000_ada() {
        return {L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE};
    }
    static L2GeometryConfig paper_ada_l2_v1() {
        return {L2GeometryMode::PAPER_ADA_SET_ASSOCIATIVE};
    }
};

class L2Geometry {
    using U = std::uint64_t;
    L2GeometryConfig config_;
    U line_bytes_, total_lines_, groups_, group_capacity_;
public:
    // Zero-capacity fully associative geometry supports legacy bypass objects.
    // It never supplies a victim and the LRU helper cannot insert into it.
    L2Geometry(const L2GeometryConfig& config, U cache_bytes, U line_bytes)
        : config_(config), line_bytes_(line_bytes), total_lines_(0), groups_(1), group_capacity_(0) {
        if (!line_bytes || (line_bytes & (line_bytes - 1)) || cache_bytes % line_bytes)
            throw std::invalid_argument("L2 geometry needs power-of-two lines and integral line capacity");
        total_lines_ = cache_bytes / line_bytes;
        switch (config.mode) {
        case L2GeometryMode::FULLY_ASSOCIATIVE:
            group_capacity_ = total_lines_;
            break;
        case L2GeometryMode::PAPER_ADA_SET_ASSOCIATIVE:
            if (line_bytes != 128 || cache_bytes != U{20} * 1024 * 16 * 128)
                throw std::invalid_argument("PAPER Ada L2 geometry requires20x1024x16x128 bytes");
            groups_ = 20 * 1024;
            group_capacity_ = 16;
            break;
        case L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE:
            if (line_bytes != AdaAddressMapping::line_bytes || cache_bytes != AdaAddressMapping::cache_bytes)
                throw std::invalid_argument("AccelSim Ada software geometry requires20x1024x16x128 bytes");
            groups_ = AdaAddressMapping::subpartitions * AdaAddressMapping::sets_per_subpartition;
            group_capacity_ = AdaAddressMapping::ways;
            break;
        default:
            throw std::invalid_argument("unknown L2 geometry mode");
        }
        if (groups_ > std::numeric_limits<std::size_t>::max() ||
            group_capacity_ > std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("L2 geometry exceeds host container size");
    }
    const L2GeometryConfig& config() const { return config_; }
    U line_bytes() const { return line_bytes_; }
    U total_lines() const { return total_lines_; }
    U group_count() const { return groups_; }
    U capacity_per_group() const { return group_capacity_; }

    // PAPER_ADA_L2_V1_WORKFLOW_1 frozen software surrogate, not a claim about
    // NVIDIA's unpublished hardware hash. Source: frozen memgen.cc SHA256
    // 19c526f604c80d8a5d95aa34d4e74b87a2bce8018fb693650ad44b05f3637402:
    // low_mask_index, l2_cache_index_addr, SectorLruCache::set_index (X mode).
    // In particular,20 logical slices require quotient/remainder; removing
    // five address bits would implement a different geometry. Index original
    // byte VA, before any backend service-address mapping. Matrix namespaces
    // remain part of the engine's tag key and are not mixed into this index.
    U partition(U address) const {
        if (config_.mode == L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE)
            return AdaAddressMapping::sub_partition(address);
        return config_.mode == L2GeometryMode::FULLY_ASSOCIATIVE ? 0 : (address >> 8) % 20;
    }
    U set(U address) const {
        if (config_.mode == L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE)
            return AdaAddressMapping::set(address);
        if (config_.mode == L2GeometryMode::FULLY_ASSOCIATIVE) return 0;
        const U index_address = (((address >> 8) / 20) << 8) | (address & 255);
        const U block = index_address >> 7;
        return ((block % 1024) ^ ((block >> 10) & 1023)) % 1024;
    }
    U group(U address) const { return partition(address) * 1024 + set(address); }
};

// Engines retain their existing tag/dirty map and store an iterator in each
// entry. No second tag map is introduced. Iterators survive insert and touch;
// erase invalidates only the erased iterator. A supplied iterator must be live
// and belong to this helper's group(address), as for std::list::splice/erase.
// Insertion does not select or discard a victim: the engine must emit/account
// for the optional victim, erase it, and then insert the newly completed fill.
template<class Key>
class L2GroupedLru {
public:
    using iterator = typename std::list<Key>::iterator;
private:
    using U = std::uint64_t;
    L2Geometry geometry_;
    std::vector<std::list<Key>> groups_;
    U size_ = 0;
public:
    explicit L2GroupedLru(const L2Geometry& geometry)
        : geometry_(geometry), groups_(static_cast<std::size_t>(geometry.group_count())) {}
    L2GroupedLru(const L2GeometryConfig& config, U cache_bytes, U line_bytes)
        : L2GroupedLru(L2Geometry(config, cache_bytes, line_bytes)) {}
    L2GroupedLru(const L2GroupedLru&) = delete;
    L2GroupedLru& operator=(const L2GroupedLru&) = delete;
    L2GroupedLru(L2GroupedLru&&) = default;
    L2GroupedLru& operator=(L2GroupedLru&&) = default;
    const L2Geometry& geometry() const { return geometry_; }
    U group(U address) const { return geometry_.group(address); }
    U capacity_per_group() const { return geometry_.capacity_per_group(); }
    U size() const { return size_; }
    bool empty() const { return size_ == 0; }
    U group_size(U address) const { return groups_[group(address)].size(); }
    const Key* victim(U address) const {
        const auto& list = groups_[group(address)];
        return list.empty() || list.size() < capacity_per_group() ? nullptr : &list.back();
    }
    iterator insert_mru(U address, const Key& key) {
        auto& list = groups_[group(address)];
        if (list.size() >= capacity_per_group())
            throw std::logic_error("L2 group full: account for and erase victim before insert");
        list.push_front(key);
        ++size_;
        return list.begin();
    }
    iterator insert_lru(U address, const Key& key) {
        auto& list = groups_[group(address)];
        if (list.size() >= capacity_per_group())
            throw std::logic_error("L2 group full: account for and erase victim before insert");
        list.push_back(key);
        ++size_;
        auto position=list.end();
        return --position;
    }
    void touch(U address, iterator position) {
        auto& list = groups_[group(address)];
        list.splice(list.begin(), list, position);
    }
    // Same LRU-end splice as the selected shared functional cache.
    void touch_lru(U address, iterator position) {
        auto& list = groups_[group(address)];
        list.splice(list.end(), list, position);
    }
    void erase(U address, iterator position) {
        auto& list = groups_[group(address)];
        if (list.empty()) throw std::logic_error("cannot erase from empty L2 group");
        list.erase(position);
        --size_;
    }
};

} // namespace GTSim
