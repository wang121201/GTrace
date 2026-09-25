#pragma once
#include <cstdint>
#include <stdexcept>

namespace direct_native {
// Empirical LLM policy; no phase/address/model dependent choice or mutable seed.
class EfHitThrottle {
public:
    static constexpr std::uint32_t denominator=65536;
    static constexpr std::uint64_t fixed_seed=0x9e3779b97f4a7c15ULL;
    static constexpr std::uint64_t multiplier=2685821657736338717ULL;
private:
    std::uint32_t numerator_;
    std::uint64_t state_=fixed_seed,eligible_=0,promoted_=0;
public:
    explicit EfHitThrottle(std::uint32_t numerator=0):numerator_(numerator){
        if(numerator>denominator)throw std::invalid_argument("EF hit numerator must be 0..65536");
    }
    bool choose(){
        ++eligible_;
        if(numerator_==0)return false;
        if(numerator_==denominator){++promoted_;return true;}
        state_^=state_>>12;state_^=state_<<25;state_^=state_>>27;
        const bool promoted=((state_*multiplier)>>48)<numerator_;
        promoted_+=promoted;return promoted;
    }
    std::uint32_t numerator()const{return numerator_;}
    std::uint64_t eligible()const{return eligible_;}
    std::uint64_t promoted()const{return promoted_;}
};
}
