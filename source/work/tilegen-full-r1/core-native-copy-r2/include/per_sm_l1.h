#ifndef PER_SM_L1_H
#define PER_SM_L1_H

#include <algorithm>
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
    bool sector_validity = false; // OFF preserves legacy whole-line completion; ON uses 32B validity.
    // Integer model value derived from the 7.062-cycle dependent-load L1
    // candidate in MEMORY_FOOTPRINT_RESULTS.md.  It is uncalibrated.
    int hit_latency_cycles = 7;
};

struct PerSmL1Access {
    int sm_id = -1;
    int allocation_id = -1;
    std::uint64_t canonical_line = 0;
    bool is_write = false;
    int node_id = -1;
    bool bypass_l1 = false; // per-request cache operator; no allocation or hit filtering
    std::uint8_t requested_sector_mask = 15; // independent of L2 dirty-store mask
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
    std::uint8_t fill_sector_mask = 15;
};

struct ReadinessLine {
    std::uint8_t valid_sector_mask = 0;
    bool occupied = false;
    bool ready = false;
    std::uint32_t set_index = 0;
    std::uint32_t way = 0;
    int allocation_id = -1;
    std::uint64_t canonical_line = 0;
    std::uint64_t generation = 0;
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
};

struct PerSmL1Decision {
    PerSmL1Outcome outcome = PerSmL1Outcome::BYPASS;
    std::uint32_t set_index = 0;
    bool forwarded_to_l2 = true;
    bool allocated = false;
    bool evicted = false;
    std::uint8_t requested_sector_mask = 0, missing_sector_mask = 0, forwarded_read_sector_mask = 0;
    ReadFillTicket read_ticket;
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
    std::uint64_t read_sector_requests=0, read_sector_hits=0, read_sector_misses=0;
    std::uint64_t write_sector_requests=0, write_sector_hits=0, write_sector_misses=0;
    std::uint64_t forwarded_read_sector_requests=0, forwarded_write_sector_requests=0;
    std::uint64_t bypassed_read_sector_requests=0, bypassed_write_sector_requests=0;
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
};

struct L1ReadMissMemo {
    const void* owner=nullptr;
    std::uint64_t line=0,epoch=0;
    int sm=-1,allocation=-1;
    bool valid=false;
    std::uint8_t requested_sector_mask=15;
};

class PerSmL1Cache {
public:
    explicit PerSmL1Cache(const PerSmL1Config& config = PerSmL1Config())
        : config_(config) {
        if ((config_.sector_validity && config_.line_bytes!=128) || config_.num_sms == 0 || config_.line_bytes == 0 ||
            config_.capacity_bytes_per_sm == 0 || config_.ways == 0 ||
            config_.hit_latency_cycles < 0 ||
            config_.capacity_bytes_per_sm % config_.line_bytes != 0) {
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
        ready_promotion_epochs_.assign(static_cast<std::size_t>(num_sets_)*config_.num_sms,0);
        ready_promotion_epochs_sm_.assign(config_.num_sms,0);
        statistics_.per_sm.resize(config_.num_sms);
        for (std::uint32_t sm = 0; sm < config_.num_sms; ++sm) {
            statistics_.per_sm[sm].sm_id = sm;
        }
    }

    const PerSmL1Config& config() const { return config_; }
    std::uint64_t host_ready_epoch(int sm) const { return ready_promotion_epochs_sm_.at(sm); }

    PerSmL1Outcome classify(const PerSmL1Access& access) const {
        ++retry_host::counts.l1_classify_calls;
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
        if (config_.sector_validity && (!access.requested_sector_mask || access.requested_sector_mask>15))
            throw std::invalid_argument("invalid L1 requested-sector mask");
        const auto location = locate(access);
        if (access.is_write) {
            return location.hit_way < config_.ways
                       ? PerSmL1Outcome::WRITE_HIT
                       : PerSmL1Outcome::WRITE_MISS;
        }
        return location.hit_way < config_.ways &&
                       lines_[location.base + location.hit_way].ready &&
                       (!config_.sector_validity || (lines_[location.base + location.hit_way].valid_sector_mask & access.requested_sector_mask)==access.requested_sector_mask)
                   ? PerSmL1Outcome::READ_HIT
                   : PerSmL1Outcome::READ_MISS;
    }

    // A cached negative may only bypass a repeated, blocked, side-effect-free
    // rejection. No positive hit/victim/LRU state survives across calls.
    bool same_negative_read(const PerSmL1Access& a,const L1ReadMissMemo& memo) const {
        if(a.bypass_l1 || a.is_write || config_.mode!=PerSmL1Mode::MODELED_SET_ASSOCIATIVE || !memo.valid)return false;
        if(memo.owner!=this || memo.sm!=a.sm_id || memo.allocation!=a.allocation_id || memo.line!=a.canonical_line||(config_.sector_validity&&memo.requested_sector_mask!=a.requested_sector_mask)){++retry_host::counts.memo_identity_rechecks;return false;}
        const auto set=(a.canonical_line/config_.line_bytes)%num_sets_;
        const auto epoch=ready_promotion_epochs_[std::size_t(a.sm_id)*num_sets_+set];
        if(memo.epoch!=epoch){++retry_host::counts.memo_epoch_rechecks;return false;}
        return true;
    }
    void remember_negative_read(const PerSmL1Access& a,L1ReadMissMemo& memo) const {
        if(a.bypass_l1 || a.is_write || config_.mode!=PerSmL1Mode::MODELED_SET_ASSOCIATIVE){memo.valid=false;return;}
        // Called only after original classify successfully validated this key.
        const auto set=(a.canonical_line/config_.line_bytes)%num_sets_;
        memo={this,a.canonical_line,ready_promotion_epochs_[std::size_t(a.sm_id)*num_sets_+set],a.sm_id,a.allocation_id,true,a.requested_sector_mask};
        ++retry_host::counts.memo_negative_records;
    }

    PerSmL1Decision access(const PerSmL1Access& access) {
        const PerSmL1Outcome classified = classify(access);
        PerSmL1Decision decision;
        decision.outcome = classified;
        decision.forwarded_to_l2 = classified != PerSmL1Outcome::READ_HIT;

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
            count_sectors(access,0,decision,true);
            ++statistics_.bypassed_transactions;
            ++statistics_.l2_input_transactions;
            if (sm_stats != nullptr) ++sm_stats->bypassed;
            append_decision(access, decision);
            return decision;
        }

        const auto before = locate(access);
        count_sectors(access,before.hit_way<config_.ways?lines_[before.base+before.hit_way].valid_sector_mask:0,decision,false);
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
        ++use_clock_;

        if (before.hit_way < config_.ways) {
            auto& line = lines_[before.base + before.hit_way];
            line.last_use = use_clock_;
            if (access.is_write) {
                ++statistics_.write_hits;
                ++statistics_.l2_input_transactions;
                if (sm_stats != nullptr) ++sm_stats->write_hits;
                if (!line.ready) ++readiness_.write_hits_on_pending;
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
                decision.read_ticket = issue_read_ticket(access, before.set_index,
                                                        before.hit_way, line.generation);
            }
            append_decision(access, decision);
            return decision;
        }

        if (access.is_write) {
            ++statistics_.write_misses;
            ++statistics_.l2_input_transactions;
            if (sm_stats != nullptr) ++sm_stats->write_misses;
            append_decision(access, decision);
            return decision;
        }

        ++statistics_.read_misses;
        ++statistics_.l2_input_transactions;
        if (sm_stats != nullptr) ++sm_stats->read_misses;
        const std::size_t chosen = before.empty_way < config_.ways
                                       ? before.empty_way
                                       : before.victim_way;
        auto& line = lines_[before.base + chosen];
        if (line.occupied) {
            decision.evicted = true;
            ++statistics_.evictions;
            if (sm_stats != nullptr) ++sm_stats->evictions;
            if (line.ready) --ready_lines_;
        } else {
            ++resident_per_sm_[static_cast<std::size_t>(access.sm_id)];
            ++total_resident_;
        }
        line.allocation_id = access.allocation_id;
        line.canonical_line = access.canonical_line;
        line.last_use = use_clock_;
        line.occupied = true;
        line.ready = false;
        line.valid_sector_mask = 0;
        line.generation = next_generation_++;
        decision.read_ticket = issue_read_ticket(access, before.set_index,
                                                chosen, line.generation);
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
            result.valid_sector_mask = line.valid_sector_mask;
            result.occupied = line.occupied;
            result.ready = line.ready;
            result.allocation_id = line.allocation_id;
            result.canonical_line = line.canonical_line;
            result.generation = line.generation;
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
        const auto new_mask=std::uint8_t(line.valid_sector_mask|ticket.fill_sector_mask);
        if (new_mask!=line.valid_sector_mask) {
            auto& epoch=ready_promotion_epochs_[std::size_t(ticket.sm_id)*num_sets_+ticket.set_index];
            if(epoch==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("L1 host ready-promotion epoch overflow");
            auto& sm_epoch=ready_promotion_epochs_sm_.at(ticket.sm_id);
            if(sm_epoch==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("L1 host SM ready-promotion epoch overflow");
            ++epoch;++sm_epoch;
            if(!line.ready)++ready_lines_;
            line.valid_sector_mask=new_mask;
            line.ready = true;
            ++readiness_.ready_transitions;
        }
        return true;
    }

private:
    static unsigned sector_count(std::uint8_t mask){unsigned n=0;for(unsigned i=0;i<4;++i)n+=(mask>>i)&1;return n;}
    void count_sectors(const PerSmL1Access& a,std::uint8_t valid,PerSmL1Decision& d,bool bypass){
        const auto requested=a.requested_sector_mask;
        if(!requested||requested>15)throw std::invalid_argument("invalid L1 requested-sector mask");
        const auto missing=std::uint8_t(requested&~valid);
        d.requested_sector_mask=requested;d.missing_sector_mask=missing;
        const auto requests=sector_count(requested),misses=sector_count(missing);
        if(bypass)(a.is_write?statistics_.bypassed_write_sector_requests:statistics_.bypassed_read_sector_requests)+=requests;
        if(a.is_write){statistics_.write_sector_requests+=requests;statistics_.write_sector_misses+=misses;statistics_.write_sector_hits+=requests-misses;statistics_.forwarded_write_sector_requests+=requests;}
        else{statistics_.read_sector_requests+=requests;statistics_.read_sector_misses+=misses;statistics_.read_sector_hits+=requests-misses;
            d.forwarded_read_sector_mask=!d.forwarded_to_l2?0:(!config_.sector_validity?15:missing);
            statistics_.forwarded_read_sector_requests+=sector_count(d.forwarded_read_sector_mask);}
    }
    std::vector<std::uint64_t> ready_promotion_epochs_,ready_promotion_epochs_sm_;
    struct Line {
        int allocation_id = -1;
        std::uint64_t canonical_line = 0;
        std::uint64_t last_use = 0;
        bool occupied = false;
        bool ready = false;
        std::uint8_t valid_sector_mask = 0;
        std::uint64_t generation = 0;
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
        result.victim_way = 0;
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
            line.valid_sector_mask = 0;
            ++flushed;
        }
        std::fill(resident_per_sm_.begin(), resident_per_sm_.end(), 0);
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
               a.generation == b.generation && a.ticket_id == b.ticket_id && a.fill_sector_mask==b.fill_sector_mask;
    }

    ReadFillTicket issue_read_ticket(const PerSmL1Access& access, std::uint32_t set,
                                    std::size_t way, std::uint64_t generation) {
        ReadFillTicket result;
        result.valid = true;
        result.sm_id = access.sm_id;
        result.allocation_id = access.allocation_id;
        result.set_index = set;
        result.way = static_cast<std::uint32_t>(way);
        result.canonical_line = access.canonical_line;
        result.generation = generation;
        result.ticket_id = next_ticket_id_++;
        const auto index=std::size_t(access.sm_id)*capacity_lines_per_sm_+std::size_t(set)*config_.ways+way;
        result.fill_sector_mask=config_.sector_validity?std::uint8_t(access.requested_sector_mask&~lines_.at(index).valid_sector_mask):15;
        if(!result.fill_sector_mask)throw std::logic_error("empty missing-sector read ticket");
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
    }

    PerSmL1Config config_;
    std::uint64_t capacity_lines_per_sm_ = 0;
    std::uint64_t num_sets_ = 0;
    std::vector<Line> lines_;
    std::vector<std::uint64_t> resident_per_sm_;
    std::uint64_t total_resident_ = 0;
    std::uint64_t ready_lines_ = 0;
    std::uint64_t next_generation_ = 1;
    std::uint64_t next_ticket_id_ = 1;
    std::unordered_map<std::uint64_t, ReadFillTicket> live_tickets_;
    ReadinessStatistics readiness_;
    std::uint64_t use_clock_ = 0;
    std::uint64_t next_sequence_ = 0;
    bool saw_kernel_ = false;
    PerSmL1Statistics statistics_;
};

}  // namespace GTSim

#endif  // PER_SM_L1_H
