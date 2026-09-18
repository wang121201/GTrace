#pragma once
#include <cstdint>
#ifndef TILEGEN_HOST_ISSUE_CERTIFICATE
#define TILEGEN_HOST_ISSUE_CERTIFICATE 1
#endif
namespace GTSim::host_issue {
// Stores only the existing forecast's conservative absolute lower bound.
// A state-changing notification always invalidates it, even if that notification
// does not ultimately release a ready node. It never advances simulated time.
template<class Cycle> struct Certificate {
    bool valid=false;
    Cycle earliest=0;
    void invalidate() noexcept { valid=false; }
    void publish(Cycle lower_bound) noexcept { earliest=lower_bound;valid=true; }
    bool excludes(Cycle now)const noexcept { return valid && now<earliest; }
};
}
