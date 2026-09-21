#pragma once
#include "per_sm_l1.h"
#include "cache_geometry.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace GTSim {
// Pinned software profile e09511b9d168632e...; sizes are bytes, clocks MHz.
// No claim of a discovered NVIDIA replacement/hash implementation.
struct AdaKernelResources {
    std::uint32_t threads_per_cta;
    std::uint32_t registers_per_thread; // zero is an explicit source value, not unknown
    std::uint32_t shared_bytes_per_cta; // total static + dynamic launch shared bytes
    std::uint64_t grid_ctas;
};
struct AdaKernelAllocation {
    std::uint32_t padded_threads, rounded_registers;
    std::uint32_t resident_ctas_per_sm;
    std::uint32_t shared_carveout_bytes, l1_bytes, l1_ways;
};
struct AdaTunerProfile {
    static constexpr unsigned sms=48, schedulers_per_sm=4, warp_threads=32;
    static constexpr unsigned max_threads_per_sm=1536, max_ctas_per_sm=24;
    static constexpr unsigned registers_per_sm=65536, shared_bytes_per_sm=102400;
    static constexpr unsigned unified_bytes=131072, l1_sets=4, line_bytes=128;
    static constexpr unsigned channels=10, subpartitions_per_channel=2;
    static constexpr unsigned l2_bytes=20*1024*16*128;
    static constexpr double core_mhz=2175.0, dram_mhz=4500.5;
    static constexpr std::array<unsigned,6> shared_options={0,8192,16384,32768,65536,102400};
    // Mirrors shader_core_config::max_cta in pinned shader.cc:4407-4517.
    // No stronger physical-SM occupancy claim; upstream rounds regs by 4.
    static AdaKernelAllocation allocate(const AdaKernelResources& r) {
        if(!r.threads_per_cta || r.threads_per_cta>1024 || !r.grid_ctas ||
           r.registers_per_thread>65536 || r.shared_bytes_per_cta>shared_bytes_per_sm)
            throw std::invalid_argument("invalid or unknown Ada launch resources");
        const unsigned threads=(r.threads_per_cta+31)/32*32;
        const unsigned regs=(r.registers_per_thread+3)/4*4;
        unsigned resident=std::min(max_threads_per_sm/threads,max_ctas_per_sm);
        if(r.shared_bytes_per_cta)resident=std::min(resident,shared_bytes_per_sm/r.shared_bytes_per_cta);
        if(regs)resident=std::min<std::uint64_t>(resident,registers_per_sm/(std::uint64_t(threads)*regs));
        const auto grid_resident=r.grid_ctas/sms+(r.grid_ctas%sms!=0);
        resident=unsigned(std::min<std::uint64_t>(resident,grid_resident));
        if(!resident)throw std::invalid_argument("Ada kernel has zero legal resident CTAs");
        const auto needed=std::uint64_t(resident)*r.shared_bytes_per_cta;
        auto it=std::lower_bound(shared_options.begin(),shared_options.end(),needed);
        if(it==shared_options.end())throw std::logic_error("no legal Ada carveout");
        const unsigned l1=unified_bytes-*it;
        return {threads,regs,resident,*it,l1,l1/(l1_sets*line_bytes)};
    }
    static PerSmL1Config l1(const AdaKernelAllocation& a) {
        PerSmL1Config c;
        c.mode=PerSmL1Mode::MODELED_SET_ASSOCIATIVE;
        c.persistence=PerSmL1Persistence::KERNEL_FLUSH;
        c.num_sms=sms;c.line_bytes=line_bytes;c.capacity_bytes_per_sm=a.l1_bytes;c.ways=a.l1_ways;
        c.store_bypass=false;c.hit_latency_cycles=34;
        c.sector32=true;c.write_allocate=true;c.dirty_protection_percent=25;
        return c;
    }
    static L2GeometryConfig l2_geometry(){return L2GeometryConfig::accelsim_rtx4000_ada_v1();}
};
}
