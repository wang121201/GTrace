#pragma once
#include "physical/hbf/hbf_config.hpp"
#include "physical/hbf/hbf_standard.hpp"
#include "physical/resource_calendar.hpp"
#include <map>
#include <set>
#include <unordered_map>

namespace hbfsim::physical::hbf {
// Device-owned state only: channel transport, NAND program ordering, and
// the two decoded pages per bank. No logical addresses, allocator or GC.
class HbfDevice {
public:
    explicit HbfDevice(HbfDeviceConfig config);
    struct Transfer { double start_ns, finish_ns, wait_ns; };
    struct Channel {
        ResourceTimeline command, rx, tx;
        double command_work_ns = 0, rx_work_ns = 0, tx_work_ns = 0;
    };
    enum class Direction { Command, HostToDevice, DeviceToHost };
    [[nodiscard]] Transfer transfer(std::size_t channel, Direction direction,
        std::uint64_t bytes, double arrival_ns, double causal_watermark_ns);
    [[nodiscard]] std::size_t channel_for_page(std::uint64_t ppn) const;
    [[nodiscard]] std::size_t bank_for_page(std::uint64_t ppn) const;
    // Untimed image installation is explicit and forbidden after commands.
    void seed_block(std::uint64_t block, std::uint32_t pages);
    // Returns whether this command must perform the page-zero auto erase.
    [[nodiscard]] bool prepare_program(std::uint64_t ppn);
    void complete_program(std::uint64_t ppn, double ready_ns);
    [[nodiscard]] double read_ready(std::uint64_t ppn) const;
    void erase(std::uint64_t block);
    [[nodiscard]] bool cache_hit(std::uint64_t ppn, double at_ns);
    void cache_fill(std::uint64_t ppn, double ready_ns);
    void advance_cache(double causal_watermark_ns);
    void cache_purge_page(std::uint64_t ppn);
    void cache_purge_block(std::uint64_t block);

    [[nodiscard]] const std::vector<Channel>& channels() const { return channels_; }
    [[nodiscard]] const HbfDeviceConfig& config() const { return config_; }
private:
    void validate_page(std::uint64_t ppn) const;
    void validate_block(std::uint64_t block) const;
    struct Block { std::uint32_t next_page = 0; };
    struct CacheEvent { std::uint64_t page; int action; /* -1 invalidate, 0 touch, 1 fill */ };
    struct BankCache {
        std::set<std::uint64_t> candidates;
        std::size_t future_consumers = 0;
        std::vector<std::uint64_t> resident; // LRU -> MRU at the causal frontier
        std::map<std::pair<double, std::uint64_t>, CacheEvent> events;
        bool pending = false;
    };
    void track_cache_events(std::size_t index, BankCache& bank);
    static bool apply_cache_event(std::vector<std::uint64_t>& resident, CacheEvent event);
    [[nodiscard]] static std::vector<std::uint64_t> project_cache(const BankCache& bank,
        double at_ns, bool* valid = nullptr);
    HbfDeviceConfig config_;
    std::uint64_t total_pages_ = 0;
    std::vector<Channel> channels_;
    std::unordered_map<std::uint64_t, Block> blocks_;
    std::unordered_map<std::uint64_t, double> program_ready_;
    std::multimap<double, std::uint64_t> program_completions_;
    // Temporal records retain future consumers until the causal watermark
    // passes. Logical residency at each timestamp is exactly two per bank.
    std::unordered_map<std::size_t, BankCache> cache_;
    // Banks without temporal events need no work when the frontier advances.
    // Each pending bank appears once, regardless of its event count.
    std::vector<std::size_t> pending_cache_banks_;
    std::uint64_t touch_sequence_ = 0;
    bool started_ = false;
};
}
