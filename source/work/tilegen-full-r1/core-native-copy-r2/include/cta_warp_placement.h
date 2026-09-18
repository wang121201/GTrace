#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace GTSim::cta_placement {
// Source CTA/local-warp IDs are separate from this scheduler ownership token.
// For W=4, S=48, P=4 this is exactly 4*CTA+warp. For W=2 it
// assigns consecutive CTA-local warps on each SM to successive SPs.
inline int token(int c,int w,int W,int S,int P,bool per_sm) {
    if(c<0 || W<1 || W>32 || w<0 || w>=W || S<1 || P!=4)
        throw std::runtime_error("invalid scheduler placement coordinate");
    const std::int64_t local=std::int64_t(c/S)*W+w;
    const std::int64_t t=per_sm ? (local/P)*(std::int64_t(S)*P)+(c%S)*P+local%P : std::int64_t(c)*W+w;
    if(t>=std::numeric_limits<int>::max())throw std::runtime_error("scheduler token domain overflow");
    return int(t);
}
inline int domain(int count,int W,int S,int P,bool per_sm) {
    if(count<1)throw std::runtime_error("empty scheduler placement domain");
    // Each SM's largest token belongs to its last CTA and last local warp.
    // Iterate at most S CTAs, not all nodes or all CTAs.
    int maximum=-1;const int first=count>S?count-S:0;
    for(int c=first;c<count;++c){int t=token(c,W-1,W,S,P,per_sm);if(t>maximum)maximum=t;}
    return maximum+1;
}
}
