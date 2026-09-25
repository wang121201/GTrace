#pragma once
#include <cstdint>

namespace GTSim {

// Exact SOFTWARE mapping for the pinned Accel-Sim RTX 4000 Ada configuration;
// this is not a claim about NVIDIA's unpublished physical address hash.
// Source: addrdec.cc (gap path, CONSECUTIVE, one chiplet), gpu-cache.cc (X),
// and NVIDIA_RTX_4000_Ada_Generation/gpgpusim.config in ../reference.
// addrdec.cc SHA256 de15bbe5c7a6e18827da26d743cfa5bce6a002d9a8a4342d23794b9104cedb8e
// gpu-cache.cc SHA256 0a8bded03d50b69049f65c4cfd59f277b160a2d8165672d66c18ec2cda86a8e6
// hashing.cc SHA256 14746fbbc84b1928c9346368af3c4ca12df64c6f405057fb11f95af9f817eec2
// config SHA256 e09511b9d168632e975422b19a9557ff5ed4482ace160fa428f9c70455f5a891
// Mapping: dramid@8; ...0000RRRR.RRRRRRRR.RBBBCCCC.BCCSSSSS
// The 10-channel division happens BEFORE extracting bank/row/column fields.
struct AdaAddressMapping {
    using U = std::uint64_t;
    static constexpr U channels = 10;
    static constexpr U subpartitions_per_channel = 2;
    static constexpr U subpartitions = channels * subpartitions_per_channel;
    static constexpr U sets_per_subpartition = 1024;
    static constexpr U ways = 16;
    static constexpr U line_bytes = 128;
    static constexpr U cache_bytes = subpartitions * sets_per_subpartition * ways * line_bytes;
    static constexpr U bank_mask = 0x7080;
    static constexpr U row_mask = 0x0fff8000;
    static constexpr U column_mask = 0x0f7f;
    static constexpr U burst_mask = 0x001f;
    static constexpr U subpartition_mask = 0x0080; // lowest selected BANK bit

    struct Decoded {
        U chip;                   // memory channel 0..9 (upstream tlx.chip)
        U bank;                   // upstream tlx.bk, including subpartition bit
        U row;
        U column;
        U burst;
        U sub_partition;          // global id 0..19: chip * 2 + (bank & 1)
        U rest_of_address;        // address after non-power-of-two channel division
        U partition_address;      // rest with BANK bit7 packed out (not cleared)
        U l2_set;                 // X index of partition_address, 0..1023
        U group;                  // flat subpartition/set id, 0..20479
    };

    static constexpr U channel(U address) noexcept { return (address >> 8) % channels; }
    static constexpr U rest_address(U address) noexcept {
        return (((address >> 8) / channels) << 8) | (address & 0xff);
    }
    static constexpr U sub_partition(U address) noexcept {
        return channel(address) * subpartitions_per_channel + ((address >> 7) & 1);
    }
    static constexpr U partition_address(U address) noexcept {
        // addrdec_packbits(~0x80, rest, 64, 0). The bank selector is omitted
        // from the local set address; retaining it would double set camping.
        return (((address >> 8) / channels) << 7) | (address & 0x7f);
    }
    static constexpr U set(U address) noexcept {
        const U local = partition_address(address);
        return ((local >> 7) ^ (local >> 17)) & (sets_per_subpartition - 1);
    }
    static constexpr U group(U address) noexcept {
        return sub_partition(address) * sets_per_subpartition + set(address);
    }
    static constexpr Decoded decode(U address) noexcept {
        const U rest = rest_address(address);
        const U bank = ((rest >> 7) & 1) | (((rest >> 12) & 7) << 1);
        const U part = partition_address(address);
        const U sub = channel(address) * subpartitions_per_channel + (bank & 1);
        const U idx = ((part >> 7) ^ (part >> 17)) & 1023;
        return {channel(address), bank, (rest >> 15) & 0x1fff,
                (rest & 0x7f) | (((rest >> 8) & 15) << 7), rest & 31,
                sub, rest, part, idx, sub * sets_per_subpartition + idx};
    }
};

} // namespace GTSim
