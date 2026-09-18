#include "host/hbf_controller.hpp"

#include <cmath>
#include <numeric>

namespace hbfsim::host {
using namespace hbfsim::physical;
using namespace physical;


std::uint64_t HbfController::zone_index(std::uint32_t stack, std::uint32_t channel, std::uint64_t zone) const {
    const auto per_channel = blocks_.size() /
        (static_cast<std::uint64_t>(config_.device.stacks) * config_.device.channels_per_stack) /
        config_.host.zone_size_blocks;
    if (stack >= config_.device.stacks || channel >= config_.device.channels_per_stack || zone >= per_channel)
        throw std::runtime_error("HBF zone is outside its host channel");
    return (static_cast<std::uint64_t>(stack) * config_.device.channels_per_stack + channel) *
        per_channel + zone;
}

std::uint64_t HbfController::physical_zone(std::uint64_t zone) const {
    const auto entry = zone_remapping_.find(zone);
    return entry == zone_remapping_.end() ? zone : entry->second;
}

bool HbfController::zone_available(std::uint64_t physical, bool invalid) const {
    const auto begin = physical * config_.host.zone_size_blocks;
    for (auto b = begin; b < begin + config_.host.zone_size_blocks; ++b) {
        const auto& block = blocks_.at(b);
        if ((block.role != BlockRole::Free && block.role != BlockRole::RawPhysical) ||
            (block.role == BlockRole::Free && logical_capacity_frozen_) ||
            block.erase_pending || block.pending_program_pages || block.pending_mapping_publications ||
            (invalid && block.valid_pages)) return false;
    }
    return true;
}

void HbfController::claim_zone(std::uint64_t physical) {
    const auto begin = physical * config_.host.zone_size_blocks;
    for (auto b = begin; b < begin + config_.host.zone_size_blocks; ++b) {
        auto& block = blocks_.at(b);
        if (block.role != BlockRole::Free) continue;
        auto& free = planes_.at(block_plane_index(b)).free_blocks;
        const auto position = std::find(free.begin(), free.end(), b);
        if (position == free.end()) throw std::runtime_error("HBF zone free block has no pool entry");
        free.erase(position);
        remove_managed_block_wear(b);
        set_block_role(b, BlockRole::RawPhysical);
    }
    zone_managed_ = true;
}

void HbfController::zone_barrier(double at_ns) {
    // Administrative operations are explicit quiescent host barriers. This
    // conservative model fences all issued IO, including reads served by SRAM.
    if (!std::isfinite(at_ns) || at_ns < 0 || at_ns < stats_.finish_ns ||
        (last_issue_arrival_ns_ && at_ns < *last_issue_arrival_ns_))
        throw std::runtime_error("HBF zone management requires a completed IO frontier");
    materialize_committed_state_through(at_ns);
    last_issue_arrival_ns_ = at_ns;
}

PhysicalCompletion HbfController::issue_channel_local(const PhysicalRequest& request) {
    const auto page_size = config_.device.page_size_bytes;
    const auto zone_bytes = static_cast<std::uint64_t>(config_.host.zone_size_blocks) *
        config_.device.pages_per_block * page_size;
    const auto capacity = total_pages_ * page_size;
    if (request.tier != Tier::HBF || (request.op != Op::Read && request.op != Op::Write) ||
        request.bytes == 0 || request.addr >= capacity || request.bytes > capacity - request.addr ||
        !std::isfinite(request.arrival_ns) || request.arrival_ns < 0 ||
        (last_issue_arrival_ns_ && request.arrival_ns < *last_issue_arrival_ns_))
        throw std::runtime_error("invalid HBF channel-local IO");
    // One request stays within a zone; the host splits larger transfers at
    // zone boundaries so a remap can never silently make a range contiguous.
    if (request.addr / zone_bytes != (request.addr + request.bytes - 1) / zone_bytes)
        throw std::runtime_error("split HBF channel IO at zone boundaries");
    if (request.op == Op::Write &&
        (request.addr % page_size || request.bytes % page_size))
        throw std::runtime_error("HBF channel writes require complete NAND pages");
    if (request.op == Op::Read &&
        (request.addr % 64 || request.bytes % 64 || request.bytes > page_size ||
         request.addr / page_size != (request.addr + request.bytes - 1) / page_size))
        throw std::runtime_error("HBF channel reads require aligned 64-byte units within one page");
    const auto physical = physical_zone(request.addr / zone_bytes);
    const auto address = physical * zone_bytes + request.addr % zone_bytes;
    materialize_committed_state_through(request.arrival_ns);
    if (!zone_available(physical, false))
        throw std::runtime_error("HBF channel zone overlaps owned media or pending IO");
    if (request.op == Op::Read) {
        for (auto ppn = address / page_size; ppn <= (address + request.bytes - 1) / page_size; ++ppn) {
            if (!blocks_[ppn / config_.device.pages_per_block].is_valid(ppn % config_.device.pages_per_block))
                throw std::runtime_error("HBF read from invalid/erased zone; rewrite data before reading");
        }
    } else {
        // Validate every target before claiming any part of the zone.
        for (auto ppn = address / page_size; ppn < (address + request.bytes) / page_size; ++ppn) {
            const auto& block = blocks_[ppn / config_.device.pages_per_block];
            const auto first = std::max(address / page_size,
                ppn / config_.device.pages_per_block * config_.device.pages_per_block);
            if (first % config_.device.pages_per_block != block.next_page)
                throw std::runtime_error("HBF zone writes must append in NAND page order; reset before reuse");
        }
        claim_zone(physical);
    }
    auto raw = request;
    raw.address_space = AddressSpace::Physical;
    raw.addr = address;
    auto result = issue(raw);
    result.resource_path = "host/channel-zone" + std::to_string(request.addr / zone_bytes) +
        "->physical-zone" + std::to_string(physical) + "/" + result.resource_path;
    return result;
}

PhysicalCompletion HbfController::invalidate_zone(std::uint32_t stack, std::uint32_t channel, std::uint64_t zone, double at_ns) {
    const auto physical = physical_zone(zone_index(stack, channel, zone));
    zone_barrier(at_ns);
    if (!zone_available(physical, false))
        throw std::runtime_error("cannot invalidate a zone owned by the logical FTL or static image");
    claim_zone(physical);
    const auto begin = physical * config_.host.zone_size_blocks;
    for (auto b = begin; b < begin + config_.host.zone_size_blocks; ++b) {
        read_buffer_purge_block(b);
        for (std::uint32_t page = 0; page < config_.device.pages_per_block; ++page) {
            if (blocks_[b].is_valid(page)) invalidate_ppn(b * config_.device.pages_per_block + page);
        }
    }
    ++stats_.host_zone_invalidations;
    return PhysicalCompletion{.tier = Tier::HBF, .op = Op::Erase,
        .arrival_ns = at_ns, .start_ns = at_ns, .finish_ns = at_ns,
        .note = "host-invalidated-zone; no media erase or copy"};
}

PhysicalCompletion HbfController::remap_zones(std::uint32_t stack, std::uint32_t channel, std::uint64_t first,
    std::uint64_t second, double at_ns) {
    const auto a = zone_index(stack, channel, first);
    const auto b = zone_index(stack, channel, second);
    if (a == b) throw std::runtime_error("HBF remap needs two distinct zones");
    zone_barrier(at_ns);
    const auto pa = physical_zone(a), pb = physical_zone(b);
    if (!zone_available(pa, true) || !zone_available(pb, true))
        throw std::runtime_error("OCP zone remap requires both zones invalid with no outstanding IO");
    claim_zone(pa);
    claim_zone(pb);
    const auto set = [&](auto local, auto physical) {
        if (local == physical) zone_remapping_.erase(local);
        else zone_remapping_[local] = physical;
    };
    set(a, pb); set(b, pa);
    Breakdown command_work;
    const double command_done = schedule_external_request_command(stack * config_.device.channels_per_stack + channel, at_ns,
        command_work, nullptr, "host zone remap registers", "host");
    const double finish = command_done + config_.host.host_zone_remap_ns;
    control_ready_ns_[stack] = std::max(control_ready_ns_[stack], finish);
    last_issue_arrival_ns_ = finish;
    stats_.finish_ns = std::max(stats_.finish_ns, finish);
    ++stats_.host_zone_remaps;
    stats_.host_gc_control_ns += config_.host.host_zone_remap_ns;
    PhysicalCompletion out{.tier = Tier::HBF, .op = Op::Erase,
        .arrival_ns = at_ns, .start_ns = at_ns, .finish_ns = finish,
        .note = "host-zone-remap; zero media copies; PEC stays physical"};
    out.breakdown = command_work;
    out.breakdown.maintenance_ns = config_.host.host_zone_remap_ns;
    stats_.stage_work += out.breakdown;
    return out;
}

PhysicalCompletion HbfController::reset_zone(std::uint32_t stack, std::uint32_t channel, std::uint64_t zone, double at_ns) {
    const auto local = zone_index(stack, channel, zone);
    zone_barrier(at_ns);
    auto physical = physical_zone(local);
    if (!zone_available(physical, true))
        throw std::runtime_error("host must invalidate the complete zone before reset");
    const auto per_channel = blocks_.size() /
        (static_cast<std::uint64_t>(config_.device.stacks) * config_.device.channels_per_stack) /
        config_.host.zone_size_blocks;
    const auto channel_begin = local / per_channel * per_channel;
    const auto pec_sum = [&](std::uint64_t p) {
        std::uint64_t sum = 0;
        for (auto b = p * config_.host.zone_size_blocks; b < (p + 1) * config_.host.zone_size_blocks; ++b)
            sum += blocks_[b].erase_count;
        return sum;
    };
    auto cold_local = local;
    auto cold_pec = pec_sum(physical);
    const auto hot_pec = cold_pec;
    for (auto candidate = channel_begin; candidate < channel_begin + per_channel; ++candidate) {
        const auto p = physical_zone(candidate);
        if (candidate == local || !zone_available(p, true)) continue;
        const auto pec = pec_sum(p);
        if (pec < cold_pec) { cold_local = candidate; cold_pec = pec; }
    }
    PhysicalCompletion out{.tier = Tier::HBF, .op = Op::Erase,
        .arrival_ns = at_ns, .start_ns = at_ns, .finish_ns = at_ns,
        .note = "host-zone-reset"};
    const auto decision_start = std::max(at_ns, control_ready_ns_[stack]);
    out.breakdown.scheduler_queue_wait_ns = decision_start - at_ns;
    out.breakdown.maintenance_ns = config_.host.host_gc_decision_ns;
    at_ns = decision_start + config_.host.host_gc_decision_ns;
    control_ready_ns_[stack] = at_ns;
    stats_.host_gc_control_ns += config_.host.host_gc_decision_ns;
    stats_.stage_work += out.breakdown;
    if (cold_local != local && hot_pec - cold_pec >=
        static_cast<std::uint64_t>(config_.host.host_zone_wear_gap) * config_.host.zone_size_blocks) {
        const auto remap = remap_zones(stack, channel, zone, cold_local - channel_begin, at_ns);
        out.breakdown += remap.breakdown;
        at_ns = remap.finish_ns;
        physical = physical_zone(local);
    }
    claim_zone(physical);
    // Reset releases invalid host ownership; OCP page zero erases on reuse.
    // A zone reset or remap must not manufacture a second P/E cycle.
    for (auto b = physical * config_.host.zone_size_blocks;
         b < (physical + 1) * config_.host.zone_size_blocks; ++b)
        release_invalid_block(b);
    claim_zone(physical);
    out.finish_ns = at_ns;
    return out;
}
} // namespace hbfsim::host
