#ifndef PER_SM_L1_H
#define PER_SM_L1_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "retry_host_memo.h"

namespace GTSim {

// This cache is a footprint/scheduling sensitivity model.  It is deliberately
// not named an exact RTX 4000 Ada L1 implementation: capacity, associativity,
// replacement, write policy, persistence, and hit latency remain modeled.
enum class PerSmL1Mode : std::uint8_t {
    BYPASS = 0,
    MODELED_SET_ASSOCIATIVE = 1,
};

enum class PerSmL1Persistence : std::uint8_t {
    KERNEL_FLUSH = 0,
    CROSS_KERNEL_PERSISTENT = 1,
};

enum class PerSmL1ReplacementPolicy : std::uint8_t {
    LRU = 0,
    FIFO = 1,
};

enum class PerSmL1Outcome : std::uint8_t {
    BYPASS = 0,
    READ_HIT = 1,
    READ_MISS = 2,
    WRITE_HIT = 3,
    WRITE_MISS = 4,
};

inline const char* per_sm_l1_mode_name(PerSmL1Mode mode) {
    switch (mode) {
        case PerSmL1Mode::BYPASS: return "BYPASS";
        case PerSmL1Mode::MODELED_SET_ASSOCIATIVE:
            return "MODELED_SET_ASSOCIATIVE";
    }
    return "UNKNOWN";
}

inline const char* per_sm_l1_persistence_name(PerSmL1Persistence value) {
    switch (value) {
        case PerSmL1Persistence::KERNEL_FLUSH: return "KERNEL_FLUSH";
        case PerSmL1Persistence::CROSS_KERNEL_PERSISTENT:
            return "CROSS_KERNEL_PERSISTENT";
    }
    return "UNKNOWN";
}

inline const char* per_sm_l1_replacement_name(PerSmL1ReplacementPolicy value) {
    switch (value) {
        case PerSmL1ReplacementPolicy::LRU: return "LRU";
        case PerSmL1ReplacementPolicy::FIFO: return "FIFO";
    }
    return "UNKNOWN";
}

struct PerSmL1Config {
    PerSmL1Mode mode = PerSmL1Mode::BYPASS;
    PerSmL1Persistence persistence =
        PerSmL1Persistence::CROSS_KERNEL_PERSISTENT;
    std::uint32_t num_sms = 48;
    std::uint64_t capacity_bytes_per_sm = 64ULL * 1024ULL;
    std::uint32_t ways = 8;
    std::uint32_t line_bytes = 128;
    // PAPER_ADA stores bypass L1 without updating its replacement state.
    // False retains the historical write-through/no-allocate model.
    bool store_bypass = false;
    // Integer model value derived from the 7.062-cycle dependent-load L1
    // candidate in MEMORY_FOOTPRINT_RESULTS.md.  It is uncalibrated.
    int hit_latency_cycles = 7;
    // Opt-in functional sector model. Legacy callers retain whole-line
    // readiness and write-through/no-allocate behavior byte-for-byte.
    bool sector32 = false;
    bool write_allocate = false;
    // AccelSim's modified-line victim protection, per SM, as a percent of
    // total capacity (not resident lines). WT modified data needs no L1 WB.
    std::uint32_t dirty_protection_percent = 0;
    // FIFO is an explicit experiment option; default LRU remains unchanged.
    PerSmL1ReplacementPolicy replacement = PerSmL1ReplacementPolicy::LRU;
};

struct PerSmL1Access {
    int sm_id = -1;
    int allocation_id = -1;
    std::uint64_t canonical_line = 0;
    bool is_write = false;
    int node_id = -1;
    bool bypass_l1 = false; // per-request cache operator; no allocation or hit filtering
    // A zero mask retains the old whole-line request convention. In sector
    // mode explicit bits 0..3 select 32 B sectors within this 128 B line.
    std::uint8_t sector_mask = 0;
    // Store byte coverage in each selected sector. Zero means unknown/no
    // known bytes, NEVER a complete sector. Ignored by the legacy model.
    std::array<std::uint32_t,4> known_byte_masks{};
};

// Each forwarded modeled read owns a separate delivery obligation. Multiple
// requests may name the same reservation generation; they are not L1-merged.
struct ReadFillTicket {
    bool valid = false;
    int sm_id = -1;
    int allocation_id = -1;
    std::uint32_t set_index = 0;
    std::uint32_t way = 0;
    std::uint64_t canonical_line = 0;
    std::uint64_t generation = 0;
    std::uint64_t ticket_id = 0;
    std::uint8_t sector_mask = 0; // zero only in legacy whole-line mode
};

struct ReadinessLine {
    bool occupied = false;
    bool ready = false;
    std::uint32_t set_index = 0;
    std::uint32_t way = 0;
    int allocation_id = -1;
    std::uint64_t canonical_line = 0;
    std::uint64_t generation = 0;
    std::uint8_t readable_sector_mask = 0;
    std::array<std::uint32_t,4> known_byte_masks{};
    bool modified = false;
    std::uint8_t resident_sector_mask = 0;
};

struct ReadinessStatistics {
    std::uint64_t issued_tickets = 0;
    std::uint64_t completed_tickets = 0;
    std::uint64_t stale_tickets = 0;
    std::uint64_t ready_transitions = 0;
    std::uint64_t pending_tag_reads = 0;
    // The inherited WRITE_HIT count is tag occupancy, NOT data readiness.
    std::uint64_t write_hits_on_pending = 0;
    std::uint64_t live_tickets = 0;
    std::uint64_t peak_live_tickets = 0;
    std::uint64_t occupied_lines = 0;
    std::uint64_t ready_lines = 0;
    std::uint64_t pending_lines = 0;
    std::uint64_t sector_readiness_promotions = 0;
    std::uint64_t store_readiness_promotions = 0;
};

struct PerSmL1Decision {
    PerSmL1Outcome outcome = PerSmL1Outcome::BYPASS;
    std::uint32_t set_index = 0;
    bool forwarded_to_l2 = true;
    bool allocated = false;
    bool evicted = false;
    ReadFillTicket read_ticket;
    std::uint8_t forwarded_sector_mask = 0;
    // Functional fall-through: downstream still serves this request, but no
    // L1 reservation was available. This does NOT simulate reservation stalls.
    bool reservation_failed = false;
};

struct PerSmL1SmStatistics {
    std::uint32_t sm_id = 0;
    std::uint64_t pre_l1_reads = 0;
    std::uint64_t pre_l1_writes = 0;
    std::uint64_t bypassed = 0;
    std::uint64_t read_hits = 0;
    std::uint64_t read_misses = 0;
    std::uint64_t write_hits = 0;
    std::uint64_t write_misses = 0;
    std::uint64_t filtered_reads = 0;
    std::uint64_t evictions = 0;
    std::uint64_t peak_resident_lines = 0;
    std::uint64_t final_resident_lines = 0;
};

struct PerSmL1Statistics {
    std::uint64_t pre_l1_transactions = 0;
    std::uint64_t pre_l1_reads = 0;
    std::uint64_t pre_l1_writes = 0;
    std::uint64_t bypassed_transactions = 0;
    std::uint64_t read_hits = 0;
    std::uint64_t read_misses = 0;
    std::uint64_t write_hits = 0;
    std::uint64_t write_misses = 0;
    std::uint64_t filtered_reads = 0;
    std::uint64_t l2_input_transactions = 0;
    std::uint64_t evictions = 0;
    std::uint64_t kernel_flushes = 0;
    std::uint64_t flushed_lines = 0;
    std::uint64_t peak_resident_lines = 0;
    std::uint64_t final_resident_lines = 0;
    std::uint64_t decision_order_fnv1a64 = 14695981039346656037ULL;
    std::vector<PerSmL1SmStatistics> per_sm;
    std::uint64_t unmodeled_reservation_stalls = 0;
    std::uint64_t capacity_reconfigurations = 0;
    std::uint64_t capacity_reconfiguration_flushed_lines = 0;
};

struct L1ReadMissMemo {
    const void* owner=nullptr;
    std::uint64_t line=0,epoch=0;
    int sm=-1,allocation=-1;
    bool valid=false;
    std::uint8_t sector_mask=0;
    std::uint64_t layout_epoch=0;
};

class PerSmL1Cache {
public:
    explicit PerSmL1Cache(const PerSmL1Config& config = PerSmL1Config())
        : config_(config) {
        if (config_.num_sms == 0 || config_.line_bytes == 0 ||
            config_.capacity_bytes_per_sm == 0 || config_.ways == 0 ||
            config_.hit_latency_cycles < 0 ||
            config_.capacity_bytes_per_sm % config_.line_bytes != 0 ||
            config_.dirty_protection_percent > 100 ||
            (config_.replacement != PerSmL1ReplacementPolicy::LRU &&
             config_.replacement != PerSmL1ReplacementPolicy::FIFO) ||
            (config_.sector32 && config_.line_bytes != 128) ||
            (!config_.sector32 && (config_.write_allocate ||
                                  config_.dirty_protection_percent != 0))) {
            throw std::invalid_argument("invalid per-SM L1 configuration");
        }
        capacity_lines_per_sm_ =
            config_.capacity_bytes_per_sm / config_.line_bytes;
        if (capacity_lines_per_sm_ == 0 ||
            capacity_lines_per_sm_ % config_.ways != 0) {
            throw std::invalid_argument(
                "per-SM L1 capacity lines must be divisible by ways");
        }
        num_sets_ = capacity_lines_per_sm_ / config_.ways;
        if (capacity_lines_per_sm_ >
            std::numeric_limits<std::size_t>::max() / config_.num_sms) {
            throw std::overflow_error("per-SM L1 allocation size overflow");
        }
        lines_.resize(static_cast<std::size_t>(capacity_lines_per_sm_) *
                      config_.num_sms);
        resident_per_sm_.assign(config_.num_sms, 0);
        modified_per_sm_.assign(config_.num_sms, 0);
        ready_promotion_epochs_.assign(static_cast<std::size_t>(num_sets_)*config_.num_sms,0);
        ready_promotion_epochs_sm_.assign(config_.num_sms,0);
        statistics_.per_sm.resize(config_.num_sms);
        for (std::uint32_t sm = 0; sm < config_.num_sms; ++sm) {
            statistics_.per_sm[sm].sm_id = sm;
        }
    }

    const PerSmL1Config& config() const { return config_; }
    std::uint64_t host_ready_epoch(int sm) const { return ready_promotion_epochs_sm_.at(sm); }

    // Quiescent kernel-boundary reconfiguration. Callers select the adaptive
    // partition; this method does not guess occupancy/shared-memory usage.
    // It invalidates tags without a writeback and preserves cumulative stats.
    void configure_capacity_bytes_per_sm(std::uint64_t bytes, std::uint32_t ways) {
        if (bytes == config_.capacity_bytes_per_sm && ways == config_.ways) return;
        if (!live_tickets_.empty())
            throw std::logic_error("L1 capacity reconfiguration requires no live read tickets");
        if (!bytes || !ways || bytes % config_.line_bytes ||
            (bytes / config_.line_bytes) % ways)
            throw std::invalid_argument("invalid L1 adaptive capacity/ways");
        const auto count = bytes / config_.line_bytes;
        if (count > std::numeric_limits<std::size_t>::max() / config_.num_sms ||
            layout_epoch_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("L1 capacity reconfiguration overflow");
        for (const auto e : ready_promotion_epochs_sm_)
            if (e == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("L1 host SM ready-promotion epoch overflow");
        // Allocate before mutating the current cache, so allocation failure is
        // harmless. Every outstanding read was explicitly ruled out above.
        std::vector<Line> replacement(static_cast<std::size_t>(count) * config_.num_sms);
        std::vector<std::uint64_t> epochs(static_cast<std::size_t>(count / ways) * config_.num_sms, 0);
        statistics_.capacity_reconfiguration_flushed_lines += total_resident_;
        ++statistics_.capacity_reconfigurations;
        ++layout_epoch_;
        for (auto& e : ready_promotion_epochs_sm_) ++e;
        lines_.swap(replacement);
        ready_promotion_epochs_.swap(epochs);
        std::fill(resident_per_sm_.begin(), resident_per_sm_.end(), 0);
        std::fill(modified_per_sm_.begin(), modified_per_sm_.end(), 0);
        total_resident_ = ready_lines_ = 0;
        capacity_lines_per_sm_ = count;
        num_sets_ = count / ways;
        config_.capacity_bytes_per_sm = bytes;
        config_.ways = ways;
    }
    void configure_capacity_bytes_per_sm(std::uint64_t bytes) {
        configure_capacity_bytes_per_sm(bytes, config_.ways);
    }

    PerSmL1Outcome classify(const PerSmL1Access& access) const {
        ++retry_host::counts.l1_classify_calls;
        validate_sector_access(access);
        // BYPASS is a strict compatibility path: legacy direct-key tests may
        // use abstract, non-byte-aligned keys which never enter modeled L1.
        if (config_.mode == PerSmL1Mode::BYPASS || access.bypass_l1 ||
            (config_.store_bypass && access.is_write)) {
            return PerSmL1Outcome::BYPASS;
        }
        if (access.canonical_line % config_.line_bytes != 0) {
            throw std::invalid_argument("per-SM L1 line is not aligned");
        }
        validate_modeled_sm(access.sm_id);
        const auto location = locate(access);
        if (access.is_write) {
            return location.hit_way < config_.ways && (!config_.sector32 ||
                       (lines_[location.base + location.hit_way].resident_sector_mask & request_mask(access)) == request_mask(access))
                       ? PerSmL1Outcome::WRITE_HIT
                       : PerSmL1Outcome::WRITE_MISS;
        }
        return location.hit_way < config_.ways &&
                       read_ready(lines_[location.base + location.hit_way], access)
                   ? PerSmL1Outcome::READ_HIT
                   : PerSmL1Outcome::READ_MISS;
    }

    // A cached negative may only bypass a repeated, blocked, side-effect-free
    // rejection. No positive hit/victim/LRU state survives across calls.
    bool same_negative_read(const PerSmL1Access& a,const L1ReadMissMemo& memo) const {
        if(a.bypass_l1 || a.is_write || config_.mode!=PerSmL1Mode::MODELED_SET_ASSOCIATIVE || !memo.valid)return false;
        if(memo.owner!=this || memo.sm!=a.sm_id || memo.allocation!=a.allocation_id || memo.line!=a.canonical_line || memo.sector_mask!=request_mask(a) || memo.layout_epoch!=layout_epoch_){++retry_host::counts.memo_identity_rechecks;return false;}
        const auto set=(a.canonical_line/config_.line_bytes)%num_sets_;
        const auto epoch=ready_promotion_epochs_[std::size_t(a.sm_id)*num_sets_+set];
        if(memo.epoch!=epoch){++retry_host::counts.memo_epoch_rechecks;return false;}
        return true;
    }
    void remember_negative_read(const PerSmL1Access& a,L1ReadMissMemo& memo) const {
        if(a.bypass_l1 || a.is_write || config_.mode!=PerSmL1Mode::MODELED_SET_ASSOCIATIVE){memo.valid=false;return;}
        // Called only after original classify successfully validated this key.
        const auto set=(a.canonical_line/config_.line_bytes)%num_sets_;
        memo={this,a.canonical_line,ready_promotion_epochs_[std::size_t(a.sm_id)*num_sets_+set],a.sm_id,a.allocation_id,true};
        memo.sector_mask=request_mask(a);memo.layout_epoch=layout_epoch_;
        ++retry_host::counts.memo_negative_records;
    }

    PerSmL1Decision access(const PerSmL1Access& access) {
        const PerSmL1Outcome classified = classify(access);
        PerSmL1Decision decision;
        decision.outcome = classified;
        decision.forwarded_to_l2 = classified != PerSmL1Outcome::READ_HIT;
        decision.forwarded_sector_mask = decision.forwarded_to_l2 ? request_mask(access) : 0;

        ++statistics_.pre_l1_transactions;
        if (access.is_write) {
            ++statistics_.pre_l1_writes;
        } else {
            ++statistics_.pre_l1_reads;
        }
        PerSmL1SmStatistics* sm_stats = nullptr;
        if (access.sm_id >= 0 &&
            static_cast<std::uint32_t>(access.sm_id) < config_.num_sms) {
            sm_stats = &statistics_.per_sm[static_cast<std::size_t>(access.sm_id)];
            if (access.is_write) ++sm_stats->pre_l1_writes;
            else ++sm_stats->pre_l1_reads;
        }

        if (classified == PerSmL1Outcome::BYPASS) {
            ++statistics_.bypassed_transactions;
            ++statistics_.l2_input_transactions;
            if (sm_stats != nullptr) ++sm_stats->bypassed;
            append_decision(access, decision);
            return decision;
        }

        const auto before = locate(access);
        decision.set_index = before.set_index;
        if (use_clock_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("per-SM L1 use clock overflow");
        }
        if (!access.is_write && classified != PerSmL1Outcome::READ_HIT) {
            if (next_ticket_id_ == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("per-SM L1 read ticket overflow");
            if (before.hit_way == config_.ways &&
                next_generation_ == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("per-SM L1 reservation generation overflow");
        }
        if (access.is_write && config_.write_allocate && before.hit_way == config_.ways &&
            next_generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("per-SM L1 reservation generation overflow");
        ++use_clock_;

        if (before.hit_way < config_.ways) {
            auto& line = lines_[before.base + before.hit_way];
            // Under FIFO this timestamp is the whole-line insertion age.
            // Neither hits nor allocation of another sector in this tag may
            // reorder it. Delivery completion also leaves it untouched.
            if (config_.replacement == PerSmL1ReplacementPolicy::LRU)
                line.last_use = use_clock_;
            if (access.is_write) {
                ++statistics_.l2_input_transactions;
                if (classified == PerSmL1Outcome::WRITE_HIT) {
                    ++statistics_.write_hits;
                    if (sm_stats != nullptr) ++sm_stats->write_hits;
                    if (!line.ready) ++readiness_.write_hits_on_pending;
                } else {
                    ++statistics_.write_misses;
                    if (sm_stats != nullptr) ++sm_stats->write_misses;
                }
                if (config_.sector32 &&
                    (classified == PerSmL1Outcome::WRITE_HIT || config_.write_allocate))
                    apply_store(line, access, before.set_index);
            } else if (classified == PerSmL1Outcome::READ_HIT) {
                ++statistics_.read_hits;
                ++statistics_.filtered_reads;
                if (sm_stats != nullptr) {
                    ++sm_stats->read_hits;
                    ++sm_stats->filtered_reads;
                }
            } else {
                // Preserve the tag/LRU admission hit, but forward the request
                // to the original L2 MSHR path until this reservation is ready.
                ++statistics_.read_misses;
                ++statistics_.l2_input_transactions;
                ++readiness_.pending_tag_reads;
                if (sm_stats != nullptr) ++sm_stats->read_misses;
                decision.forwarded_sector_mask = missing_mask(line, access);
                decision.read_ticket = issue_read_ticket(access, before.set_index,
                                                        before.hit_way, line.generation,
                                                        decision.forwarded_sector_mask);
                ++line.pending_read_tickets;
                line.resident_sector_mask |= decision.forwarded_sector_mask;
            }
            append_decision(access, decision);
            return decision;
        }

        if (access.is_write) {
            ++statistics_.write_misses;
            ++statistics_.l2_input_transactions;
            if (sm_stats != nullptr) ++sm_stats->write_misses;
            if (!config_.write_allocate) {
                append_decision(access, decision);
                return decision;
            }
        } else {
            ++statistics_.read_misses;
            ++statistics_.l2_input_transactions;
            if (sm_stats != nullptr) ++sm_stats->read_misses;
        }
        const std::size_t chosen = before.empty_way < config_.ways
                                       ? before.empty_way
                                       : before.victim_way;
        if (chosen == config_.ways) {
            // Timing reservation failure is not implemented in the functional
            // interface. Forward without allocation; never evict a protected
            // dirty/pending line merely to make progress.
            decision.reservation_failed = true;
            ++statistics_.unmodeled_reservation_stalls;
            append_decision(access, decision);
            return decision;
        }
        auto& line = lines_[before.base + chosen];
        if (line.occupied) {
            decision.evicted = true;
            ++statistics_.evictions;
            if (sm_stats != nullptr) ++sm_stats->evictions;
            if (line.ready) --ready_lines_;
            if (line.modified) --modified_per_sm_[access.sm_id];
        } else {
            ++resident_per_sm_[static_cast<std::size_t>(access.sm_id)];
            ++total_resident_;
        }
        line = Line{};
        line.allocation_id = access.allocation_id;
        line.canonical_line = access.canonical_line;
        line.last_use = use_clock_;
        line.occupied = true;
        line.ready = false;
        line.generation = next_generation_++;
        if (access.is_write) {
            apply_store(line, access, before.set_index);
        } else {
            decision.read_ticket = issue_read_ticket(access, before.set_index,
                                                    chosen, line.generation,
                                                    decision.forwarded_sector_mask);
            ++line.pending_read_tickets;
            line.resident_sector_mask |= decision.forwarded_sector_mask;
        }
        decision.allocated = true;
        statistics_.peak_resident_lines =
            std::max(statistics_.peak_resident_lines, total_resident_);
        if (sm_stats != nullptr) {
            sm_stats->peak_resident_lines = std::max(
                sm_stats->peak_resident_lines,
                resident_per_sm_[static_cast<std::size_t>(access.sm_id)]);
        }
        append_decision(access, decision);
        return decision;
    }

    void begin_kernel() {
        if (saw_kernel_ &&
            config_.mode == PerSmL1Mode::MODELED_SET_ASSOCIATIVE &&
            config_.persistence == PerSmL1Persistence::KERNEL_FLUSH) {
            flush_all();
        }
        saw_kernel_ = true;
    }

    PerSmL1Statistics statistics() const {
        PerSmL1Statistics result = statistics_;
        result.final_resident_lines = total_resident_;
        for (std::uint32_t sm = 0; sm < config_.num_sms; ++sm) {
            result.per_sm[sm].final_resident_lines = resident_per_sm_[sm];
        }
        return result;
    }

    std::uint64_t live_read_tickets() const {
        return static_cast<std::uint64_t>(live_tickets_.size());
    }

    ReadinessStatistics readiness() const {
        auto result = readiness_;
        result.live_tickets = live_read_tickets();
        result.occupied_lines = total_resident_;
        result.ready_lines = ready_lines_;
        result.pending_lines = total_resident_ - ready_lines_;
        return result;
    }

    ReadinessLine inspect(const PerSmL1Access& access) const {
        if (access.canonical_line % config_.line_bytes != 0)
            throw std::invalid_argument("per-SM L1 inspection line is not aligned");
        const auto location = locate(access);
        ReadinessLine result;
        result.set_index = location.set_index;
        result.way = static_cast<std::uint32_t>(location.hit_way);
        if (location.hit_way < config_.ways) {
            const auto& line = lines_[location.base + location.hit_way];
            result.occupied = line.occupied;
            result.ready = line.ready;
            result.allocation_id = line.allocation_id;
            result.canonical_line = line.canonical_line;
            result.generation = line.generation;
            result.readable_sector_mask = line.readable_sector_mask;
            result.known_byte_masks = line.known_byte_masks;
            result.modified = line.modified;
            result.resident_sector_mask = line.resident_sector_mask;
        }
        return result;
    }

    // Called exactly when this read is delivered, never on mere tag admission.
    // A stale ticket still retires its request; no replacement or LRU update.
    bool complete_read(const ReadFillTicket& ticket) {
        if (!ticket.valid) return false;
        const auto found = live_tickets_.find(ticket.ticket_id);
        if (found == live_tickets_.end() || !same_ticket(found->second, ticket))
            throw std::logic_error("unknown, duplicate or altered L1 read ticket");
        const std::size_t index = static_cast<std::size_t>(ticket.sm_id) *
                                     static_cast<std::size_t>(capacity_lines_per_sm_) +
                                 static_cast<std::size_t>(ticket.set_index) * config_.ways + ticket.way;
        auto& line = lines_.at(index);
        const bool matches = line.occupied && line.allocation_id == ticket.allocation_id &&
                             line.canonical_line == ticket.canonical_line &&
                             line.generation == ticket.generation;
        live_tickets_.erase(found);
        ++readiness_.completed_tickets;
        if (!matches) {
            ++readiness_.stale_tickets;
            return false;
        }
        if (!line.pending_read_tickets)
            throw std::logic_error("L1 pending read accounting underflow");
        --line.pending_read_tickets;
        if (config_.sector32) {
            for (unsigned sector = 0; sector < 4; ++sector)
                if (ticket.sector_mask & (1u << sector))
                    line.known_byte_masks[sector] = UINT32_MAX;
            promote_sectors(line, ticket.sector_mask, ticket.sm_id, ticket.set_index, false);
        } else if (!line.ready) {
            promote_epoch(ticket.sm_id, ticket.set_index);
            line.ready = true;
            ++ready_lines_;
            ++readiness_.ready_transitions;
        }
        return true;
    }

private:
    std::vector<std::uint64_t> ready_promotion_epochs_,ready_promotion_epochs_sm_;
    struct Line {
        int allocation_id = -1;
        std::uint64_t canonical_line = 0;
        // Last access for LRU; whole-line insertion timestamp for FIFO.
        std::uint64_t last_use = 0;
        bool occupied = false;
        bool ready = false;
        std::uint64_t generation = 0;
        std::uint8_t readable_sector_mask = 0;
        std::array<std::uint32_t,4> known_byte_masks{};
        bool modified = false;
        std::uint64_t pending_read_tickets = 0;
        std::uint8_t resident_sector_mask = 0;
    };

    struct Location {
        std::size_t base = 0;
        std::size_t hit_way = 0;
        std::size_t empty_way = 0;
        std::size_t victim_way = 0;
        std::uint32_t set_index = 0;
    };

    void validate_modeled_sm(int sm_id) const {
        if (sm_id < 0 || static_cast<std::uint32_t>(sm_id) >= config_.num_sms) {
            throw std::invalid_argument(
                "modeled per-SM L1 requires a valid scheduler SM id");
        }
    }

    std::uint8_t request_mask(const PerSmL1Access& access) const {
        return config_.sector32 ? (access.sector_mask ? access.sector_mask : 15) : 0;
    }
    void validate_sector_access(const PerSmL1Access& access) const {
        if (!config_.sector32) return;
        if (access.sector_mask & 0xf0)
            throw std::invalid_argument("L1 sector mask exceeds 128 B line");
        const auto requested = request_mask(access);
        for (unsigned s = 0; s < 4; ++s)
            if (access.known_byte_masks[s] && !(requested & (1u << s)))
                throw std::invalid_argument("L1 known bytes outside requested sectors");
    }
    bool read_ready(const Line& line, const PerSmL1Access& access) const {
        return config_.sector32
            ? (line.readable_sector_mask & request_mask(access)) == request_mask(access)
            : line.ready;
    }
    std::uint8_t missing_mask(const Line& line, const PerSmL1Access& access) const {
        return config_.sector32 ? request_mask(access) & ~line.readable_sector_mask : 0;
    }
    void promote_epoch(int sm, std::uint32_t set) {
        auto& epoch = ready_promotion_epochs_[std::size_t(sm) * num_sets_ + set];
        auto& sm_epoch = ready_promotion_epochs_sm_.at(sm);
        if (epoch == std::numeric_limits<std::uint64_t>::max() ||
            sm_epoch == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("L1 host ready-promotion epoch overflow");
        ++epoch;
        ++sm_epoch;
    }
    void promote_sectors(Line& line, std::uint8_t mask, int sm,
                         std::uint32_t set, bool from_store) {
        const auto added = static_cast<std::uint8_t>(mask & ~line.readable_sector_mask);
        if (!added) return;
        promote_epoch(sm, set);
        line.readable_sector_mask |= added;
        for (unsigned s = 0; s < 4; ++s) {
            if (!(added & (1u << s))) continue;
            ++readiness_.sector_readiness_promotions;
            if (from_store) ++readiness_.store_readiness_promotions;
        }
        // Historical line-ready counters now mean all four sectors readable.
        if (!line.ready && line.readable_sector_mask == 15) {
            line.ready = true;
            ++ready_lines_;
            ++readiness_.ready_transitions;
        }
    }
    void apply_store(Line& line, const PerSmL1Access& access, std::uint32_t set) {
        line.resident_sector_mask |= request_mask(access);
        if (!line.modified) {
            line.modified = true;
            ++modified_per_sm_[access.sm_id];
        }
        std::uint8_t known_sectors = 0;
        for (unsigned s = 0; s < 4; ++s) {
            line.known_byte_masks[s] |= access.known_byte_masks[s];
            if (line.known_byte_masks[s] == UINT32_MAX)
                known_sectors |= (1u << s);
        }
        promote_sectors(line, known_sectors, access.sm_id, set, true);
    }

    Location locate(const PerSmL1Access& access) const {
        ++retry_host::counts.l1_locate_calls;
        validate_modeled_sm(access.sm_id);
        Location result;
        const auto line_index = access.canonical_line / config_.line_bytes;
        result.set_index = static_cast<std::uint32_t>(line_index % num_sets_);
        result.base = static_cast<std::size_t>(access.sm_id) *
                          static_cast<std::size_t>(capacity_lines_per_sm_) +
                      static_cast<std::size_t>(result.set_index) * config_.ways;
        result.hit_way = config_.ways;
        result.empty_way = config_.ways;
        result.victim_way = config_.ways;
        std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t way = 0; way < config_.ways; ++way) {
            const auto& line = lines_[result.base + way];
            if (!line.occupied) {
                if (result.empty_way == config_.ways) result.empty_way = way;
                continue;
            }
            if (line.allocation_id == access.allocation_id &&
                line.canonical_line == access.canonical_line) {
                result.hit_way = way;
                break;
            }
            // The source policy protects modified lines while the per-SM
            // modified fraction is below its threshold. A WT eviction does
            // not emit another store. Sector reservations are ineligible too.
            const bool protected_dirty = config_.dirty_protection_percent && line.modified &&
                static_cast<long double>(modified_per_sm_[access.sm_id]) * 100.0L <
                    static_cast<long double>(capacity_lines_per_sm_) * config_.dirty_protection_percent;
            if ((config_.sector32 && line.pending_read_tickets) || protected_dirty) continue;
            if (line.last_use < oldest) {
                oldest = line.last_use;
                result.victim_way = way;
            }
        }
        return result;
    }

    void flush_all() {
        std::uint64_t flushed = 0;
        for (auto& line : lines_) {
            if (!line.occupied) continue;
            line.occupied = false;
            line.ready = false;
            line.readable_sector_mask = 0;
            line.known_byte_masks = {};
            line.modified = false;
            line.pending_read_tickets = 0;
            line.resident_sector_mask = 0;
            ++flushed;
        }
        std::fill(resident_per_sm_.begin(), resident_per_sm_.end(), 0);
        std::fill(modified_per_sm_.begin(), modified_per_sm_.end(), 0);
        total_resident_ = 0;
        ready_lines_ = 0;
        ++statistics_.kernel_flushes;
        statistics_.flushed_lines += flushed;
    }

    static void append_u64(std::uint64_t& hash, std::uint64_t value) {
        constexpr std::uint64_t prime = 1099511628211ULL;
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (8 * byte)) & 0xffULL;
            hash *= prime;
        }
    }

    static bool same_ticket(const ReadFillTicket& a, const ReadFillTicket& b) {
        return a.valid == b.valid && a.sm_id == b.sm_id && a.allocation_id == b.allocation_id &&
               a.set_index == b.set_index && a.way == b.way && a.canonical_line == b.canonical_line &&
               a.generation == b.generation && a.ticket_id == b.ticket_id &&
               a.sector_mask == b.sector_mask;
    }

    ReadFillTicket issue_read_ticket(const PerSmL1Access& access, std::uint32_t set,
                                    std::size_t way, std::uint64_t generation,
                                    std::uint8_t sector_mask) {
        ReadFillTicket result;
        result.valid = true;
        result.sm_id = access.sm_id;
        result.allocation_id = access.allocation_id;
        result.set_index = set;
        result.way = static_cast<std::uint32_t>(way);
        result.canonical_line = access.canonical_line;
        result.generation = generation;
        result.ticket_id = next_ticket_id_++;
        result.sector_mask = sector_mask;
        const auto inserted = live_tickets_.emplace(result.ticket_id, result);
        if (!inserted.second) throw std::logic_error("duplicate L1 read ticket allocation");
        ++readiness_.issued_tickets;
        readiness_.peak_live_tickets = std::max(readiness_.peak_live_tickets, live_read_tickets());
        return result;
    }

    void append_decision(const PerSmL1Access& access,
                         const PerSmL1Decision& decision) {
        const std::uint64_t values[] = {
            next_sequence_++,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(access.sm_id)),
            static_cast<std::uint64_t>(static_cast<std::int64_t>(access.node_id)),
            static_cast<std::uint64_t>(
                static_cast<std::int64_t>(access.allocation_id)),
            access.canonical_line,
            access.is_write ? 1ULL : 0ULL,
            static_cast<std::uint64_t>(decision.outcome),
            decision.set_index,
            decision.forwarded_to_l2 ? 1ULL : 0ULL,
            decision.allocated ? 1ULL : 0ULL,
            decision.evicted ? 1ULL : 0ULL,
        };
        for (const auto value : values) {
            append_u64(statistics_.decision_order_fnv1a64, value);
        }
        if (config_.sector32) {
            append_u64(statistics_.decision_order_fnv1a64, request_mask(access));
            for (const auto mask : access.known_byte_masks)
                append_u64(statistics_.decision_order_fnv1a64, mask);
            append_u64(statistics_.decision_order_fnv1a64, decision.forwarded_sector_mask);
            append_u64(statistics_.decision_order_fnv1a64, decision.reservation_failed);
        }
    }

    PerSmL1Config config_;
    std::uint64_t capacity_lines_per_sm_ = 0;
    std::uint64_t num_sets_ = 0;
    std::vector<Line> lines_;
    std::vector<std::uint64_t> resident_per_sm_;
    std::vector<std::uint64_t> modified_per_sm_;
    std::uint64_t total_resident_ = 0;
    std::uint64_t ready_lines_ = 0;
    std::uint64_t next_generation_ = 1;
    std::uint64_t next_ticket_id_ = 1;
    std::unordered_map<std::uint64_t, ReadFillTicket> live_tickets_;
    ReadinessStatistics readiness_;
    std::uint64_t use_clock_ = 0;
    std::uint64_t next_sequence_ = 0;
    bool saw_kernel_ = false;
    std::uint64_t layout_epoch_ = 0;
    PerSmL1Statistics statistics_;
};

}  // namespace GTSim

#endif  // PER_SM_L1_H
