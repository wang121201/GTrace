#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace GTSim {
using Cycle=std::int64_t;
inline Cycle checked_cycle(std::uint64_t c) {
    if(c>static_cast<std::uint64_t>(std::numeric_limits<Cycle>::max()))
        throw std::overflow_error("cycle exceeds signed 64-bit domain");
    return static_cast<Cycle>(c);
}
inline Cycle cycle_add(Cycle c,Cycle duration) {
    if(c<0||duration<0||c>std::numeric_limits<Cycle>::max()-duration)
        throw std::overflow_error("cycle addition overflow");
    return c+duration;
}
}
