#include "physical/hbf/hbf_device.hpp"
#include <algorithm>
#include <cmath>

namespace hbfsim::physical::hbf {
HbfDevice::HbfDevice(HbfDeviceConfig config) : config_(config) {
    const auto grade = speed_grade(config.speed_grade);
    if (!config.stacks || !config.channels_per_stack ||
        config.channels_per_stack > grade.maximum_channels ||
        !config.dies_per_channel || config.dies_per_channel > 4 ||
        config.channels_per_stack * config.dies_per_channel > grade.maximum_dies ||
        !config.planes_per_die || config.planes_per_die > 16 ||
        config.page_size_bytes != kPageBytes || !config.pages_per_block || !config.blocks_per_plane)
        throw std::invalid_argument("invalid OCP HBF channel/die/bank/page geometry");
    auto capacity = std::uint64_t{config.page_size_bytes};
    for (const auto factor : {config.stacks, config.channels_per_stack, config.dies_per_channel,
             config.planes_per_die, config.blocks_per_plane, config.pages_per_block}) {
        if (capacity > std::numeric_limits<std::uint64_t>::max() / factor)
            throw std::invalid_argument("OCP HBF geometry exceeds the address space");
        capacity *= factor;
    }
    total_pages_ = capacity / config.page_size_bytes;
    channels_.resize(static_cast<std::size_t>(config.stacks) * config.channels_per_stack);
}
void HbfDevice::validate_page(std::uint64_t ppn) const {
    if (ppn >= total_pages_) throw std::out_of_range("HBF page outside device geometry");
}
void HbfDevice::validate_block(std::uint64_t block) const {
    if (block >= total_pages_ / config_.pages_per_block)
        throw std::out_of_range("HBF block outside device geometry");
}
std::size_t HbfDevice::bank_for_page(std::uint64_t ppn) const {
    validate_page(ppn);
    return ppn / config_.pages_per_block / config_.blocks_per_plane;
}
std::size_t HbfDevice::channel_for_page(std::uint64_t ppn) const {
    return bank_for_page(ppn) / config_.planes_per_die / config_.dies_per_channel;
}
HbfDevice::Transfer HbfDevice::transfer(std::size_t channel, Direction direction,
    std::uint64_t bytes, double arrival, double watermark) {
    if (!std::isfinite(arrival) || arrival < 0 || !bytes ||
        !std::isfinite(watermark) || watermark < 0 || watermark > arrival)
        throw std::invalid_argument("invalid HBF channel transfer");
    auto& c = channels_.at(channel);
    auto& r = direction == Direction::Command ? c.command :
        direction == Direction::HostToDevice ? c.rx : c.tx;
    auto& work = direction == Direction::Command ? c.command_work_ns :
        direction == Direction::HostToDevice ? c.rx_work_ns : c.tx_work_ns;
    const auto duration = bytes / speed_grade(config_.speed_grade).payload_GBps_per_channel;
    r.prune_before(watermark);
    const auto start = r.preview_start(arrival, duration);
    const auto finish = causal_finish(start, duration);
    if (const auto gap = r.first_fitting_gap(arrival, duration)) r.consume_gap(*gap, start, finish);
    else {
        if (start > r.ready_ns) r.insert_frontier_gap(start);
        r.ready_ns = finish;
    }
    work += duration; r.reserved_work_ns += duration; started_ = true;
    return {start, finish, start - arrival};
}
void HbfDevice::seed_block(std::uint64_t block, std::uint32_t pages) {
    validate_block(block);
    if (started_ || pages > config_.pages_per_block) throw std::logic_error("invalid timed HBF image installation");
    if (pages) blocks_[block].next_page = pages;
}
bool HbfDevice::prepare_program(std::uint64_t ppn) {
    validate_page(ppn);
    started_ = true;
    auto& block = blocks_[ppn / config_.pages_per_block];
    const auto page = ppn % config_.pages_per_block;
    if (page && page != block.next_page) throw std::runtime_error("HBF write-order status 0x6");
    if (!page) { cache_purge_block(ppn / config_.pages_per_block);
        for (std::uint32_t p = 0; p < config_.pages_per_block; ++p) program_ready_.erase(ppn + p); }
    block.next_page = static_cast<std::uint32_t>(page + 1);
    return page == 0;
}
void HbfDevice::complete_program(std::uint64_t ppn, double ready) {
    validate_page(ppn);
    (void)blocks_.at(ppn / config_.pages_per_block);
    if (!std::isfinite(ready) || ready < 0) throw std::invalid_argument("invalid HBF program completion time");
    program_ready_[ppn] = ready;
    program_completions_.emplace(ready, ppn);
}
double HbfDevice::read_ready(std::uint64_t ppn) const {
    validate_page(ppn);
    const auto found = blocks_.find(ppn / config_.pages_per_block);
    if (found == blocks_.end() || ppn % config_.pages_per_block >= found->second.next_page)
        throw std::runtime_error("HBF erased-page read status 0x7");
    const auto ready = program_ready_.find(ppn);
    return ready == program_ready_.end() ? 0 : ready->second;
}
void HbfDevice::erase(std::uint64_t block) {
    validate_block(block);
    blocks_.erase(block);
    cache_purge_block(block);
    for (auto page = block * config_.pages_per_block; page < (block + 1) * config_.pages_per_block; ++page)
        program_ready_.erase(page);
}
bool HbfDevice::apply_cache_event(std::vector<std::uint64_t>& resident, CacheEvent event) {
    const auto at = std::find(resident.begin(), resident.end(), event.page);
    if (event.action < 0) { if (at != resident.end()) resident.erase(at); return true; }
    if (event.action == 0 && at == resident.end()) return false;
    if (at != resident.end()) resident.erase(at);
    resident.push_back(event.page);
    if (resident.size() > kCachedPagesPerBank) resident.erase(resident.begin());
    return true;
}
std::vector<std::uint64_t> HbfDevice::project_cache(const BankCache& bank, double at, bool* valid) {
    auto resident = bank.resident;
    if (valid) *valid = true;
    for (const auto& [key, event] : bank.events) {
        if (key.first > at) break;
        if (!apply_cache_event(resident, event) && valid) *valid = false;
    }
    return resident;
}
bool HbfDevice::cache_hit(std::uint64_t ppn, double at) {
    auto found = cache_.find(bank_for_page(ppn));
    if (found == cache_.end()) return false;
    auto& bank = found->second;
    if (!bank.candidates.contains(ppn)) return false;
    const auto resident = project_cache(bank, at);
    if (std::find(resident.begin(), resident.end(), ppn) == resident.end()) return false;
    const auto key = std::pair{at, touch_sequence_++};
    bank.events.emplace(key, CacheEvent{ppn, false});
    bool valid = true;
    (void)project_cache(bank, std::numeric_limits<double>::infinity(), &valid);
    if (!valid) bank.events.erase(key);
    else {
        ++bank.future_consumers;
        track_cache_events(found->first, bank);
    }
    return valid;
}
void HbfDevice::track_cache_events(std::size_t index, BankCache& bank) {
    if (bank.pending) return;
    pending_cache_banks_.push_back(index);
    bank.pending = true;
}
void HbfDevice::cache_fill(std::uint64_t ppn, double ready) {
    const auto index = bank_for_page(ppn);
    auto& bank = cache_[index];
    const auto key = std::pair{ready, touch_sequence_++};
    bank.candidates.insert(ppn);
    bank.events.emplace(key, CacheEvent{ppn, true});
    if (bank.future_consumers && bank.events.rbegin()->first != key) {
        bool valid = true;
        (void)project_cache(bank, std::numeric_limits<double>::infinity(), &valid);
        // A future cache-hit response is already committed. Bypass this fill
        // instead of evicting its data retroactively; residency stays <= 2.
        if (!valid) { bank.events.erase(key); return; }
    }
    track_cache_events(index, bank);
}
void HbfDevice::advance_cache(double watermark) {
    while (!program_completions_.empty() && program_completions_.begin()->first <= watermark) {
        const auto [ready, ppn] = *program_completions_.begin();
        const auto entry = program_ready_.find(ppn);
        if (entry != program_ready_.end() && entry->second == ready) program_ready_.erase(entry);
        program_completions_.erase(program_completions_.begin());
    }
    for (std::size_t i = 0; i < pending_cache_banks_.size();) {
        auto& bank = cache_.at(pending_cache_banks_[i]);
        auto event = bank.events.begin();
        while (event != bank.events.end() && event->first.first <= watermark) {
            if (!apply_cache_event(bank.resident, event->second))
                throw std::logic_error("HBF cache lost a reserved consumer");
            if (event->second.action == 0) --bank.future_consumers;
            event = bank.events.erase(event);
        }
        if (bank.events.empty()) {
            bank.candidates = {bank.resident.begin(), bank.resident.end()};
            bank.pending = false;
            pending_cache_banks_[i] = pending_cache_banks_.back();
            pending_cache_banks_.pop_back();
        } else {
            ++i;
        }
    }
}
void HbfDevice::cache_purge_page(std::uint64_t ppn) {
    const auto b = cache_.find(bank_for_page(ppn));
    if (b == cache_.end()) return;
    auto& bank = b->second;
    if (bank.events.empty()) {
        std::erase(bank.resident, ppn);
        bank.candidates.erase(ppn);
        return;
    }
    // Keep prior fills: deleting history can resurrect an evicted page.
    // The host fences block invalidation after its outstanding consumers.
    bank.events.emplace(std::pair{bank.events.rbegin()->first.first, touch_sequence_++},
        CacheEvent{ppn, -1});
}
void HbfDevice::cache_purge_block(std::uint64_t block) {
    const auto b = cache_.find(bank_for_page(block * config_.pages_per_block));
    if (b == cache_.end()) return;
    std::set<std::uint64_t> pages;
    for (const auto page : b->second.resident)
        if (page / config_.pages_per_block == block) pages.insert(page);
    for (const auto& [key, event] : b->second.events) {
        (void)key;
        if (event.page / config_.pages_per_block == block) pages.insert(event.page);
    }
    for (const auto page : pages) cache_purge_page(page);
}
}
