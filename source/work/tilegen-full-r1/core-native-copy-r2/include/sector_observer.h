#ifndef SECTOR_OBSERVER_H
#define SECTOR_OBSERVER_H

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace GTSim {

// The native GTSim memory path groups accesses by a 128-byte L2 line and does
// not retain a byte or 32-byte sector mask.  This contract deliberately keeps
// modeled full-line accounting separate from unavailable hardware counters.
enum class SectorObserverMode {
    HARDWARE_EXACT,
    MODELED_FULL_LINE_EQUIVALENT_ONLY,
};

enum class HardwareCounterAvailability {
    AVAILABLE,
    UNAVAILABLE_PERMISSION,
};

struct SectorObservationContract {
    SectorObserverMode mode;
    HardwareCounterAvailability hardware_counter_availability;
    std::uint32_t gtsim_line_bytes;
    std::uint32_t hardware_sector_bytes;
    std::uint32_t sectors_per_line;
    bool gtsim_sector_mask_available;
    bool exact_hardware_requested_sectors_available;
    bool full_line_equivalent_is_hardware_measurement;
    bool conditional_envelope_requires_request_identity_mapping;
    const char* traffic_claim;
};

struct SectorProjection {
    std::uint64_t modeled_line_occurrences;
    std::uint64_t modeled_full_line_equivalent_bytes;
    std::uint64_t modeled_full_line_equivalent_sectors;
    std::uint64_t conditional_min_requested_sectors;
    std::uint64_t conditional_max_requested_sectors;
    bool conditional_on_one_hardware_request_per_modeled_line;
    bool exact_hardware_requested_sectors_available;
};

inline std::uint64_t checked_sector_product(std::uint64_t value,
                                            std::uint64_t factor) {
    if (factor != 0 && value > std::numeric_limits<std::uint64_t>::max() / factor) {
        throw std::overflow_error("sector projection overflow");
    }
    return value * factor;
}

inline SectorObservationContract
make_rtx4000_ada_sector_observation_contract() {
    return {
        SectorObserverMode::MODELED_FULL_LINE_EQUIVALENT_ONLY,
        HardwareCounterAvailability::UNAVAILABLE_PERMISSION,
        128,
        32,
        4,
        false,
        false,
        false,
        true,
        "MODELED_128B_FULL_LINE_EQUIVALENT_ONLY",
    };
}

inline SectorProjection project_modeled_line_occurrences(
        const SectorObservationContract& contract,
        std::uint64_t modeled_line_occurrences) {
    if (contract.gtsim_line_bytes == 0 || contract.hardware_sector_bytes == 0 ||
        contract.gtsim_line_bytes % contract.hardware_sector_bytes != 0 ||
        contract.sectors_per_line !=
            contract.gtsim_line_bytes / contract.hardware_sector_bytes) {
        throw std::invalid_argument("inconsistent line/sector observation contract");
    }
    return {
        modeled_line_occurrences,
        checked_sector_product(modeled_line_occurrences,
                               contract.gtsim_line_bytes),
        checked_sector_product(modeled_line_occurrences,
                               contract.sectors_per_line),
        modeled_line_occurrences,
        checked_sector_product(modeled_line_occurrences,
                               contract.sectors_per_line),
        contract.conditional_envelope_requires_request_identity_mapping,
        contract.exact_hardware_requested_sectors_available,
    };
}

inline std::uint64_t admit_exact_hardware_requested_sectors(
        const SectorObservationContract& contract,
        std::uint64_t measured_requested_sectors) {
    if (!contract.exact_hardware_requested_sectors_available ||
        contract.hardware_counter_availability !=
            HardwareCounterAvailability::AVAILABLE) {
        throw std::runtime_error(
            "exact hardware requested-sector count is unavailable");
    }
    return measured_requested_sectors;
}

}  // namespace GTSim

#endif  // SECTOR_OBSERVER_H
