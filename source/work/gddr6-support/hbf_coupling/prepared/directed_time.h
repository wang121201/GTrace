#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
namespace sg_hbf::directed_time {
inline std::uint64_t checked_mul(
    std::uint64_t lhs,
    std::uint64_t rhs,
    const char* description) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw std::runtime_error(
            std::string(description) + " overflows uint64_t");
    }
    return lhs * rhs;
}

inline double picoseconds_to_ns_never_early(std::uint64_t value) {
    auto result = static_cast<double>(value) / 1000.0;
    if (!std::isfinite(result) || result < 0.0) {
        throw std::runtime_error(
            "coupled issue time cannot be represented in nanoseconds");
    }

    const auto covers_exact_picoseconds = [value](double nanoseconds) {
        if (value == 0) return nanoseconds >= 0.0;
        if (!std::isfinite(nanoseconds) || nanoseconds <= 0.0) return false;

        // frexp exposes the exact binary64 value as
        // significand * 2^(exponent - digits).  Comparing that rational after
        // multiplication by 1000 avoids another rounded floating operation.
        int exponent = 0;
        const auto fraction = std::frexp(nanoseconds, &exponent);
        const auto significand = static_cast<std::uint64_t>(
            std::ldexp(fraction, std::numeric_limits<double>::digits));
        const auto scaled_significand = checked_mul(
            significand, 1000, "coupled binary64 picosecond comparison");
        const auto shift = exponent - std::numeric_limits<double>::digits;
        constexpr auto bits = std::numeric_limits<std::uint64_t>::digits;
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (shift >= 0) {
            if (shift >= bits ||
                scaled_significand > (maximum >> shift)) {
                return true;
            }
            return (scaled_significand << shift) >= value;
        }

        const auto right_shift = -shift;
        if (right_shift >= bits || value > (maximum >> right_shift)) {
            return false;
        }
        return scaled_significand >= (value << right_shift);
    };

    // The uint64 -> binary64 conversion and the division are both rounded, so
    // a fixed one-ULP correction is insufficient.  Walk to the exact directed
    // ceiling and prove the postcondition with integer arithmetic.
    while (result > 0.0) {
        const auto prior = std::nextafter(result, 0.0);
        if (!covers_exact_picoseconds(prior)) break;
        result = prior;
    }
    while (!covers_exact_picoseconds(result)) {
        result = std::nextafter(
            result, std::numeric_limits<double>::infinity());
        if (!std::isfinite(result)) {
            throw std::runtime_error(
                "coupled issue time cannot be represented in nanoseconds");
        }
    }
    return result;
}

inline std::uint64_t nanoseconds_to_picoseconds_never_early(double value) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::runtime_error(
            "coupled completion time must be finite and nonnegative");
    }
    if (value == 0.0) return 0;
    const auto scaled = std::nextafter(
        value * 1000.0, std::numeric_limits<double>::infinity());
    if (!std::isfinite(scaled) ||
        scaled >= static_cast<double>(
            std::numeric_limits<std::uint64_t>::max())) {
        throw std::runtime_error(
            "coupled completion time overflows integer picoseconds");
    }
    return static_cast<std::uint64_t>(std::ceil(scaled));
}

}
