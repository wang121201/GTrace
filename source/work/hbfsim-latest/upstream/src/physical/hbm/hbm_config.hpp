#pragma once
#include <cstdint>
#include <string_view>

namespace hbfsim::physical::hbm {
inline constexpr std::string_view kStandard = "JEDEC-JESD270-4-2025-04";
// Organization and reference rate follow the public JEDEC announcement.
// Absolute timing minima remain product assumptions, not recovered JEDEC tables.
struct HbmDeviceConfig {
    std::uint64_t capacity_bytes = 48ull * 1024ull * 1024ull * 1024ull;
    std::uint32_t stacks = 1;
    std::uint32_t channels_per_stack = 32;
    std::uint32_t pseudo_channels_per_channel = 2;
    std::uint32_t bank_groups_per_pseudo_channel = 16;
    std::uint32_t banks_per_group = 4;
    // Interface geometry and rate are the single source of truth for HBM
    // data movement. A channel is divided evenly into pseudo-channels. Each
    // pseudo-channel burst transfers burst_length beats over its share of the
    // DQ pins, so width/rate/BL derive both bytes and duration; callers cannot
    // configure an inconsistent aggregate GB/s or independent tBL.
    std::uint64_t channel_row_size_bytes = 2048;
    std::uint32_t channel_width_bits = 64;
    std::uint32_t burst_length = 8;
    double pin_rate_Gbps = 8.0;
    // HBM3/HBM4 use a command clock below the DQ transfer rate. The ratio is
    // explicit because tCCD is specified in command-clock cycles.
    std::uint32_t data_rate_per_command_clock = 4;
};

struct HbmTimingConfig {
    double tRCDRD_ns = 14.0;
    double tRCDWR_ns = 14.0;
    double tCL_ns = 14.0;
    double tCWL_ns = 10.0;
    double tRP_ns = 14.0;
    double tRAS_ns = 32.0;
    double tRC_ns = 46.0;
    double tWR_ns = 15.0;
    double tRTP_ns = 7.5;
    std::uint32_t tCCD_S_cycles = 2;
    std::uint32_t tCCD_L_cycles = 4;
    double tRRD_S_ns = 4.0;
    double tRRD_L_ns = 6.0;
    double tFAW_ns = 20.0;
    double tWTR_S_ns = 4.0;
    double tWTR_L_ns = 8.0;
    double tRTW_ns = 8.0;
    double tREFI_ns = 3900.0;
    double tRFC_ns = 350.0;
    double tRFCsb_ns = 160.0;
    double tRREFD_ns = 10.0;
};

struct HbmControllerConfig {
    // Contiguous bytes served by one pseudo-channel before the map rotates to
    // the next one. An explicit value must be a multiple of the derived burst
    // bytes and divide the per-pseudo-channel row bytes; 0 selects the
    // largest such value that does not exceed 256 B (256 B on every shipped
    // geometry). The resolved value is reported by effective_interleave_bytes()
    // and by HbmDevice::config().
    std::uint64_t interleave_bytes = 0;
    double address_mapping_ns = 0.0;
    bool refresh_enabled = true;
    bool same_bank_refresh = true;
    // FR-FCFS scheduling: per-pseudo-channel pending-queue depth (a real
    // controller holds a few tens of requests per channel; a full queue
    // backpressures admission) and the anti-starvation time window. In
    // addition to this time gate, an entry may be bypassed at most queue_depth
    // times, so equal-timestamp row hits cannot starve an old conflict.
    std::uint32_t queue_depth = 32;
    double frfcfs_cap_ns = 5000.0;
    // Host-time fast path (config key hbm-replicate-symmetric-pseudo-channels):
    // when a contiguous request presents the same local burst sequence to
    // pseudo-channels in identical controller state, one representative is
    // simulated and its outcome is copied to the others. Results are
    // identical in completion timing and integer counters to individual
    // service; additive floating-point work totals may round differently.
    bool replicate_symmetric_pseudo_channels = true;
};

}
