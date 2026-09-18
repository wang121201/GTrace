#include "physical/hbm/hbm_device.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hbfsim::physical::hbm {
namespace {

void require_positive_count(std::uint64_t value, const char* name) {
    if (value == 0) {
        throw std::runtime_error(std::string(name) + " must be positive");
    }
}

void require_positive_timing(double value, const char* name) {
    if (!(value > 0.0) || !std::isfinite(value)) {
        throw std::runtime_error(std::string(name) + " must be positive and finite");
    }
}

void require_nonnegative_timing(double value, const char* name) {
    if (value < 0.0 || !std::isfinite(value)) {
        throw std::runtime_error(std::string(name) + " must be nonnegative and finite");
    }
}

constexpr double kMaximumCommandClockCycle = 70368744177664.0;  // 2^46

std::uint64_t checked_mul(std::uint64_t lhs, std::uint64_t rhs, const char* name) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw std::runtime_error(std::string(name) + " overflows uint64_t");
    }
    return lhs * rhs;
}

std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs, const char* name) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw std::runtime_error(std::string(name) + " overflows uint64_t");
    }
    return lhs + rhs;
}

std::uint64_t modular_add(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t modulus) {
    // Both operands are reduced, so the sum cannot wrap even when modulus is
    // not a power of two.
    return lhs >= modulus - rhs ? lhs - (modulus - rhs) : lhs + rhs;
}

std::uint64_t modular_subtract(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t modulus) {
    return lhs >= rhs ? lhs - rhs : modulus - (rhs - lhs);
}

std::size_t command_index(HbmCommand command) {
    return static_cast<std::size_t>(command);
}

const char* command_name(HbmCommand command) {
    switch (command) {
    case HbmCommand::ACT:
        return "ACT";
    case HbmCommand::PRE:
        return "PRE";
    case HbmCommand::RD:
        return "RD";
    case HbmCommand::WR:
        return "WR";
    }
    throw std::runtime_error("unknown HBM command");
}

// Golden-ratio hash of the per-pseudo-channel stripe index, taking the HIGH
// multiplier bits: it decorrelates power-of-two request strides before
// pseudo-channel selection (low multiplier bits would still alias for even
// strides).
std::uint64_t pseudo_channel_hash(std::uint64_t stripe) {
    return (stripe * 0x9E3779B97F4A7C15ull) >> 32;
}

std::uint64_t bank_group_hash(std::uint64_t bank_row) {
    return (bank_row * 0xD1B54A32D192ED03ull) >> 32;
}

void mix(std::uint64_t& seed, std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    value ^= value >> 31;
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
}

template <class T, class Compare>
void insert_sorted(std::vector<T>& values, T value, Compare compare) {
    values.insert(
        std::upper_bound(values.begin(), values.end(), value, compare),
        std::move(value));
}

}  // namespace

std::uint64_t HbmConfig::pseudo_channel_width_bits() const {
    if (device.pseudo_channels_per_channel == 0 ||
        device.channel_width_bits % device.pseudo_channels_per_channel != 0) {
        throw std::runtime_error(
            "HBM channel width must divide evenly across pseudo-channels");
    }
    return device.channel_width_bits / device.pseudo_channels_per_channel;
}

std::uint64_t HbmConfig::row_size_bytes() const {
    if (device.pseudo_channels_per_channel == 0 ||
        device.channel_row_size_bytes % device.pseudo_channels_per_channel != 0) {
        throw std::runtime_error(
            "HBM channel row size must divide evenly across pseudo-channels");
    }
    return device.channel_row_size_bytes / device.pseudo_channels_per_channel;
}

std::uint64_t HbmConfig::burst_bytes() const {
    const auto pseudo_width = pseudo_channel_width_bits();
    if (pseudo_width % 8 != 0) {
        throw std::runtime_error("HBM pseudo-channel width must be byte-aligned");
    }
    return checked_mul(pseudo_width / 8, device.burst_length, "HBM burst bytes");
}

std::uint64_t HbmConfig::effective_interleave_bytes() const {
    const auto row = row_size_bytes();
    const auto burst = burst_bytes();
    if (burst > row || row % burst != 0) {
        throw std::runtime_error(
            "HBM derived burst bytes must divide the pseudo-channel row size");
    }
    if (controller.interleave_bytes != 0) {
        if (controller.interleave_bytes % burst != 0 || controller.interleave_bytes > row ||
            row % controller.interleave_bytes != 0) {
            throw std::runtime_error(
                "HBM controller.interleave_bytes=" + std::to_string(controller.interleave_bytes) +
                " must be a multiple of the derived burst bytes (" +
                std::to_string(burst) + ") and divide the pseudo-channel row (" +
                std::to_string(row) + " bytes); set hbm-interleave-bytes to such "
                "a value or to 0 for the automatic default");
        }
        return controller.interleave_bytes;
    }
    // Largest burst multiple that divides the row and stays within the
    // default: walk the divisors of bursts-per-row.
    const auto bursts_per_row = row / burst;
    std::uint64_t best = burst;
    for (std::uint64_t bursts = 1; bursts <= bursts_per_row; ++bursts) {
        if (bursts_per_row % bursts != 0) {
            continue;
        }
        if (bursts > kDefaultInterleaveBytes / burst) {
            break;
        }
        best = bursts * burst;
    }
    return best;
}

double HbmConfig::channel_bandwidth_GBps() const {
    return device.pin_rate_Gbps * static_cast<double>(device.channel_width_bits) / 8.0;
}

double HbmConfig::pseudo_channel_bandwidth_GBps() const {
    return device.pin_rate_Gbps * static_cast<double>(pseudo_channel_width_bits()) / 8.0;
}

double HbmConfig::command_clock_period_ns() const {
    return device.data_rate_per_command_clock / device.pin_rate_Gbps;
}

double HbmConfig::command_clock_MHz() const {
    return 1000.0 / command_clock_period_ns();
}

double HbmConfig::burst_duration_ns() const {
    return static_cast<double>(device.burst_length) / device.pin_rate_Gbps;
}

double HbmConfig::tCCD_S_ns() const {
    return timing.tCCD_S_cycles * command_clock_period_ns();
}

double HbmConfig::tCCD_L_ns() const {
    return timing.tCCD_L_cycles * command_clock_period_ns();
}

std::uint64_t HbmConfig::command_clock_cycles(double time_ns) const {
    if (!(time_ns >= 0.0) || !std::isfinite(time_ns)) {
        throw std::runtime_error(
            "HBM command time must be nonnegative and finite");
    }
    const double tck_ns = command_clock_period_ns();
    require_positive_timing(tck_ns, "HBM derived command-clock period");
    const double cycles = time_ns / tck_ns;
    if (!(cycles < kMaximumCommandClockCycle)) {
        throw std::runtime_error(
            "HBM command time exceeds the representable clock horizon");
    }
    auto eligible = static_cast<std::uint64_t>(std::floor(cycles));
    const auto covered_by_edge = [time_ns, tck_ns](double edge_ns) {
        if (edge_ns >= time_ns) {
            return true;
        }
        const auto infinity = std::numeric_limits<double>::infinity();
        const double tolerance = std::min(
            tck_ns * 1e-6,
            16.0 * std::max({
                std::nextafter(time_ns, infinity) - time_ns,
                std::nextafter(edge_ns, infinity) - edge_ns,
                std::numeric_limits<double>::epsilon() *
                    std::max(1.0, std::abs(time_ns))}));
        return time_ns - edge_ns <= tolerance;
    };
    while (!covered_by_edge(command_clock_time_ns(eligible))) {
        ++eligible;
    }
    while (eligible != 0 && covered_by_edge(command_clock_time_ns(eligible - 1))) {
        --eligible;
    }
    if (eligible >= static_cast<std::uint64_t>(kMaximumCommandClockCycle)) {
        throw std::runtime_error(
            "HBM command time exceeds the representable clock horizon");
    }
    return eligible;
}

double HbmConfig::command_clock_time_ns(std::uint64_t cycles) const {
    return static_cast<double>(cycles) * command_clock_period_ns();
}

double HbmConfig::command_aligned_time_ns(double time_ns) const {
    return std::max(time_ns, command_clock_time_ns(command_clock_cycles(time_ns)));
}

std::string HbmAddress::path() const {
    std::ostringstream out;
    out << "stack" << stack << "/ch" << channel << "/pch" << pseudo_channel
        << "/bg" << bank_group << "/bank" << bank << "/row" << row
        << "/off" << offset;
    return out.str();
}

double HbmStats::row_hit_rate() const {
    const auto total = row_hits + row_misses + row_conflicts;
    return total == 0 ? 0.0 : static_cast<double>(row_hits) / static_cast<double>(total);
}

double HbmStats::active_span_ns() const {
    return std::isfinite(first_arrival_ns) && finish_ns > first_arrival_ns ?
        finish_ns - first_arrival_ns : 0.0;
}

double HbmStats::utilization() const {
    const auto span = active_span_ns();
    return span <= 0.0 || pseudo_channels == 0 ? 0.0 :
        bus_busy_ns / (span * static_cast<double>(pseudo_channels));
}

double HbmStats::bus_parallelism() const {
    const auto span = active_span_ns();
    return span <= 0.0 ? 0.0 : bus_busy_ns / span;
}

double HbmStats::pseudo_channel_busy_skew() const {
    return avg_active_pseudo_channel_busy_ns <= 0.0 ? 0.0 :
        max_pseudo_channel_busy_ns / avg_active_pseudo_channel_busy_ns;
}

HbmDevice::HbmDevice(HbmConfig config, AddressHeatmap* address_heatmap)
    : config_(config), address_heatmap_(address_heatmap) {
    require_positive_count(config_.device.capacity_bytes, "HBM capacity_bytes");
    require_positive_count(config_.device.stacks, "HBM stacks");
    require_positive_count(config_.device.channels_per_stack, "HBM channels_per_stack");
    require_positive_count(config_.device.pseudo_channels_per_channel, "HBM pseudo_channels_per_channel");
    require_positive_count(
        config_.device.bank_groups_per_pseudo_channel,
        "HBM bank_groups_per_pseudo_channel");
    require_positive_count(config_.device.banks_per_group, "HBM banks_per_group");
    require_positive_count(config_.device.channel_row_size_bytes, "HBM channel_row_size_bytes");
    require_positive_count(config_.device.channel_width_bits, "HBM channel_width_bits");
    require_positive_count(config_.device.burst_length, "HBM burst_length");
    require_positive_count(config_.controller.queue_depth, "HBM queue_depth");
    require_positive_timing(config_.device.pin_rate_Gbps, "HBM pin_rate_Gbps");
    require_positive_count(
        config_.device.data_rate_per_command_clock,
        "HBM data_rate_per_command_clock");
    if (config_.device.channel_width_bits % 8 != 0) {
        throw std::runtime_error("HBM channel_width_bits must be byte-aligned");
    }
    const auto row_size_bytes = config_.row_size_bytes();
    const auto burst_bytes = config_.burst_bytes();
    if (burst_bytes > row_size_bytes || row_size_bytes % burst_bytes != 0) {
        throw std::runtime_error(
            "HBM derived burst bytes must divide the pseudo-channel row size");
    }
    // Resolve the automatic interleave once; config() then reports the value
    // the map actually uses.
    config_.controller.interleave_bytes = config_.effective_interleave_bytes();
    if (config_.device.capacity_bytes % burst_bytes != 0) {
        throw std::runtime_error(
            "HBM capacity_bytes must be an integer number of physical bursts");
    }
    const double channel_bandwidth_GBps = config_.channel_bandwidth_GBps();
    const double pseudo_channel_bandwidth_GBps =
        config_.pseudo_channel_bandwidth_GBps();
    const double command_clock_period_ns = config_.command_clock_period_ns();
    const double command_clock_MHz = config_.command_clock_MHz();
    const double burst_duration_ns = config_.burst_duration_ns();
    require_positive_timing(channel_bandwidth_GBps, "HBM derived channel bandwidth");
    require_positive_timing(
        pseudo_channel_bandwidth_GBps,
        "HBM derived pseudo-channel bandwidth");
    require_positive_timing(command_clock_period_ns, "HBM derived command-clock period");
    require_positive_timing(command_clock_MHz, "HBM derived command-clock frequency");
    require_positive_timing(burst_duration_ns, "HBM derived burst duration");
    require_positive_timing(config_.tCCD_S_ns(), "HBM derived tCCD_S");
    require_positive_timing(config_.tCCD_L_ns(), "HBM derived tCCD_L");
    const double system_bandwidth_GBps = channel_bandwidth_GBps *
        static_cast<double>(config_.device.channels_per_stack) *
        static_cast<double>(config_.device.stacks);
    require_positive_timing(system_bandwidth_GBps, "HBM derived system bandwidth");
    if (config_.device.burst_length % config_.device.data_rate_per_command_clock != 0) {
        throw std::runtime_error(
            "HBM burst length must span a whole number of command-clock cycles");
    }
    const auto burst_command_cycles =
        config_.device.burst_length / config_.device.data_rate_per_command_clock;
    if (config_.timing.tCCD_S_cycles < burst_command_cycles) {
        throw std::runtime_error(
            "HBM tCCD_S cycles cannot be shorter than one derived data burst");
    }
    if (config_.timing.tCCD_L_cycles < config_.timing.tCCD_S_cycles) {
        throw std::runtime_error("HBM tCCD_L cycles must be at least tCCD_S cycles");
    }
    require_nonnegative_timing(config_.controller.address_mapping_ns, "HBM address_mapping_ns");
    require_positive_timing(config_.timing.tRCDRD_ns, "HBM tRCDRD_ns");
    require_positive_timing(config_.timing.tRCDWR_ns, "HBM tRCDWR_ns");
    require_positive_timing(config_.timing.tCL_ns, "HBM tCL_ns");
    require_positive_timing(config_.timing.tCWL_ns, "HBM tCWL_ns");
    require_positive_timing(config_.timing.tRP_ns, "HBM tRP_ns");
    require_positive_timing(config_.timing.tRAS_ns, "HBM tRAS_ns");
    require_positive_timing(config_.timing.tRC_ns, "HBM tRC_ns");
    require_positive_timing(config_.timing.tWR_ns, "HBM tWR_ns");
    require_positive_timing(config_.timing.tRTP_ns, "HBM tRTP_ns");
    require_positive_count(config_.timing.tCCD_S_cycles, "HBM tCCD_S_cycles");
    require_positive_count(config_.timing.tCCD_L_cycles, "HBM tCCD_L_cycles");
    require_positive_timing(config_.timing.tRRD_S_ns, "HBM tRRD_S_ns");
    require_positive_timing(config_.timing.tRRD_L_ns, "HBM tRRD_L_ns");
    require_positive_timing(config_.timing.tFAW_ns, "HBM tFAW_ns");
    require_positive_timing(config_.timing.tWTR_S_ns, "HBM tWTR_S_ns");
    require_positive_timing(config_.timing.tWTR_L_ns, "HBM tWTR_L_ns");
    require_positive_timing(config_.timing.tRTW_ns, "HBM tRTW_ns");
    require_positive_timing(config_.timing.tREFI_ns, "HBM tREFI_ns");
    require_positive_timing(config_.timing.tRFC_ns, "HBM tRFC_ns");
    require_positive_timing(config_.timing.tRFCsb_ns, "HBM tRFCsb_ns");
    require_positive_timing(config_.timing.tRREFD_ns, "HBM tRREFD_ns");
    require_nonnegative_timing(config_.controller.frfcfs_cap_ns, "HBM frfcfs_cap_ns");
    if (config_.timing.tRRD_L_ns < config_.timing.tRRD_S_ns) {
        throw std::runtime_error("HBM tRRD_L must be at least tRRD_S");
    }
    if (config_.timing.tWTR_L_ns < config_.timing.tWTR_S_ns) {
        throw std::runtime_error("HBM tWTR_L must be at least tWTR_S");
    }
    // Absolute-ns minima round up to whole command clocks once, here.
    const auto cycles = [this](double value_ns, const char* name) {
        const auto result = config_.command_clock_cycles(value_ns);
        if (result == 0) {
            throw std::runtime_error(
                std::string(name) + " must be at least one command clock");
        }
        return result;
    };
    burst_cycles_ = burst_command_cycles;
    tccd_s_ = config_.timing.tCCD_S_cycles;
    tccd_l_ = config_.timing.tCCD_L_cycles;
    trcdrd_ = cycles(config_.timing.tRCDRD_ns, "HBM effective tRCDRD");
    trcdwr_ = cycles(config_.timing.tRCDWR_ns, "HBM effective tRCDWR");
    tcl_ = cycles(config_.timing.tCL_ns, "HBM effective tCL");
    tcwl_ = cycles(config_.timing.tCWL_ns, "HBM effective tCWL");
    trp_ = cycles(config_.timing.tRP_ns, "HBM effective tRP");
    tras_ = cycles(config_.timing.tRAS_ns, "HBM effective tRAS");
    trc_ = cycles(config_.timing.tRC_ns, "HBM effective tRC");
    twr_ = cycles(config_.timing.tWR_ns, "HBM effective tWR");
    trtp_ = cycles(config_.timing.tRTP_ns, "HBM effective tRTP");
    trrd_s_ = cycles(config_.timing.tRRD_S_ns, "HBM effective tRRD_S");
    trrd_l_ = cycles(config_.timing.tRRD_L_ns, "HBM effective tRRD_L");
    tfaw_ = cycles(config_.timing.tFAW_ns, "HBM effective tFAW");
    twtr_s_ = cycles(config_.timing.tWTR_S_ns, "HBM effective tWTR_S");
    twtr_l_ = cycles(config_.timing.tWTR_L_ns, "HBM effective tWTR_L");
    trtw_ = cycles(config_.timing.tRTW_ns, "HBM effective tRTW");
    trefi_ = cycles(config_.timing.tREFI_ns, "HBM effective tREFI");
    trfc_ = cycles(config_.timing.tRFC_ns, "HBM effective tRFC");
    trfcsb_ = cycles(config_.timing.tRFCsb_ns, "HBM effective tRFCsb");
    trrefd_ = cycles(config_.timing.tRREFD_ns, "HBM effective tRREFD");
    refresh_commands_per_period_ =
        config_.controller.same_bank_refresh ? config_.device.banks_per_group : 1;
    if (config_.controller.refresh_enabled) {
        // Every access must fit between the precharge for one refresh of its
        // bank and the next refresh of that bank; the same-bank command
        // spacing must fit the whole rotation into one tREFI.
        const auto refresh_cycles =
            config_.controller.same_bank_refresh ? trfcsb_ : trfc_;
        const auto longest_access = std::max({
            trcdrd_, trcdwr_, tcl_ + burst_cycles_, tcwl_ + burst_cycles_, trp_});
        if (trp_ + refresh_cycles + longest_access >= trefi_) {
            throw std::runtime_error(
                "HBM tRP plus the refresh window must leave room for an "
                "access inside tREFI");
        }
        if (trefi_ / refresh_commands_per_period_ < trrefd_) {
            throw std::runtime_error(
                "HBM tRREFD must fit banks_per_group same-bank refreshes "
                "into one tREFI");
        }
    }
    auto total_pseudo_channels = checked_mul(
        config_.device.stacks,
        config_.device.channels_per_stack,
        "HBM pseudo-channel topology");
    total_pseudo_channels = checked_mul(
        total_pseudo_channels,
        config_.device.pseudo_channels_per_channel,
        "HBM pseudo-channel topology");
    const auto banks_per_pseudo_channel = checked_mul(
        config_.device.bank_groups_per_pseudo_channel,
        config_.device.banks_per_group,
        "HBM bank topology");
    if (total_pseudo_channels > std::numeric_limits<std::size_t>::max() ||
        banks_per_pseudo_channel > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("HBM topology cannot be represented by size_t");
    }
    row_size_bytes_ = row_size_bytes;
    burst_bytes_ = burst_bytes;
    units_per_row_ = row_size_bytes / config_.controller.interleave_bytes;
    total_pseudo_channels_ = total_pseudo_channels;
    stripe_bytes_ = checked_mul(
        total_pseudo_channels, config_.controller.interleave_bytes, "HBM stripe bytes");
    banks_per_pseudo_channel_ = banks_per_pseudo_channel;
    pseudo_channels_per_stack_ = checked_mul(
        config_.device.channels_per_stack,
        config_.device.pseudo_channels_per_channel,
        "HBM pseudo-channels per stack");
    command_clock_period_ns_ = command_clock_period_ns;
    pseudo_channels_.resize(static_cast<std::size_t>(total_pseudo_channels));
    pseudo_channel_accesses_.assign(pseudo_channels_.size(), 0);
    pseudo_channel_bus_busy_cycles_.assign(pseudo_channels_.size(), 0);
    route_ticket_markers_.assign(
        pseudo_channels_.size(),
        std::numeric_limits<std::uint64_t>::max());
    for (auto& pseudo_channel : pseudo_channels_) {
        pseudo_channel.banks.resize(static_cast<std::size_t>(banks_per_pseudo_channel));
        pseudo_channel.bank_groups.resize(config_.device.bank_groups_per_pseudo_channel);
    }
    refresh_parallel_stats();
}

// ---------------------------------------------------------------------------
// Address map: pch-interleave-bg-rotate-v2
//
//   unit           = addr / interleave_bytes      (contiguous interleave unit)
//   lane, stripe   = unit % P, unit / P           (P = pseudo-channels)
//   pseudo-channel = (lane + hash(stripe)) % P    (reversible high-bit swizzle)
//   within a pseudo-channel, successive stripes rotate bank groups first,
//   then fill the row of the bank they land in (column units), then rotate
//   banks within the group, then rows:
//     lane_bg = stripe % BG; g1 = stripe / BG
//     column_unit = g1 % units_per_row; g2 = g1 / units_per_row
//     bank = g2 % banks_per_group; row = g2 / banks_per_group
//     bank_group = (lane_bg + hash(g2)) % BG
// A request that covers whole stripes presents the same local bank/row/column
// sequence to every pseudo-channel.
// ---------------------------------------------------------------------------

HbmDevice::LocalAddress HbmDevice::local_address(std::uint64_t unit) const {
    const auto bank_groups = config_.device.bank_groups_per_pseudo_channel;
    const auto lane_bg = unit % bank_groups;
    const auto g1 = unit / bank_groups;
    LocalAddress local;
    local.column_unit = g1 % units_per_row_;
    const auto g2 = g1 / units_per_row_;
    local.bank = static_cast<std::uint32_t>(g2 % config_.device.banks_per_group);
    local.row = g2 / config_.device.banks_per_group;
    local.bank_group = static_cast<std::uint32_t>(modular_add(
        lane_bg, bank_group_hash(g2) % bank_groups, bank_groups));
    return local;
}

void HbmDevice::assign_pseudo_channel(
    HbmAddress& addr,
    std::size_t pseudo_channel) const {
    addr.stack = static_cast<std::uint32_t>(
        pseudo_channel / pseudo_channels_per_stack_);
    const auto within_stack = pseudo_channel % pseudo_channels_per_stack_;
    addr.channel = static_cast<std::uint32_t>(
        within_stack / config_.device.pseudo_channels_per_channel);
    addr.pseudo_channel = static_cast<std::uint32_t>(
        within_stack % config_.device.pseudo_channels_per_channel);
}

std::uint64_t HbmDevice::byte_address(
    std::size_t pseudo_channel,
    std::uint64_t unit,
    std::uint64_t unit_offset) const {
    const auto lane = modular_subtract(
        pseudo_channel,
        pseudo_channel_hash(unit) % total_pseudo_channels_,
        total_pseudo_channels_);
    const auto global_unit = checked_add(
        checked_mul(unit, total_pseudo_channels_, "HBM encoded unit"),
        lane,
        "HBM encoded unit");
    return checked_add(
        checked_mul(global_unit, config_.controller.interleave_bytes, "HBM encoded byte address"),
        unit_offset,
        "HBM encoded byte address");
}

HbmAddress HbmDevice::decode(std::uint64_t addr) const {
    if (addr >= config_.device.capacity_bytes) {
        throw std::runtime_error("HBM byte address is out of capacity");
    }
    const auto global_unit = addr / config_.controller.interleave_bytes;
    const auto unit_offset = addr % config_.controller.interleave_bytes;
    const auto lane = global_unit % total_pseudo_channels_;
    const auto stripe = global_unit / total_pseudo_channels_;
    const auto pseudo_channel_linear = modular_add(
        lane,
        pseudo_channel_hash(stripe) % total_pseudo_channels_,
        total_pseudo_channels_);
    const auto local = local_address(stripe);
    HbmAddress decoded;
    assign_pseudo_channel(decoded, static_cast<std::size_t>(pseudo_channel_linear));
    decoded.bank_group = local.bank_group;
    decoded.bank = local.bank;
    decoded.row = local.row;
    decoded.offset = checked_add(
        checked_mul(
            local.column_unit, config_.controller.interleave_bytes, "HBM decoded column offset"),
        unit_offset,
        "HBM decoded burst offset");
    return decoded;
}

std::uint64_t HbmDevice::encode(const HbmAddress& addr) const {
    if (addr.stack >= config_.device.stacks || addr.channel >= config_.device.channels_per_stack ||
        addr.pseudo_channel >= config_.device.pseudo_channels_per_channel ||
        addr.bank_group >= config_.device.bank_groups_per_pseudo_channel ||
        addr.bank >= config_.device.banks_per_group || addr.offset >= row_size_bytes_) {
        throw std::runtime_error("HBM address field out of range");
    }
    const auto bank_groups = config_.device.bank_groups_per_pseudo_channel;
    const auto g2 = checked_add(
        checked_mul(addr.row, config_.device.banks_per_group, "HBM encoded bank row"),
        addr.bank,
        "HBM encoded bank row");
    const auto lane_bg = modular_subtract(
        addr.bank_group, bank_group_hash(g2) % bank_groups, bank_groups);
    const auto column_unit = addr.offset / config_.controller.interleave_bytes;
    const auto unit_offset = addr.offset % config_.controller.interleave_bytes;
    const auto g1 = checked_add(
        checked_mul(g2, units_per_row_, "HBM encoded row units"),
        column_unit,
        "HBM encoded row units");
    const auto stripe = checked_add(
        checked_mul(g1, bank_groups, "HBM encoded stripe"),
        lane_bg,
        "HBM encoded stripe");
    const auto encoded = byte_address(
        pseudo_channel_index(addr), stripe, unit_offset);
    if (encoded >= config_.device.capacity_bytes) {
        throw std::runtime_error("HBM encoded address is out of capacity");
    }
    return encoded;
}

std::size_t HbmDevice::pseudo_channel_index(const HbmAddress& addr) const {
    return (static_cast<std::size_t>(addr.stack) * config_.device.channels_per_stack + addr.channel) *
        config_.device.pseudo_channels_per_channel + addr.pseudo_channel;
}

std::size_t HbmDevice::bank_index(const HbmAddress& addr) const {
    return static_cast<std::size_t>(addr.bank_group) * config_.device.banks_per_group + addr.bank;
}

double HbmDevice::ns(std::uint64_t cycles) const {
    return static_cast<double>(cycles) * command_clock_period_ns_;
}

// ---------------------------------------------------------------------------
// Front end
// ---------------------------------------------------------------------------

PhysicalCompletion HbmDevice::issue(const PhysicalRequest& request) {
    return pump(enqueue(request));
}

void HbmDevice::validate_and_begin_request(const PhysicalRequest& request) {
    if (request.tier != Tier::HBM) {
        throw std::runtime_error("HbmDevice received non-HBM request");
    }
    if (request.op != Op::Read && request.op != Op::Write) {
        throw std::runtime_error("HbmDevice supports read/write requests only");
    }
    if (request.bytes == 0) {
        throw std::runtime_error("HBM request bytes must be positive");
    }
    if (!std::isfinite(request.arrival_ns) || request.arrival_ns < 0.0) {
        throw std::runtime_error("HBM request arrival must be finite and non-negative");
    }
    if (request.addr >= application_capacity_bytes() ||
        request.bytes > application_capacity_bytes() - request.addr) {
        throw std::runtime_error("HBM request range exceeds application capacity after controller reservation");
    }
    if (last_enqueue_arrival_ns_ &&
        request.arrival_ns < *last_enqueue_arrival_ns_) {
        throw std::runtime_error(
            "HBM requests must be enqueued in nondecreasing arrival order; "
            "sort or explicitly admit the event stream before enqueueing");
    }
    last_enqueue_arrival_ns_ = request.arrival_ns;
    floor_bound_cycle_ = config_.command_clock_cycles(
        request.arrival_ns + config_.controller.address_mapping_ns);
    stats_.first_arrival_ns = std::min(stats_.first_arrival_ns, request.arrival_ns);
}

void HbmDevice::reserve_controller_buffer(std::uint64_t bytes) {
    if (last_enqueue_arrival_ns_ || stats_.controller_buffer_transfers || controller_buffer_bytes_)
        throw std::runtime_error("HBM controller storage must be reserved once before traffic");
    const auto rounded = checked_add(bytes, burst_bytes_ - 1, "HBM controller reservation") /
        burst_bytes_ * burst_bytes_;
    if (rounded >= config_.device.capacity_bytes)
        throw std::runtime_error("HBM cannot contain the controller buffer and application memory");
    controller_buffer_bytes_ = rounded;
    buffer_burst_counts_.resize(pseudo_channels_.size(), 0);
    buffer_channels_.reserve(pseudo_channels_.size());
}

void HbmDevice::advance_buffer_frontier(double at_ns) {
    // Reclaim each channel lazily when it is next used. Updating a joint
    // frontier must not flush replicated scheduler state on every admission.
    buffer_frontier_cycle_ = std::max(buffer_frontier_cycle_, config_.command_clock_cycles(at_ns));
}

PhysicalCompletion HbmDevice::transfer_controller_buffer(const PhysicalRequest& request) {
    if (!controller_buffer_bytes_ || request.tier != Tier::HBM ||
        (request.op != Op::Read && request.op != Op::Write) || request.bytes == 0 ||
        !std::isfinite(request.arrival_ns) || request.arrival_ns < 0 ||
        request.addr < application_capacity_bytes() || request.addr >= config_.device.capacity_bytes ||
        request.bytes > config_.device.capacity_bytes - request.addr)
        throw std::runtime_error("invalid reserved HBM controller-buffer transfer");
    flush_service_classes();
    for (const auto index : buffer_channels_) buffer_burst_counts_[index] = 0;
    buffer_channels_.clear();
    const auto add_bursts = [&](std::size_t index, std::uint64_t count) {
        if (buffer_burst_counts_[index] == 0) buffer_channels_.push_back(index);
        buffer_burst_counts_[index] += count;
    };
    const auto last = request.addr + request.bytes - 1;
    auto cursor = request.addr / burst_bytes_;
    auto remaining = last / burst_bytes_ - cursor + 1;
    const auto bursts_per_unit = config_.controller.interleave_bytes / burst_bytes_;
    const auto bursts_per_stripe = stripe_bytes_ / burst_bytes_;
    // A stripe is a permutation of all pseudo-channels. Complete stripes
    // contribute the same integer burst count to each channel; only the two
    // edges need the address swizzle. Bank/row decoding does not affect DMA.
    const auto add_edge = [&](std::uint64_t first, std::uint64_t count) {
        const auto rotation = pseudo_channel_hash(first / bursts_per_stripe) % total_pseudo_channels_;
        auto index = modular_add((first / bursts_per_unit) % total_pseudo_channels_,
            rotation, total_pseudo_channels_);
        auto offset = first % bursts_per_unit;
        while (count != 0) {
            const auto chunk = std::min(count, bursts_per_unit - offset);
            add_bursts(static_cast<std::size_t>(index), chunk);
            count -= chunk;
            offset = 0;
            index = index + 1 == total_pseudo_channels_ ? 0 : index + 1;
        }
    };
    if (const auto offset = cursor % bursts_per_stripe; offset != 0) {
        const auto count = std::min(remaining, bursts_per_stripe - offset);
        add_edge(cursor, count);
        cursor += count;
        remaining -= count;
    }
    const auto stripes = remaining / bursts_per_stripe;
    if (stripes != 0) {
        for (std::size_t index = 0; index < pseudo_channels_.size(); ++index)
            add_bursts(index, stripes * bursts_per_unit);
        cursor += stripes * bursts_per_stripe;
        remaining %= bursts_per_stripe;
    }
    if (remaining != 0) add_edge(cursor, remaining);
    // Preserve the old ascending-channel reduction order, including exact
    // floating-point statistics and the first channel chosen on finish ties.
    std::sort(buffer_channels_.begin(), buffer_channels_.end());
    PhysicalCompletion out;
    out.id = request.id;
    out.tier = Tier::HBM;
    out.op = request.op;
    out.arrival_ns = request.arrival_ns;
    out.start_ns = std::numeric_limits<double>::infinity();
    out.finish_ns = request.arrival_ns;
    out.logical_bytes = request.bytes;
    auto* spans = trace_spans_enabled(request.trace) ? &out.spans : nullptr;
    const auto offered = config_.command_clock_cycles(request.arrival_ns);
    for (const auto index : buffer_channels_) {
        const auto count = buffer_burst_counts_[index];
        auto& pc = pseudo_channels_[index];
        pc.data_bus_cycles.prune_before(buffer_frontier_cycle_);
        const auto duration = ns(count * burst_cycles_);
        const auto start_cycle = static_cast<std::uint64_t>(pc.data_bus_cycles.reserve(offered, count * burst_cycles_));
        const auto start = std::max(request.arrival_ns, ns(start_cycle));
        const auto finish = std::max(start, ns(start_cycle + count * burst_cycles_));
        out.start_ns = std::min(out.start_ns, start);
        if (finish > out.finish_ns) {
            out.finish_ns = finish;
            out.breakdown.scheduler_queue_wait_ns = start - request.arrival_ns;
            out.breakdown.channel_transfer_ns = duration;
        }
        out.physical_bytes += count * burst_bytes_;
        pseudo_channel_accesses_[index] += count;
        pseudo_channel_bus_busy_cycles_[index] += count * burst_cycles_;
        bus_busy_cycles_ += count * burst_cycles_;
        stats_.controller_buffer_bus_busy_ns += duration;
        stats_.stage_work.channel_transfer_ns += duration;
        stats_.stage_work.scheduler_queue_wait_ns += start - request.arrival_ns;
        if (spans) {
            HbmAddress location;
            assign_pseudo_channel(location, index);
            add_trace_span(spans, request.op == Op::Read ? "hbf_buffer_read" : "hbf_buffer_write",
                "hbm_buffer_bus", location.path(), start, finish, false,
                std::to_string(count * burst_bytes_) + "B; shared HBM data channel");
        }
    }
    if (request.op == Op::Read) {
        stats_.read_bytes += out.physical_bytes;
        stats_.controller_buffer_read_bytes += out.physical_bytes;
    } else {
        stats_.write_bytes += out.physical_bytes;
        stats_.controller_buffer_write_bytes += out.physical_bytes;
    }
    ++stats_.controller_buffer_transfers;
    if (address_heatmap_ != nullptr) {
        address_heatmap_->record_contiguous_accesses(
            AddressTrafficRecord{
                .domain = AddressDomain::HbmPhysical,
                .direction = request.op == Op::Read ? TrafficDirection::Read : TrafficDirection::Write,
                .source = HeatmapTrafficSource::Maintenance,
                .address = request.addr - request.addr % burst_bytes_,
                .bytes = burst_bytes_,
            }, out.physical_bytes / burst_bytes_);
    }
    stats_.bus_busy_ns = ns(bus_busy_cycles_);
    stats_.first_arrival_ns = std::min(stats_.first_arrival_ns, request.arrival_ns);
    stats_.finish_ns = std::max(stats_.finish_ns, out.finish_ns);
    return out;
}

void HbmDevice::create_parent(std::uint64_t ticket, const PhysicalRequest& request) {
    PhysicalCompletion aggregate;
    aggregate.id = request.id;
    aggregate.tier = Tier::HBM;
    aggregate.op = request.op;
    aggregate.arrival_ns = request.arrival_ns;
    aggregate.start_ns = std::numeric_limits<double>::infinity();
    aggregate.logical_bytes = request.bytes;
    pending_.emplace(ticket, PendingRequest{
        .completion = std::move(aggregate),
        .remaining_children = 0,
        .total_children = 0,
        .pseudo_channels = 0,
        .critical_pseudo_channel = 0,
        .enqueue_complete = false,
        .has_child_completion = false,
        .retain_diagnostics = request.trace.retain_completion_diagnostics ||
            trace_spans_enabled(request.trace),
    });
    const auto [route_it, route_inserted] =
        ticket_pseudo_channels_.emplace(ticket, std::vector<std::size_t>{});
    if (!route_inserted) {
        throw std::runtime_error("HBM generated a duplicate parent ticket");
    }
}

std::uint64_t HbmDevice::enqueue(const PhysicalRequest& request) {
    flush_service_classes();
    validate_and_begin_request(request);
// INDEPENDENT_PC_BEGIN trace-latch
    independent_trace_safe_ = independent_trace_safe_ &&
        request.trace.mode == TraceMode::Off &&
        !request.trace.retain_completion_diagnostics;
// INDEPENDENT_PC_END trace-latch
    if (next_ticket_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("HBM parent ticket space exhausted");
    }
    const auto ticket = next_ticket_++;
    create_parent(ticket, request);

    const auto first_burst_base = request.addr - request.addr % burst_bytes_;
    const auto request_last = checked_add(
        request.addr, request.bytes - 1, "HBM request end address");
    const auto last_burst_base = request_last - request_last % burst_bytes_;
    const auto burst_count = (last_burst_base - first_burst_base) / burst_bytes_ + 1;
    if (address_heatmap_ != nullptr) {
        address_heatmap_->record_contiguous_accesses(
            AddressTrafficRecord{
                .domain = AddressDomain::HbmPhysical,
                .direction = request.op == Op::Read ?
                    TrafficDirection::Read : TrafficDirection::Write,
                .source = request.heatmap_source,
                .address = first_burst_base,
                .bytes = burst_bytes_,
            },
            burst_count);
    }
    // Every child of one parent shares the arrival, so it shares the first
    // eligible command cycle.
    const auto ready_cycle = floor_bound_cycle_;
    if (replicable(request)) {
        enqueue_replicated(ticket, request, ready_cycle);
    } else {
        enqueue_children(ticket, request, ready_cycle);
    }
    auto& parent = pending_.at(ticket);
    parent.pseudo_channels = ticket_pseudo_channels_.at(ticket).size();
    parent.enqueue_complete = true;
    if (parent.remaining_children == 0) {
        complete_parent(ticket, parent);
    }
    return ticket;
}

void HbmDevice::push_child(
    std::size_t pseudo_channel_index,
    std::uint64_t ticket,
    const PhysicalRequest& request,
    std::uint64_t ready_cycle,
    std::uint64_t bytes,
    HbmAddress addr) {
    auto& queue = pseudo_channels_[pseudo_channel_index].queue;
    // Admission is strictly bounded: service an existing entry before a new
    // child is inserted, never after temporarily exceeding the cap.
    while (queue.size() >= config_.controller.queue_depth) {
        service_one(pseudo_channel_index);
    }
    // The parent cannot complete (and be erased) while its enqueue is open.
    auto& parent = pending_.at(ticket);
    if (parent.remaining_children == std::numeric_limits<std::size_t>::max() ||
        parent.total_children == std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("HBM split request has too many children");
    }
    parent.remaining_children++;
    parent.total_children++;
    queue.push_back(QueuedRequest{
        .ticket = ticket,
        .op = request.op,
        .arrival_ns = request.arrival_ns,
        .ready_cycle = ready_cycle,
        .bytes = bytes,
        .addr = addr,
        .trace = request.trace,
        .bypass_count = 0,
    });
    stats_.max_queue_occupancy = std::max(
        stats_.max_queue_occupancy,
        static_cast<std::uint64_t>(queue.size()));
    if (recording_.active && recording_.representative == pseudo_channel_index &&
        recording_.push_ticket == ticket) {
        recording_.pushed++;
    }
}

void HbmDevice::enqueue_children(
    std::uint64_t ticket,
    const PhysicalRequest& request,
    std::uint64_t ready_cycle) {
    // A controller transaction may not cross a derived DRAM burst; each burst
    // resolves to exactly one pseudo-channel/bank/row/column tuple, and an
    // unaligned logical range is charged for every burst it touches. Children
    // are produced directly into their bounded controller queues.
    auto& routes = ticket_pseudo_channels_.at(ticket);
    std::uint64_t cursor = request.addr;
    std::uint64_t remaining = request.bytes;
    while (remaining != 0) {
        const auto burst_remaining = burst_bytes_ - cursor % burst_bytes_;
        const auto bytes = std::min(remaining, burst_remaining);
        const auto addr = decode(cursor);
        const auto pc_index = pseudo_channel_index(addr);
        if (route_ticket_markers_[pc_index] != ticket) {
            route_ticket_markers_[pc_index] = ticket;
            routes.push_back(pc_index);
        }
        push_child(pc_index, ticket, request, ready_cycle, bytes, addr);
        remaining -= bytes;
        if (remaining != 0) {
            cursor += bytes;
        }
    }
}

// ---------------------------------------------------------------------------
// Symmetric replication
//
// Contiguous requests give groups of pseudo-channels the same local burst
// sequence, including partial stripes. Pseudo-channels whose controller state (bank
// rows, unexpired timing gates and placement history, refresh position) and
// pending queues are identical will therefore execute identical events. One
// representative per equivalence class is serviced by the ordinary scheduler;
// its state and per-parent outcome are then copied to the other class
// members. Completion timing and integer counters are identical to individual
// service; additive floating-point work totals may round differently.
// ---------------------------------------------------------------------------

bool HbmDevice::replicable(const PhysicalRequest& request) const {
    return config_.controller.replicate_symmetric_pseudo_channels &&
        total_pseudo_channels_ > 1 &&
        !trace_spans_enabled(request.trace) &&
        request.bytes >= 2 * config_.controller.interleave_bytes;
}

std::uint64_t HbmDevice::normalization_floor(
    const PseudoChannelState& pseudo_channel) const {
    auto next_cycle = pseudo_channel.queue.empty() ?
        floor_bound_cycle_ : pseudo_channel.queue.front().ready_cycle;
    if (config_.controller.refresh_enabled) {
        next_cycle = std::min(next_cycle, refresh_nominal_cycle(
            pseudo_channel.refresh_period, pseudo_channel.refresh_command));
    }
    return std::max(pseudo_channel.service_floor_cycle, next_cycle);
}

void HbmDevice::prune_placements(
    PseudoChannelState& pseudo_channel,
    std::uint64_t floor) const {
    pseudo_channel.data_bus_cycles.prune_before(
        controller_buffer_bytes_ ? std::min(floor, buffer_frontier_cycle_) : floor);
}

std::uint64_t HbmDevice::normalized_gate(std::uint64_t gate, std::uint64_t floor) {
    return gate <= floor ? 0 : gate;
}

std::uint64_t HbmDevice::symmetry_hash(std::size_t pseudo_channel_index) const {
    const auto& pc = pseudo_channels_[pseudo_channel_index];
    const auto floor = normalization_floor(pc);
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    mix(seed, std::bit_cast<std::uint64_t>(std::max(static_cast<double>(floor), pc.data_bus_cycles.ready_ns)));
    mix(seed, pc.data_bus_cycles.gap_fingerprint_after(floor));
    for (const auto& bank : pc.banks) {
        mix(seed, bank.has_open_row ? bank.open_row + 1 : 0);
        mix(seed, bank.active_request ? *bank.active_request + 1 : 0);
        mix(seed, bank.refresh_start_cycle);
        mix(seed, bank.refresh_until_cycle);
        for (const auto value : bank.ready) {
            mix(seed, normalized_gate(value, floor));
        }
    }
    for (const auto& group : pc.bank_groups) {
        for (const auto value : group.ready) {
            mix(seed, normalized_gate(value, floor));
        }
    }
    mix(seed, normalized_gate(pc.row_command_ready_cycle, floor));
    for (const auto gate : pc.ready) {
        mix(seed, normalized_gate(gate, floor));
    }
    for (const auto cycle : pc.activations) {
        if (cycle + tfaw_ > floor) {
            mix(seed, cycle);
        }
    }
    mix(seed, pc.refresh_period);
    mix(seed, pc.refresh_command);
    const auto next_refresh = refresh_nominal_cycle(pc.refresh_period, pc.refresh_command);
    mix(seed, pc.has_refresh_issue && pc.last_refresh_issue + trrefd_ > next_refresh ?
        pc.last_refresh_issue + 1 : 0);
    mix(seed, pc.queue.size());
    for (const auto& entry : pc.queue) {
        mix(seed, entry.ticket);
        mix(seed, entry.ready_cycle);
        mix(seed, entry.bytes);
        mix(seed, (static_cast<std::uint64_t>(entry.addr.bank_group) << 32) | entry.addr.bank);
        mix(seed, entry.addr.row);
        mix(seed, entry.addr.offset);
        mix(seed, entry.bypass_count);
        mix(seed, entry.progress.has_value());
        if (entry.progress) {
            mix(seed, entry.progress->ready_cycle);
            mix(seed, entry.progress->first_issue ? *entry.progress->first_issue + 1 : 0);
        }
    }
    return seed;
}

bool HbmDevice::symmetric(std::size_t lhs_index, std::size_t rhs_index) const {
    const auto& lhs = pseudo_channels_[lhs_index];
    const auto& rhs = pseudo_channels_[rhs_index];
    const auto bus_floor = static_cast<double>(std::max(normalization_floor(lhs), normalization_floor(rhs)));
    if (std::max(bus_floor, lhs.data_bus_cycles.ready_ns) != std::max(bus_floor, rhs.data_bus_cycles.ready_ns) ||
        lhs.data_bus_cycles.gap_fingerprint_after(bus_floor) != rhs.data_bus_cycles.gap_fingerprint_after(bus_floor) ||
        !lhs.data_bus_cycles.same_gaps_after(rhs.data_bus_cycles, bus_floor))
        return false;
    if (lhs.queue.size() != rhs.queue.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.queue.size(); ++index) {
        const auto& a = lhs.queue[index];
        const auto& b = rhs.queue[index];
        // Trace spans name the pseudo-channel, so traced entries are never
        // interchangeable.
        if (trace_spans_enabled(a.trace) || trace_spans_enabled(b.trace)) {
            return false;
        }
        if (a.ticket != b.ticket || a.op != b.op ||
            a.arrival_ns != b.arrival_ns || a.ready_cycle != b.ready_cycle ||
            a.bytes != b.bytes || a.addr.bank_group != b.addr.bank_group ||
            a.addr.bank != b.addr.bank || a.addr.row != b.addr.row ||
            a.addr.offset != b.addr.offset || a.bypass_count != b.bypass_count ||
            a.trace.retain_completion_diagnostics !=
                b.trace.retain_completion_diagnostics) {
            return false;
        }
        if (a.progress.has_value() != b.progress.has_value()) {
            return false;
        }
        if (a.progress &&
            (a.progress->ready_cycle != b.progress->ready_cycle ||
             a.progress->first_issue != b.progress->first_issue ||
             a.progress->precharged != b.progress->precharged ||
             a.progress->activated != b.progress->activated ||
             a.progress->completion.breakdown != b.progress->completion.breakdown)) {
            return false;
        }
    }
    const auto floor = normalization_floor(lhs);
    if (floor != normalization_floor(rhs)) {
        return false;
    }
    const auto same_gates = [floor](const CommandGates& a, const CommandGates& b) {
        for (std::size_t index = 0; index < a.size(); ++index) {
            if (normalized_gate(a[index], floor) != normalized_gate(b[index], floor)) {
                return false;
            }
        }
        return true;
    };
    for (std::size_t index = 0; index < lhs.banks.size(); ++index) {
        const auto& a = lhs.banks[index];
        const auto& b = rhs.banks[index];
        if (a.has_open_row != b.has_open_row ||
            (a.has_open_row && a.open_row != b.open_row) ||
            a.active_request != b.active_request ||
            a.refresh_start_cycle != b.refresh_start_cycle ||
            a.refresh_until_cycle != b.refresh_until_cycle ||
            !same_gates(a.ready, b.ready)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < lhs.bank_groups.size(); ++index) {
        if (!same_gates(lhs.bank_groups[index].ready, rhs.bank_groups[index].ready)) {
            return false;
        }
    }
    // Placement histories compare after dropping entries that can no longer
    // constrain a command at or after the floor.
    const auto same_history = [](const auto& a, const auto& b, auto&& live, auto&& equal) {
        auto ia = a.begin();
        auto ib = b.begin();
        for (;;) {
            while (ia != a.end() && !live(*ia)) ++ia;
            while (ib != b.end() && !live(*ib)) ++ib;
            if (ia == a.end() || ib == b.end()) {
                return ia == a.end() && ib == b.end();
            }
            if (!equal(*ia, *ib)) {
                return false;
            }
            ++ia;
            ++ib;
        }
    };
    if (!same_gates(lhs.ready, rhs.ready) ||
        normalized_gate(lhs.row_command_ready_cycle, floor) !=
            normalized_gate(rhs.row_command_ready_cycle, floor)) {
        return false;
    }
    if (!same_history(
            lhs.activations, rhs.activations,
            [&](std::uint64_t cycle) { return cycle + tfaw_ > floor; },
            [](std::uint64_t a, std::uint64_t b) { return a == b; })) {
        return false;
    }
    if (lhs.refresh_period != rhs.refresh_period ||
        lhs.refresh_command != rhs.refresh_command) {
        return false;
    }
    const auto next_refresh = refresh_nominal_cycle(lhs.refresh_period, lhs.refresh_command);
    const auto spacing = [this, next_refresh](const PseudoChannelState& pc) {
        return pc.has_refresh_issue && pc.last_refresh_issue + trrefd_ > next_refresh ?
            pc.last_refresh_issue + 1 : 0;
    };
    return spacing(lhs) == spacing(rhs);
}

std::vector<HbmDevice::SymmetryClass> HbmDevice::partition_symmetric(
    const std::vector<std::size_t>& candidates) const {
    std::vector<SymmetryClass> classes;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> classes_by_hash;
    classes_by_hash.reserve(candidates.size());
    for (const auto pseudo_channel : candidates) {
        if (!classes.empty() && symmetric(classes.back().representative, pseudo_channel)) {
            classes.back().members.push_back(pseudo_channel);
            continue;
        }
        auto& bucket = classes_by_hash[symmetry_hash(pseudo_channel)];
        std::optional<std::size_t> matched;
        for (const auto class_index : bucket) {
            // The hash is only an index; membership requires the exact
            // comparison so a collision can never merge distinct states.
            if (symmetric(classes[class_index].representative, pseudo_channel)) {
                matched = class_index;
                break;
            }
        }
        if (matched) {
            classes[*matched].members.push_back(pseudo_channel);
        } else {
            bucket.push_back(classes.size());
            classes.push_back(SymmetryClass{
                .representative = pseudo_channel,
                .members = {},
            });
        }
    }
    return classes;
}

HbmDevice::CounterDelta HbmDevice::counter_snapshot(std::size_t representative) const {
    return CounterDelta{
        .read_bytes = stats_.read_bytes,
        .write_bytes = stats_.write_bytes,
        .row_hits = stats_.row_hits,
        .row_misses = stats_.row_misses,
        .row_conflicts = stats_.row_conflicts,
        .activations = stats_.activations,
        .precharges = stats_.precharges,
        .refresh_count = stats_.refresh_count,
        .bus_busy_cycles = bus_busy_cycles_,
        .accesses = pseudo_channel_accesses_[representative],
        .busy_cycles = pseudo_channel_bus_busy_cycles_[representative],
    };
}

void HbmDevice::begin_recording(std::size_t representative, std::uint64_t push_ticket) {
    recording_ = Recording{};
    recording_.active = true;
    recording_.representative = representative;
    recording_.push_ticket = push_ticket;
    recording_.snapshot = counter_snapshot(representative);
}

void HbmDevice::fold_recording() {
    if (!recording_.active) {
        throw std::runtime_error("HBM replication recording is not open");
    }
    const auto now = counter_snapshot(recording_.representative);
    const auto fold = [](std::uint64_t& total, std::uint64_t current, std::uint64_t then) {
        if (current < then) {
            throw std::runtime_error("HBM replication counter regressed");
        }
        total += current - then;
    };
    auto& delta = recording_.delta;
    const auto& then = recording_.snapshot;
    fold(delta.read_bytes, now.read_bytes, then.read_bytes);
    fold(delta.write_bytes, now.write_bytes, then.write_bytes);
    fold(delta.row_hits, now.row_hits, then.row_hits);
    fold(delta.row_misses, now.row_misses, then.row_misses);
    fold(delta.row_conflicts, now.row_conflicts, then.row_conflicts);
    fold(delta.activations, now.activations, then.activations);
    fold(delta.precharges, now.precharges, then.precharges);
    fold(delta.refresh_count, now.refresh_count, then.refresh_count);
    fold(delta.bus_busy_cycles, now.bus_busy_cycles, then.bus_busy_cycles);
    fold(delta.accesses, now.accesses, then.accesses);
    fold(delta.busy_cycles, now.busy_cycles, then.busy_cycles);
    recording_.snapshot = now;
}

HbmDevice::Recording HbmDevice::pause_recording() {
    fold_recording();
    Recording recording = std::move(recording_);
    recording_ = Recording{};
    return recording;
}

void HbmDevice::resume_recording(Recording recording) {
    recording_ = std::move(recording);
    recording_.active = true;
    recording_.snapshot = counter_snapshot(recording_.representative);
}

void HbmDevice::synchronize_class(const SymmetryClass& symmetry_class) {
    const auto& source = pseudo_channels_[symmetry_class.representative];
    for (const auto member : symmetry_class.members) {
        auto& target = pseudo_channels_[member];
        if (controller_buffer_bytes_) {
            // Equal future command schedules do not imply equal earlier
            // buffer occupancy. New bus placements were replayed separately.
            static_cast<PseudoChannelCommandState&>(target) = source;
        } else {
            target = source;
        }
        for (auto& entry : target.queue) {
            assign_pseudo_channel(entry.addr, member);
            if (entry.progress && entry.trace.retain_completion_diagnostics) {
                entry.progress->completion.resource_path = entry.addr.path();
            }
        }
    }
}

void HbmDevice::flush_service_classes() {
    for (const auto& symmetry_class : service_classes_) {
        synchronize_class(symmetry_class);
    }
    service_classes_.clear();
}

void HbmDevice::replicate_recording(const SymmetryClass& symmetry_class) {
    replay_recording(symmetry_class);
    synchronize_class(symmetry_class);
}

void HbmDevice::replay_recording(const SymmetryClass& symmetry_class) {
    if (!recording_.active || recording_.representative != symmetry_class.representative) {
        throw std::runtime_error("HBM replication recording is not open");
    }
    const Recording recording = pause_recording();
    const auto& delta = recording.delta;
    for (const auto member : symmetry_class.members) {
        for (const auto& placement : recording.data_bus_reservations) {
            auto& bus = pseudo_channels_[member].data_bus_cycles;
            if (bus.reserve(placement.begin_ns, placement.end_ns - placement.begin_ns) != placement.begin_ns)
                throw std::runtime_error("HBM replicated bus placement differs from its representative");
        }
        pseudo_channel_accesses_[member] = checked_add(
            pseudo_channel_accesses_[member], delta.accesses, "HBM pseudo-channel accesses");
        pseudo_channel_bus_busy_cycles_[member] += delta.busy_cycles;
        stats_.read_bytes += delta.read_bytes;
        stats_.write_bytes += delta.write_bytes;
        stats_.row_hits += delta.row_hits;
        stats_.row_misses += delta.row_misses;
        stats_.row_conflicts += delta.row_conflicts;
        stats_.activations += delta.activations;
        stats_.precharges += delta.precharges;
        stats_.refresh_count += delta.refresh_count;
        bus_busy_cycles_ += delta.bus_busy_cycles;
        if (recording.pushed != 0) {
            auto& parent = pending_.at(recording.push_ticket);
            if (parent.remaining_children >
                    std::numeric_limits<std::size_t>::max() - recording.pushed ||
                parent.total_children >
                    std::numeric_limits<std::size_t>::max() - recording.pushed) {
                throw std::runtime_error("HBM split request has too many children");
            }
            parent.remaining_children += recording.pushed;
            parent.total_children += recording.pushed;
        }
        for (const auto& record : recording.records) {
            const auto found = pending_.find(record.ticket);
            if (found == pending_.end() ||
                found->second.remaining_children < record.count) {
                throw std::runtime_error("HBM replicated children of an unknown parent");
            }
            auto& parent = found->second;
            auto& out = parent.completion;
            stats_.stage_work += record.work;
            stats_.replicated_bursts += record.count;
            parent.has_child_completion = true;
            out.start_ns = std::min(out.start_ns, record.min_start_ns);
            if (record.max_finish_ns > out.finish_ns ||
                (record.max_finish_ns == out.finish_ns &&
                 member < parent.critical_pseudo_channel)) {
                out.breakdown = record.critical;
                parent.critical_pseudo_channel = member;
            }
            out.finish_ns = std::max(out.finish_ns, record.max_finish_ns);
            out.physical_bytes = checked_add(
                out.physical_bytes, record.physical_bytes, "HBM aggregate physical bytes");
            parent.remaining_children -= record.count;
            if (parent.remaining_children == 0 && parent.enqueue_complete) {
                complete_parent(record.ticket, parent);
            }
        }
    }
    stats_.bus_busy_ns = ns(bus_busy_cycles_);
}

void HbmDevice::enqueue_replicated(
    std::uint64_t ticket,
    const PhysicalRequest& request,
    std::uint64_t ready_cycle) {
    auto& routes = ticket_pseudo_channels_.at(ticket);
    const auto first_stripe = request.addr / stripe_bytes_;
    const auto last_address = request.addr + request.bytes - 1;
    const auto last_stripe = last_address / stripe_bytes_;
    const auto first_offset = request.addr % stripe_bytes_;
    const auto last_end = last_address % stripe_bytes_ + 1;
    const auto first_rotation = pseudo_channel_hash(first_stripe) % total_pseudo_channels_;
    const auto last_rotation = pseudo_channel_hash(last_stripe) % total_pseudo_channels_;
    struct LocalSpan {
        std::uint64_t begin;
        std::uint64_t end;
        std::vector<std::size_t> candidates;
    };
    std::vector<LocalSpan> spans;
    for (std::size_t index = 0; index < pseudo_channels_.size(); ++index) {
        const auto first_unit = modular_subtract(
            index, first_rotation, total_pseudo_channels_) * config_.controller.interleave_bytes;
        const auto last_unit = modular_subtract(
            index, last_rotation, total_pseudo_channels_) * config_.controller.interleave_bytes;
        const auto begin = first_stripe * config_.controller.interleave_bytes + std::min(
            config_.controller.interleave_bytes, first_offset > first_unit ? first_offset - first_unit : 0);
        const auto end = last_stripe * config_.controller.interleave_bytes + std::min(
            config_.controller.interleave_bytes, last_end > last_unit ? last_end - last_unit : 0);
        if (begin >= end) {
            continue;
        }
        const auto found = std::find_if(spans.begin(), spans.end(), [&](const LocalSpan& span) {
            return span.begin == begin && span.end == end;
        });
        if (found == spans.end()) {
            spans.push_back(LocalSpan{begin, end, {index}});
        } else {
            found->candidates.push_back(index);
        }
    }
    bool replicated = false;
    const auto quantum_bytes = checked_mul(
        row_size_bytes_, banks_per_pseudo_channel_, "HBM replication quantum");
    for (const auto& span : spans) {
        for (auto quantum_begin = span.begin; quantum_begin < span.end;) {
            const auto quantum_end = quantum_begin + std::min(
                span.end - quantum_begin, quantum_bytes - quantum_begin % quantum_bytes);
            for (const auto& symmetry_class : partition_symmetric(span.candidates)) {
                const auto representative = symmetry_class.representative;
                if (!symmetry_class.members.empty()) {
                    begin_recording(representative, ticket);
                }
                HbmAddress addr;
                assign_pseudo_channel(addr, representative);
                auto cursor = quantum_begin;
                while (cursor < quantum_end) {
                    const auto local = local_address(cursor / config_.controller.interleave_bytes);
                    addr.bank_group = local.bank_group;
                    addr.bank = local.bank;
                    addr.row = local.row;
                    addr.offset = local.column_unit * config_.controller.interleave_bytes +
                        cursor % config_.controller.interleave_bytes;
                    const auto bytes = std::min(quantum_end - cursor, burst_bytes_ - cursor % burst_bytes_);
                    push_child(representative, ticket, request, ready_cycle, bytes, addr);
                    cursor += bytes;
                }
                if (quantum_begin == span.begin) {
                    routes.push_back(representative);
                }
                if (!symmetry_class.members.empty()) {
                    replicate_recording(symmetry_class);
                    replicated = true;
                    if (quantum_begin == span.begin) {
                        routes.insert(
                            routes.end(),
                            symmetry_class.members.begin(),
                            symmetry_class.members.end());
                    }
                }
            }
            quantum_begin = quantum_end;
        }
    }
    if (replicated) {
        stats_.replicated_requests++;
    }
}

PhysicalCompletion HbmDevice::pump(std::uint64_t ticket) {
    flush_service_classes();
    const auto routed = ticket_pseudo_channels_.find(ticket);
    if (routed == ticket_pseudo_channels_.end()) {
        throw std::runtime_error("HBM pump on unknown or already-pumped ticket");
    }
    if (completed_.find(ticket) == completed_.end()) {
        service_until_complete(ticket);
    }
    const auto done = completed_.find(ticket);
    if (done == completed_.end()) {
        throw std::runtime_error("HBM parent request lost an internal burst");
    }
    auto completion = std::move(done->second);
    completed_.erase(done);
    ticket_pseudo_channels_.erase(ticket);
    return completion;
}

bool HbmDevice::service_before(double arrival_ns) {
    if (std::isnan(arrival_ns) || arrival_ns < 0.0) {
        throw std::runtime_error("HBM service boundary must be non-negative");
    }
    if (service_classes_.empty()) {
        std::vector<std::size_t> active;
        for (std::size_t index = 0; index < pseudo_channels_.size(); ++index) {
            if (!pseudo_channels_[index].queue.empty()) {
                active.push_back(index);
            }
        }
        if (config_.controller.replicate_symmetric_pseudo_channels) {
            service_classes_ = partition_symmetric(active);
        } else {
            for (const auto index : active) {
                service_classes_.push_back(SymmetryClass{index, {}});
            }
        }
    }
    double next_ns = arrival_ns;
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < service_classes_.size(); ++index) {
        const auto& pseudo_channel = pseudo_channels_[service_classes_[index].representative];
        if (pseudo_channel.queue.empty()) {
            continue;
        }
        const auto event = next_event(pseudo_channel);
        const auto at_ns = event.request_index ?
            std::max(ns(event.issue_cycle),
                     pseudo_channel.queue[*event.request_index].arrival_ns + config_.controller.address_mapping_ns) :
            ns(event.issue_cycle);
        if (at_ns >= arrival_ns || at_ns > next_ns) {
            continue;
        }
        if (at_ns < next_ns) {
            next_ns = at_ns;
            candidates.clear();
        }
        candidates.push_back(index);
    }
    if (candidates.empty()) {
        return false;
    }
    for (const auto index : candidates) {
        const auto& symmetry_class = service_classes_[index];
        if (!symmetry_class.members.empty()) {
            begin_recording(symmetry_class.representative, 0);
        }
        service_one(symmetry_class.representative, arrival_ns);
        if (!symmetry_class.members.empty()) {
            replay_recording(symmetry_class);
        }
    }
    return true;
}

// INDEPENDENT_PC_BEGIN bounded-drain
HbmDevice::IndependentDrainResult HbmDevice::drain_independent_before(double boundary_ns) {
    if (std::isnan(boundary_ns) || boundary_ns < 0.0) {
        throw std::runtime_error("HBM service boundary must be non-negative");
    }
    IndependentDrainResult result;
    const bool supported = std::isfinite(boundary_ns) &&
        !config_.controller.refresh_enabled &&
        !config_.controller.same_bank_refresh &&
        !config_.controller.replicate_symmetric_pseudo_channels &&
        controller_buffer_bytes_ == 0 && buffer_frontier_cycle_ == 0 &&
        !recording_.active && independent_trace_safe_;
    if (!supported) {
        while (service_before(boundary_ns)) {
            ++result.legacy_global_rounds;
        }
        return result;
    }
    result.used_independent = true;
    for (std::size_t index = 0; index < pseudo_channels_.size(); ++index) {
        const auto& pc = pseudo_channels_[index];
        while (!pc.queue.empty()) {
            // Same preview and strict boundary as original service_before.
            const auto event = next_event(pc);
            const auto at_ns = event.request_index ?
                std::max(ns(event.issue_cycle),
                         pc.queue[*event.request_index].arrival_ns + config_.controller.address_mapping_ns) :
                ns(event.issue_cycle);
            if (at_ns >= boundary_ns) break;
            // Preserve prune -> second next_event -> command/aggregation.
            service_one(index, boundary_ns);
            ++result.pc_service_one_calls;
        }
    }
    return result;
}

// INDEPENDENT_PC_END bounded-drain
std::vector<std::pair<std::uint64_t, PhysicalCompletion>>
HbmDevice::take_completions() {
    std::vector<std::pair<std::uint64_t, PhysicalCompletion>> completions;
    completions.reserve(completed_.size());
    for (auto& [ticket, completion] : completed_) {
        completions.emplace_back(ticket, std::move(completion));
        ticket_pseudo_channels_.erase(ticket);
    }
    completed_.clear();
    std::sort(completions.begin(), completions.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    return completions;
}

void HbmDevice::service_until_complete(std::uint64_t ticket) {
    const auto& routes = ticket_pseudo_channels_.at(ticket);
    const auto holds_child = [ticket](const PseudoChannelState& pc) {
        return std::any_of(
            pc.queue.begin(), pc.queue.end(),
            [ticket](const QueuedRequest& entry) { return entry.ticket == ticket; });
    };
    if (!config_.controller.replicate_symmetric_pseudo_channels || routes.size() < 2) {
        // One sweep services every routed pseudo-channel once; the parent's
        // completion is checked between sweeps.
        while (completed_.find(ticket) == completed_.end()) {
            bool progress = false;
            for (const auto pc_index : routes) {
                if (!pseudo_channels_[pc_index].queue.empty()) {
                    service_one(pc_index);
                    progress = true;
                }
            }
            if (!progress) {
                throw std::runtime_error("HBM parent request lost an internal burst");
            }
        }
        return;
    }
    // Same sweep semantics on symmetric classes: every pseudo-channel is
    // serviced once per sweep until the sweep in which the last child of the
    // ticket finishes, so each class receives the maximum service count.
    const auto classes = partition_symmetric(routes);
    std::vector<Recording> recordings(classes.size());
    std::vector<std::uint64_t> services(classes.size(), 0);
    std::uint64_t sweeps = 0;
    for (std::size_t index = 0; index < classes.size(); ++index) {
        const auto representative = classes[index].representative;
        const bool record = !classes[index].members.empty();
        if (record) {
            begin_recording(representative, ticket);
        }
        while (holds_child(pseudo_channels_[representative])) {
            service_one(representative);
            services[index]++;
        }
        sweeps = std::max(sweeps, services[index]);
        if (record) {
            recordings[index] = pause_recording();
        }
    }
    for (std::size_t index = 0; index < classes.size(); ++index) {
        const auto representative = classes[index].representative;
        const bool record = !classes[index].members.empty();
        if (record) {
            resume_recording(std::move(recordings[index]));
        }
        auto& queue = pseudo_channels_[representative].queue;
        while (services[index] < sweeps && !queue.empty()) {
            service_one(representative);
            services[index]++;
        }
        if (record) {
            replicate_recording(classes[index]);
        }
    }
}

void HbmDevice::drain_queues() {
    flush_service_classes();
    if (!config_.controller.replicate_symmetric_pseudo_channels || pseudo_channels_.size() < 2) {
        for (std::size_t index = 0; index < pseudo_channels_.size(); ++index) {
            while (!pseudo_channels_[index].queue.empty()) {
                service_one(index);
            }
        }
        return;
    }
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < pseudo_channels_.size(); ++index) {
        if (!pseudo_channels_[index].queue.empty()) {
            candidates.push_back(index);
        }
    }
    const auto classes = partition_symmetric(candidates);
    for (const auto& symmetry_class : classes) {
        const auto representative = symmetry_class.representative;
        const bool record = !symmetry_class.members.empty();
        if (record) {
            begin_recording(representative, std::numeric_limits<std::uint64_t>::max());
        }
        while (!pseudo_channels_[representative].queue.empty()) {
            service_one(representative);
        }
        if (record) {
            replicate_recording(symmetry_class);
        }
    }
}

// ---------------------------------------------------------------------------
// Command placement
// ---------------------------------------------------------------------------

std::uint64_t HbmDevice::place_activation(
    const PseudoChannelState& pseudo_channel,
    std::uint64_t lower) const {
    auto cycle = std::max(lower, pseudo_channel.ready[command_index(HbmCommand::ACT)]);
    const auto& activations = pseudo_channel.activations;
    if (activations.size() == 4) {
        cycle = std::max(cycle, activations.front() + tfaw_);
    }
    return cycle;
}

std::uint64_t HbmDevice::place_column(
    const PseudoChannelState& pseudo_channel,
    bool write,
    std::uint64_t lower) const {
    const auto cas = write ? tcwl_ : tcl_;
    auto cycle = std::max(
        lower, pseudo_channel.ready[command_index(write ? HbmCommand::WR : HbmCommand::RD)]);
    // An earlier DMA can delay this queued column beyond the joint frontier
    // even though its original eligibility is older. Commands before that
    // frontier have already been serviced, and buffer traffic may have pruned
    // their bus history. Search only the remaining causal interval.
    const auto bus_earliest = std::max(cycle + cas, buffer_frontier_cycle_);
    const auto bus_start = pseudo_channel.data_bus_cycles.preview_start(bus_earliest, burst_cycles_);
    return std::max(cycle, static_cast<std::uint64_t>(bus_start) - cas);
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

HbmDevice::FirstCommand HbmDevice::first_command(
    const PseudoChannelState& pseudo_channel,
    std::size_t request_index) const {
    const auto& queued = pseudo_channel.queue[request_index];
    const auto flat_bank = bank_index(queued.addr);
    const auto& bank = pseudo_channel.banks[flat_bank];
    const auto& group = pseudo_channel.bank_groups[queued.addr.bank_group];
    const auto column = queued.op == Op::Read ? HbmCommand::RD : HbmCommand::WR;
    HbmCommand command = column;
    if (!bank.has_open_row) {
        command = HbmCommand::ACT;
    } else if (bank.open_row != queued.addr.row) {
        command = HbmCommand::PRE;
    }
    if (bank.active_request && *bank.active_request != request_index && command != column) {
        return {};
    }
    const auto index = command_index(command);
    auto issue = std::max({
        queued.progress ? queued.progress->ready_cycle : queued.ready_cycle,
        pseudo_channel.service_floor_cycle, group.ready[index], bank.ready[index]});
    switch (command) {
    case HbmCommand::ACT:
        issue = place_activation(
            pseudo_channel, std::max(issue, pseudo_channel.row_command_ready_cycle));
        break;
    case HbmCommand::PRE:
        issue = std::max(issue, pseudo_channel.row_command_ready_cycle);
        break;
    case HbmCommand::RD:
    case HbmCommand::WR:
        issue = place_column(pseudo_channel, command == HbmCommand::WR, issue);
        break;
    }
    if (config_.controller.refresh_enabled &&
        refresh_targets_bank(pseudo_channel.refresh_command, flat_bank) &&
        refresh_nominal_cycle(pseudo_channel.refresh_period, pseudo_channel.refresh_command) <= issue &&
        bank.active_request != request_index) {
        return {};
    }
    return FirstCommand{
        .issue_cycle = issue, .command = command, .row_hit = command == column};
}

std::size_t HbmDevice::pick_next(const PseudoChannelState& pseudo_channel) const {
    std::size_t selected = pseudo_channel.queue.size();
    FirstCommand selected_preview;
    const auto& oldest = pseudo_channel.queue.front();
    bool capped = false;
    for (std::size_t index = 0; index < pseudo_channel.queue.size(); ++index) {
        const auto& candidate = pseudo_channel.queue[index];
        if (index != 0) {
            capped = capped ||
                pseudo_channel.queue[index - 1].bypass_count >= config_.controller.queue_depth ||
                candidate.arrival_ns - oldest.arrival_ns > config_.controller.frfcfs_cap_ns;
        }
        const bool started = candidate.progress && candidate.progress->first_issue;
        if (capped && !started) {
            continue;
        }
        if (index != 0 && !started) {
            const auto& previous = pseudo_channel.queue[index - 1];
            if (candidate.op == previous.op && candidate.ready_cycle == previous.ready_cycle &&
                candidate.addr.bank_group == previous.addr.bank_group &&
                candidate.addr.bank == previous.addr.bank && candidate.addr.row == previous.addr.row) {
                continue;
            }
        }
        const auto preview = first_command(pseudo_channel, index);
        if (preview.issue_cycle < selected_preview.issue_cycle ||
            (preview.issue_cycle == selected_preview.issue_cycle &&
             preview.row_hit && !selected_preview.row_hit)) {
            selected = index;
            selected_preview = preview;
        }
    }
    return selected;
}

HbmDevice::ScheduledEvent HbmDevice::next_event(
    const PseudoChannelState& pseudo_channel) const {
    ScheduledEvent selected;
    const auto picked = pick_next(pseudo_channel);
    if (picked != pseudo_channel.queue.size()) {
        const auto preview = first_command(pseudo_channel, picked);
        selected = ScheduledEvent{
            .issue_cycle = preview.issue_cycle,
            .request_index = picked,
            .command = preview.command};
    }
    if (!config_.controller.refresh_enabled) {
        return selected;
    }
    const auto nominal = refresh_nominal_cycle(
        pseudo_channel.refresh_period, pseudo_channel.refresh_command);
    auto refresh_cycle = std::max({
        nominal, pseudo_channel.service_floor_cycle, pseudo_channel.row_command_ready_cycle});
    if (pseudo_channel.has_refresh_issue) {
        refresh_cycle = std::max(refresh_cycle, pseudo_channel.last_refresh_issue + trrefd_);
    }
    bool closed = true;
    for (std::size_t index = 0; index < pseudo_channel.banks.size(); ++index) {
        if (!refresh_targets_bank(pseudo_channel.refresh_command, index)) {
            continue;
        }
        const auto& bank = pseudo_channel.banks[index];
        if (bank.active_request) {
            closed = false;
            continue;
        }
        if (bank.has_open_row) {
            closed = false;
            const auto issue = std::max({
                nominal, pseudo_channel.service_floor_cycle,
                pseudo_channel.row_command_ready_cycle,
                bank.ready[command_index(HbmCommand::PRE)]});
            if (issue < selected.issue_cycle ||
                (issue == selected.issue_cycle && selected.request_index)) {
                selected = ScheduledEvent{
                    .issue_cycle = issue, .precharge_bank = index,
                    .command = HbmCommand::PRE};
            }
        } else {
            refresh_cycle = std::max(refresh_cycle, bank.ready[kRefreshGate]);
        }
    }
    if (closed && refresh_cycle <= selected.issue_cycle) {
        selected = ScheduledEvent{.issue_cycle = refresh_cycle};
    }
    return selected;
}

void HbmDevice::service_one(std::size_t pseudo_channel_index, double boundary_ns) {
    auto& pseudo_channel = pseudo_channels_[pseudo_channel_index];
    if (pseudo_channel.queue.empty()) {
        throw std::runtime_error("HBM scheduler serviced an empty queue");
    }
    prune_placements(pseudo_channel, normalization_floor(pseudo_channel));
    const auto event = next_event(pseudo_channel);
    if (event.issue_cycle == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("HBM scheduler has no eligible command");
    }
    const auto event_time = event.request_index ?
        std::max(ns(event.issue_cycle),
                 pseudo_channel.queue[*event.request_index].arrival_ns + config_.controller.address_mapping_ns) :
        ns(event.issue_cycle);
    if (event_time >= boundary_ns) {
        throw std::runtime_error("HBM scheduler crossed its service boundary");
    }
// INDEPENDENT_PC_BEGIN test-command-observation
#if TILEGEN_HBF_INDEPENDENT_PC_TEST
    // Test-only observation occurs at the same post-validation point for both
    // APIs. No production fields, counters, allocation or callbacks remain.
    independent_test_commands_.push_back(IndependentCommandRecord{
        pseudo_channel_index, event.issue_cycle, event.request_index,
        event.precharge_bank, event.command,
        event.request_index ? pseudo_channel.queue[*event.request_index].ticket : 0});
#endif
// INDEPENDENT_PC_END test-command-observation
    if (!event.request_index) {
        auto& owner = pseudo_channel.queue.front();
        auto* spans = trace_spans_enabled(owner.trace) ?
            &command_progress(owner).completion.spans : nullptr;
        if (event.precharge_bank) {
            const auto flat_bank = *event.precharge_bank;
            HbmAddress address;
            assign_pseudo_channel(address, pseudo_channel_index);
            address.bank_group = static_cast<std::uint32_t>(flat_bank / config_.device.banks_per_group);
            address.bank = static_cast<std::uint32_t>(flat_bank % config_.device.banks_per_group);
            add_trace_span(
                spans, "PRE", "hbm_command", spans ? address.path() : std::string{},
                ns(event.issue_cycle), ns(event.issue_cycle + trp_), false, "refresh precharge");
            stats_.precharges++;
            apply_command_state(pseudo_channel, address, HbmCommand::PRE, event.issue_cycle);
            pseudo_channel.service_floor_cycle = event.issue_cycle;
        } else if (!skip_idle_refreshes(pseudo_channel, boundary_ns)) {
            resolve_next_refresh(pseudo_channel, pseudo_channel_index, event.issue_cycle, spans);
        }
        return;
    }
    const auto picked = *event.request_index;
    auto& entry = pseudo_channel.queue[picked];
    const auto preview = FirstCommand{
        .issue_cycle = event.issue_cycle, .command = event.command,
        .row_hit = event.command == HbmCommand::RD || event.command == HbmCommand::WR};
    auto child = service_command(pseudo_channel, entry, preview);
    if (!preview.row_hit) {
        pseudo_channel.banks[bank_index(entry.addr)].active_request = picked;
        return;
    }
    for (std::size_t index = 0; index < picked; ++index) {
        auto& count = pseudo_channel.queue[index].bypass_count;
        if (count < config_.controller.queue_depth) {
            ++count;
        }
    }
    const auto ticket = entry.ticket;
    pseudo_channel.queue.erase(
        pseudo_channel.queue.begin() + static_cast<std::ptrdiff_t>(picked));
    for (auto& bank : pseudo_channel.banks) {
        if (bank.active_request == picked) {
            bank.active_request.reset();
        } else if (bank.active_request && *bank.active_request > picked) {
            --*bank.active_request;
        }
    }
    pseudo_channel_accesses_[pseudo_channel_index] = checked_add(
        pseudo_channel_accesses_[pseudo_channel_index], 1, "HBM pseudo-channel accesses");
    pseudo_channel_bus_busy_cycles_[pseudo_channel_index] += burst_cycles_;
    aggregate_child(ticket, pseudo_channel_index, std::move(child));
}

void HbmDevice::aggregate_child(
    std::uint64_t ticket,
    std::size_t pseudo_channel_index,
    PhysicalCompletion child) {
    const auto found = pending_.find(ticket);
    if (found == pending_.end() || found->second.remaining_children == 0) {
        throw std::runtime_error("HBM completed an unknown internal burst");
    }
    auto& parent = found->second;
    auto& out = parent.completion;
    // Device totals represent physical work, so every burst child
    // contributes. The parent completion retains only its latency-critical
    // child's compact diagnostic; canonical additive work is accumulated
    // separately in stats_.stage_work so parallel children are neither lost
    // nor confused with elapsed wall time.
    stats_.stage_work += child.breakdown;
    const bool first = !parent.has_child_completion;
    parent.has_child_completion = true;
    if (first && parent.retain_diagnostics) {
        out.resource_path = child.resource_path;
        out.note = child.note;
    }
    out.start_ns = std::min(out.start_ns, child.start_ns);
    const bool critical = first || child.finish_ns > out.finish_ns ||
        (child.finish_ns == out.finish_ns &&
         pseudo_channel_index < parent.critical_pseudo_channel);
    out.finish_ns = std::max(out.finish_ns, child.finish_ns);
    out.physical_bytes = checked_add(
        out.physical_bytes, child.physical_bytes, "HBM aggregate physical bytes");
    if (critical) {
        out.breakdown = child.breakdown;
        parent.critical_pseudo_channel = pseudo_channel_index;
    }
    if (parent.retain_diagnostics) {
        out.spans.insert(out.spans.end(), child.spans.begin(), child.spans.end());
    }
    if (recording_.active && recording_.representative == pseudo_channel_index) {
        auto record = std::find_if(
            recording_.records.begin(), recording_.records.end(),
            [ticket](const ChildRecord& candidate) { return candidate.ticket == ticket; });
        if (record == recording_.records.end()) {
            recording_.records.push_back(ChildRecord{.ticket = ticket});
            record = recording_.records.end() - 1;
        }
        record->count++;
        record->physical_bytes = checked_add(
            record->physical_bytes, child.physical_bytes, "HBM replicated physical bytes");
        record->min_start_ns = std::min(record->min_start_ns, child.start_ns);
        if (child.finish_ns > record->max_finish_ns) {
            record->max_finish_ns = child.finish_ns;
            record->critical = child.breakdown;
        }
        record->work += child.breakdown;
    }
    parent.remaining_children--;
    if (parent.remaining_children != 0 || !parent.enqueue_complete) {
        return;
    }
    complete_parent(ticket, parent);
}

void HbmDevice::complete_parent(std::uint64_t ticket, PendingRequest& parent) {
    auto& out = parent.completion;
    if (!std::isfinite(out.start_ns)) {
        out.start_ns = out.arrival_ns;
    }
    if (parent.total_children > 1 && parent.retain_diagnostics) {
        out.resource_path = "hbm/split/" + std::to_string(parent.total_children) +
            "-bursts/" + std::to_string(parent.pseudo_channels) + "-pseudochannels";
        out.note = "split-burst-request";
    }
    completed_.emplace(ticket, std::move(out));
    pending_.erase(ticket);
}

HbmDevice::CommandProgress& HbmDevice::command_progress(QueuedRequest& queued) {
    if (queued.progress) {
        return *queued.progress;
    }
    auto& progress = queued.progress.emplace();
    progress.ready_cycle = queued.ready_cycle;
    auto& out = progress.completion;
    out.tier = Tier::HBM;
    out.op = queued.op;
    out.arrival_ns = queued.arrival_ns;
    out.logical_bytes = queued.bytes;
    out.physical_bytes = burst_bytes_;
    if (queued.trace.retain_completion_diagnostics || trace_spans_enabled(queued.trace)) {
        out.resource_path = queued.addr.path();
    }
    auto* spans = trace_spans_enabled(queued.trace) ? &out.spans : nullptr;
    const auto mapped_ns = queued.arrival_ns + config_.controller.address_mapping_ns;
    const auto ready_ns = std::max(mapped_ns, ns(queued.ready_cycle));
    out.breakdown.address_mapping_ns = config_.controller.address_mapping_ns;
    out.breakdown.scheduler_queue_wait_ns = ready_ns - mapped_ns;
    if (spans != nullptr) {
        add_trace_span(
            spans, "address_map", "mapping", out.resource_path, queued.arrival_ns, mapped_ns);
        add_trace_span(
            spans, "wait_command_clock", "queue", out.resource_path,
            mapped_ns, ready_ns, true, "request becomes command-eligible on the next CK edge");
    }
    return progress;
}

PhysicalCompletion HbmDevice::service_command(
    PseudoChannelState& pseudo_channel,
    QueuedRequest& queued,
    const FirstCommand& scheduled) {
    auto& progress = command_progress(queued);
    auto& out = progress.completion;
    auto& bank = pseudo_channel.banks[bank_index(queued.addr)];
    const auto command = scheduled.command;
    const auto issue = scheduled.issue_cycle;
    const auto column = queued.op == Op::Read ? HbmCommand::RD : HbmCommand::WR;
    const auto cas = queued.op == Op::Read ? tcl_ : tcwl_;
    auto* spans = trace_spans_enabled(queued.trace) ? &out.spans : nullptr;
    const auto event_ns = [this, &queued](std::uint64_t cycle) {
        return std::max(queued.arrival_ns + config_.controller.address_mapping_ns, ns(cycle));
    };
    const auto ready = progress.ready_cycle;
    const auto refresh_begin = std::max(ready, bank.refresh_start_cycle);
    const auto refresh_end = std::min(issue, bank.refresh_until_cycle);
    const auto refresh_wait = refresh_end > refresh_begin ? refresh_end - refresh_begin : 0;
    out.breakdown.refresh_stall_ns += ns(refresh_wait);
    out.breakdown.scheduler_queue_wait_ns += ns(issue - ready - refresh_wait);
    if (spans != nullptr) {
        if (refresh_wait != 0) {
            add_trace_span(
                spans, "wait_refresh", "queue", out.resource_path,
                event_ns(refresh_begin), event_ns(refresh_end), true, "target bank refresh");
        }
        add_trace_span(
            spans, std::string("wait_") + command_name(command), "queue", out.resource_path,
            event_ns(ready), event_ns(refresh_wait != 0 ? refresh_begin : issue), true,
            "command arbitration and timing gates");
        if (refresh_wait != 0) {
            add_trace_span(
                spans, std::string("wait_") + command_name(command), "queue", out.resource_path,
                event_ns(refresh_end), event_ns(issue), true, "command arbitration and timing gates");
        }
    }
    if (!progress.first_issue) {
        progress.first_issue = issue;
        out.start_ns = event_ns(issue);
    }
    std::uint64_t duration = cas;
    std::string detail;
    switch (command) {
    case HbmCommand::PRE:
        duration = trp_;
        out.breakdown.precharge_ns += ns(duration);
        stats_.precharges++;
        progress.precharged = true;
        if (spans) detail = "scope=bank closes_row";
        break;
    case HbmCommand::ACT:
        duration = column == HbmCommand::WR ? trcdwr_ : trcdrd_;
        out.breakdown.activation_ns += ns(duration);
        stats_.activations++;
        progress.activated = true;
        if (spans) detail = "scope=row opens_row row=" + std::to_string(queued.addr.row);
        break;
    case HbmCommand::RD:
    case HbmCommand::WR:
        out.breakdown.command_ns += ns(duration);
        if (spans) detail = "scope=column accesses_column";
        break;
    }
    if (spans != nullptr) {
        add_trace_span(
            spans, command_name(command), "hbm_command", out.resource_path,
            event_ns(issue), event_ns(issue + duration), true, detail);
    }
    apply_command_state(pseudo_channel, queued.addr, command, issue);
    pseudo_channel.service_floor_cycle = issue;
    progress.ready_cycle = issue + duration;
    if (command != column) {
        return {};
    }
    const bool retain_diagnostics =
        queued.trace.retain_completion_diagnostics || trace_spans_enabled(queued.trace);
    if (progress.precharged) {
        stats_.row_conflicts++;
        if (retain_diagnostics) out.note = "row-conflict";
    } else if (progress.activated) {
        stats_.row_misses++;
        if (retain_diagnostics) out.note = "row-miss";
    } else {
        stats_.row_hits++;
        if (retain_diagnostics) out.note = "row-hit";
    }
    const auto bus_start = issue + cas;
    const auto finish = bus_start + burst_cycles_;
    out.breakdown.channel_transfer_ns = ns(burst_cycles_);
    out.finish_ns = event_ns(finish);
    if (spans != nullptr) {
        add_trace_span(
            spans, queued.op == Op::Read ? "read_burst" : "write_burst", "hbm_bus",
            "stack" + std::to_string(queued.addr.stack) + "/ch" +
                std::to_string(queued.addr.channel) + "/pch" +
                std::to_string(queued.addr.pseudo_channel),
            event_ns(bus_start), event_ns(finish), true, std::to_string(out.physical_bytes) + "B");
    }
    if (queued.op == Op::Read) {
        stats_.read_bytes += out.physical_bytes;
    } else {
        stats_.write_bytes += out.physical_bytes;
    }
    bus_busy_cycles_ += burst_cycles_;
    stats_.bus_busy_ns = ns(bus_busy_cycles_);
    stats_.finish_ns = std::max(stats_.finish_ns, out.finish_ns);
    return std::move(out);
}

std::uint64_t HbmDevice::refresh_nominal_cycle(
    std::uint64_t period,
    std::uint32_t command) const {
    return (period + 1) * trefi_ + (command * trefi_) / refresh_commands_per_period_;
}

bool HbmDevice::refresh_targets_bank(
    std::uint32_t command,
    std::size_t bank_index) const {
    return !config_.controller.same_bank_refresh ||
        bank_index % config_.device.banks_per_group == command;
}

void HbmDevice::resolve_next_refresh(
    PseudoChannelState& pseudo_channel,
    std::size_t pseudo_channel_index,
    std::uint64_t issue,
    std::vector<TraceSpan>* spans) {
    const auto command = pseudo_channel.refresh_command;
    const auto nominal = refresh_nominal_cycle(pseudo_channel.refresh_period, command);
    const auto end = issue + (config_.controller.same_bank_refresh ? trfcsb_ : trfc_);
    for (std::size_t index = 0; index < pseudo_channel.banks.size(); ++index) {
        if (!refresh_targets_bank(command, index)) {
            continue;
        }
        auto& bank = pseudo_channel.banks[index];
        if (bank.has_open_row || bank.active_request || bank.ready[kRefreshGate] > issue) {
            throw std::runtime_error("HBM refresh issued before target banks were ready");
        }
        bank.ready[command_index(HbmCommand::ACT)] = std::max(
            bank.ready[command_index(HbmCommand::ACT)], end);
        bank.ready[kRefreshGate] = end;
        bank.refresh_start_cycle = nominal;
        bank.refresh_until_cycle = end;
    }
    stats_.refresh_count++;
    if (spans != nullptr) {
        HbmAddress location;
        assign_pseudo_channel(location, pseudo_channel_index);
        add_trace_span(
            spans, config_.controller.same_bank_refresh ? "REFsb" : "REFab", "refresh",
            "stack" + std::to_string(location.stack) + "/ch" +
                std::to_string(location.channel) + "/pch" +
                std::to_string(location.pseudo_channel),
            ns(issue), ns(end), false,
            config_.controller.same_bank_refresh ?
                "same-bank refresh of bank " + std::to_string(command) + " in every bank group" :
                std::string("all-bank refresh"));
    }
    pseudo_channel.has_refresh_issue = true;
    pseudo_channel.last_refresh_issue = issue;
    pseudo_channel.service_floor_cycle = issue;
    pseudo_channel.row_command_ready_cycle = issue + 1;
    if (command + 1 == refresh_commands_per_period_) {
        pseudo_channel.refresh_command = 0;
        pseudo_channel.refresh_period++;
    } else {
        pseudo_channel.refresh_command = command + 1;
    }
}

bool HbmDevice::skip_idle_refreshes(PseudoChannelState& pseudo_channel, double boundary_ns) {
    const auto nominal = refresh_nominal_cycle(
        pseudo_channel.refresh_period, pseudo_channel.refresh_command);
    if (pseudo_channel.open_rows != 0 || pseudo_channel.refresh_command != 0 ||
        pseudo_channel.row_command_ready_cycle > nominal ||
        (pseudo_channel.has_refresh_issue && pseudo_channel.last_refresh_issue + trrefd_ > nominal) ||
        std::any_of(pseudo_channel.queue.begin(), pseudo_channel.queue.end(),
                    [](const QueuedRequest& entry) { return trace_spans_enabled(entry.trace); }) ||
        std::any_of(pseudo_channel.banks.begin(), pseudo_channel.banks.end(),
                    [nominal](const BankState& bank) {
                        return bank.active_request || bank.ready[kRefreshGate] > nominal;
                    })) {
        return false;
    }
    const auto ready_ns = std::max(
        ns(pseudo_channel.queue.front().ready_cycle),
        pseudo_channel.queue.front().arrival_ns + config_.controller.address_mapping_ns);
    const auto limit_ns = std::min(boundary_ns, ready_ns);
    auto limit = config_.command_clock_cycles(limit_ns);
    if (ns(limit) >= limit_ns) {
        if (limit == 0) {
            return false;
        }
        --limit;
    }
    const auto commands = refresh_commands_per_period_;
    const auto last_offset = (static_cast<std::uint64_t>(commands - 1) * trefi_) / commands;
    if (limit < last_offset) {
        return false;
    }
    const auto periods = (limit - last_offset) / trefi_;
    if (periods <= pseudo_channel.refresh_period) {
        return false;
    }
    const auto skipped = periods - pseudo_channel.refresh_period;
    const auto last_start = periods * trefi_;
    const auto duration = config_.controller.same_bank_refresh ? trfcsb_ : trfc_;
    for (std::size_t index = 0; index < pseudo_channel.banks.size(); ++index) {
        const auto command = config_.controller.same_bank_refresh ? index % config_.device.banks_per_group : 0;
        const auto start = last_start + (command * trefi_) / commands;
        auto& bank = pseudo_channel.banks[index];
        bank.ready[command_index(HbmCommand::ACT)] = std::max(
            bank.ready[command_index(HbmCommand::ACT)], start + duration);
        bank.ready[kRefreshGate] = start + duration;
        bank.refresh_start_cycle = start;
        bank.refresh_until_cycle = start + duration;
    }
    stats_.refresh_count += skipped * commands;
    pseudo_channel.refresh_period = periods;
    pseudo_channel.has_refresh_issue = true;
    pseudo_channel.last_refresh_issue = last_start + last_offset;
    pseudo_channel.service_floor_cycle = pseudo_channel.last_refresh_issue;
    pseudo_channel.row_command_ready_cycle = pseudo_channel.last_refresh_issue + 1;
    return true;
}

void HbmDevice::apply_command_state(
    PseudoChannelState& pseudo_channel,
    const HbmAddress& addr,
    HbmCommand command,
    std::uint64_t issue) {
    auto& bank = pseudo_channel.banks[bank_index(addr)];
    auto& group = pseudo_channel.bank_groups[addr.bank_group];
    const auto raise = [](std::uint64_t& gate, std::uint64_t value) {
        gate = std::max(gate, value);
    };
    constexpr auto kAct = static_cast<std::size_t>(HbmCommand::ACT);
    constexpr auto kPre = static_cast<std::size_t>(HbmCommand::PRE);
    constexpr auto kRd = static_cast<std::size_t>(HbmCommand::RD);
    constexpr auto kWr = static_cast<std::size_t>(HbmCommand::WR);
    const auto reserve_column_bus = [&](bool write) {
        const auto begin = issue + (write ? tcwl_ : tcl_);
        if (pseudo_channel.data_bus_cycles.reserve(begin, burst_cycles_) != begin)
            throw std::runtime_error("HBM data-bus reservation changed after command selection");
        if (controller_buffer_bytes_ && recording_.active &&
            recording_.representative == pseudo_channel_index(addr)) {
            auto& placements = recording_.data_bus_reservations;
            const auto end = begin + burst_cycles_;
            if (!placements.empty() && placements.back().end_ns == begin) {
                placements.back().end_ns = static_cast<double>(end);
            } else {
                placements.push_back({static_cast<double>(begin), static_cast<double>(end)});
            }
        }
        raise(bank.ready[kRefreshGate], issue + (write ? tcwl_ : tcl_) + burst_cycles_);
    };
    switch (command) {
    case HbmCommand::PRE:
        pseudo_channel.row_command_ready_cycle = issue + 1;
        if (bank.has_open_row) {
            bank.has_open_row = false;
            pseudo_channel.open_rows--;
        }
        raise(bank.ready[kAct], issue + trp_);
        // A refresh of a precharged bank waits tRP after its PRE.
        raise(bank.ready[kRefreshGate], issue + trp_);
        break;
    case HbmCommand::ACT:
        pseudo_channel.row_command_ready_cycle = issue + 1;
        if (!bank.has_open_row) {
            bank.has_open_row = true;
            pseudo_channel.open_rows++;
        }
        bank.open_row = addr.row;
        raise(bank.ready[kPre], issue + tras_);
        raise(bank.ready[kAct], issue + std::max(trc_, tras_ + trp_));
        raise(bank.ready[kRd], issue + trcdrd_);
        raise(bank.ready[kWr], issue + trcdwr_);
        raise(group.ready[kAct], issue + trrd_l_);
        raise(pseudo_channel.ready[kAct], issue + trrd_s_);
        if (pseudo_channel.activations.size() == 4) {
            pseudo_channel.activations.erase(pseudo_channel.activations.begin());
        }
        pseudo_channel.activations.push_back(issue);
        break;
    case HbmCommand::RD:
        raise(pseudo_channel.ready[kRd], issue + tccd_s_);
        raise(pseudo_channel.ready[kWr], issue + std::max(tccd_s_, trtw_));
        raise(bank.ready[kPre], issue + trtp_);
        // Column commands stay in order inside a bank and its bank group.
        raise(bank.ready[kRd], issue + tccd_l_);
        raise(bank.ready[kWr], issue + tccd_l_);
        raise(group.ready[kRd], issue + tccd_l_);
        raise(group.ready[kWr], issue + tccd_l_);
        reserve_column_bus(false);
        break;
    case HbmCommand::WR:
        raise(pseudo_channel.ready[kRd], issue + std::max(tccd_s_, tcwl_ + burst_cycles_ + twtr_s_));
        raise(pseudo_channel.ready[kWr], issue + tccd_s_);
        raise(bank.ready[kPre], issue + tcwl_ + burst_cycles_ + twr_);
        raise(bank.ready[kRd], issue + std::max(tccd_l_, tcwl_ + burst_cycles_ + twtr_l_));
        raise(bank.ready[kWr], issue + tccd_l_);
        raise(group.ready[kWr], issue + tccd_l_);
        raise(group.ready[kRd], issue + std::max(tccd_l_, tcwl_ + burst_cycles_ + twtr_l_));
        reserve_column_bus(true);
        break;
    }
}

void HbmDevice::refresh_parallel_stats() const {
    stats_.pseudo_channels = pseudo_channels_.size();
    stats_.active_pseudo_channels = 0;
    stats_.max_pseudo_channel_accesses = 0;
    stats_.max_pseudo_channel_busy_ns = 0.0;
    stats_.avg_active_pseudo_channel_busy_ns = 0.0;
    double active_busy_ns = 0.0;
    for (std::size_t index = 0; index < pseudo_channels_.size(); ++index) {
        const auto accesses = pseudo_channel_accesses_[index];
        const auto busy_ns = ns(pseudo_channel_bus_busy_cycles_[index]);
        if (accesses == 0 && busy_ns <= 0.0) {
            continue;
        }
        ++stats_.active_pseudo_channels;
        active_busy_ns += busy_ns;
        stats_.max_pseudo_channel_busy_ns =
            std::max(stats_.max_pseudo_channel_busy_ns, busy_ns);
        stats_.max_pseudo_channel_accesses =
            std::max(stats_.max_pseudo_channel_accesses, accesses);
    }
    if (stats_.active_pseudo_channels != 0) {
        stats_.avg_active_pseudo_channel_busy_ns =
            active_busy_ns / static_cast<double>(stats_.active_pseudo_channels);
    }
}

} // namespace hbfsim::physical::hbm
