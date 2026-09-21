#pragma once
// Functional projection of the native cache policy. Requests complete before
// the next source request; this is deliberately not a GPU timing simulation.
#include "native_trace.h"
#include "work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h"
#include "work/tilegen-full-r1/core-native-copy-r2/include/dirty_sector_eval.h"
#include "cache_geometry.h"
#include <array>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace direct_native {
using U = std::uint64_t;
using J = nlohmann::json;
namespace g = GTSim;
inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Context {
    U call_index=native_trace::unknown, cta=native_trace::unknown;
    U warp=native_trace::unknown, pc=native_trace::unknown;
    U node_id=native_trace::unknown, sm_id=native_trace::unknown;
};
struct CacheKey {
    int matrix;
    U line;
    bool operator==(const CacheKey&) const = default;
};
struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const {
        return std::hash<U>{}(key.line) ^ (std::hash<int>{}(key.matrix)<<1);
    }
};
class FunctionalCache {
    struct Entry {
        std::uint8_t dirty;
        std::list<CacheKey>::iterator position;
        // Used only by the optional Accel-Sim sector/lazy-write projection.
        std::uint8_t present=0, readable=0;
        std::array<std::uint32_t,4> known{};
    };
    g::PerSmL1Cache l1_;
    U max_lines_;
    g::L2GroupedLru<CacheKey> lru_;
    std::unordered_map<CacheKey,Entry,CacheKeyHash> l2_;
    std::function<U(int,U)> map_;
    std::function<void(const native_trace::Record&)> emit_;
    U requests_=0, fills_=0, writebacks_=0, hits_=0, clean_evictions_=0;
    U dram_read_bytes_=0, dram_write_bytes_=0;
    U sector_read_misses_=0, sector_merge_reads_=0, lazy_store_allocations_=0;
    bool ada_sector_mode_=false;
    U read_source_bytes_=0, write_source_bytes_=0, source_instructions_=0;
    U source_ranges_=0, source_hash_=14695981039346656037ULL;
    U resident_dirty_lines_=0,resident_dirty_sectors_=0;
    g::DirtySectorEvalStatistics dirty_{};
    void hash(U value) {
        for (unsigned i=0;i<8;++i) {source_hash_^=(value>>(8*i))&255;source_hash_*=1099511628211ULL;}
    }
    void emit(const CacheKey& key,U offset,U bytes,bool write,const Context& context) {
        native_trace::Record r;
        r.request_id=requests_;r.source_sequence=requests_;
        r.source_matrix_id=U(key.matrix);r.source_line_address=key.line;
        r.service_address=map_(key.matrix,key.line)+offset;r.bytes=bytes;
        r.node_id=context.node_id;r.sm_id=context.sm_id;
        // Direct mode never schedules a GPU subpartition. Original warp/CTA
        // identity is retained separately without inventing L2 attribution.
        r.l2_subpartition_id=ada_sector_mode_
            ? lru_.geometry().partition(key.line+offset) : native_trace::unknown;
        r.cause=write?native_trace::Cause::DirtyWriteback:native_trace::Cause::ReadFillOrRfo;
        r.call_index=context.call_index;r.cta=context.cta;r.warp=context.warp;r.pc=context.pc;
        // All four time fields keep the Writer's mandatory unknown value.
        emit_(r);++requests_;
        if(write){++writebacks_;dram_write_bytes_+=bytes;}
        else{++fills_;dram_read_bytes_+=bytes;}
    }
    void access_legacy(const CacheKey& key,bool write,std::uint8_t mask,bool bypass,const Context& context) {
        require(context.sm_id<l1_.config().num_sms,"direct source SM out of range");
        const g::PerSmL1Access a{int(context.sm_id),key.matrix,key.line,write,-1,bypass};
        const auto decision=l1_.access(a);
        if (!decision.forwarded_to_l2) return;
        auto found=l2_.find(key);
        if(found!=l2_.end()) {
            ++hits_;lru_.touch(key.line,found->second.position);
            if(write) {
                const auto created=g::sector_popcount(std::uint8_t(mask&~found->second.dirty));
                dirty_.dirty_sector_creations+=created;resident_dirty_sectors_+=created;
                if(!found->second.dirty&&mask)++resident_dirty_lines_;
                found->second.dirty|=mask;
            }
        } else {
            // Native L2 issues fill/RFO before selecting a victim on completion.
            emit(key,0,128,false,context);
            if(const auto* selected=lru_.victim(key.line)) {
                const auto victim=*selected;auto prior=l2_.find(victim);
                require(prior!=l2_.end(),"direct LRU victim missing");
                const auto bits=prior->second.dirty;
                if(bits) {
                    const unsigned sectors=g::sector_popcount(bits);
                    --resident_dirty_lines_;resident_dirty_sectors_-=sectors;
                    dirty_.evicted_dirty_sectors+=sectors;
                    ++dirty_.eviction_masks.at(bits);++dirty_.eviction_popcounts.at(sectors);
                    for(unsigned s=0;s<4;++s)if(bits&(1U<<s)) {
                        emit(victim,s*32,32,true,context);++dirty_.writeback_run_lengths.at(1);
                    }
                    ++dirty_.eviction_run_counts.at(sectors);
                } else ++clean_evictions_;
                lru_.erase(victim.line,prior->second.position);l2_.erase(prior);
            }
            auto position=lru_.insert_mru(key.line,key);l2_.emplace(key,Entry{mask,position});
            dirty_.dirty_sector_creations+=g::sector_popcount(mask);
            if(mask){++resident_dirty_lines_;resident_dirty_sectors_+=g::sector_popcount(mask);}
        }
        if(decision.read_ticket.valid)
            require(l1_.complete_read(decision.read_ticket),"direct immediate L1 read completion failed");
    }
    // Functional projection of pinned gpu-cache.cc: lazy stores allocate and
    // mark byte coverage without fetching. Only a later read of an unreadable
    // sector fetches its complete 32 B atom. Timing/MSHR pressure is not modeled.
    void access_ada(const CacheKey& key,bool write,std::uint8_t requested,
                    const std::array<std::uint32_t,4>& known,bool bypass,
                    const Context& context) {
        require(context.sm_id<l1_.config().num_sms,"direct source SM out of range");
        require(requested!=0 && (requested&0xf)==requested,"direct Ada sector mask invalid");
        g::PerSmL1Access a{int(context.sm_id),key.matrix,key.line,write,-1,bypass};
        a.sector_mask=requested;a.known_byte_masks=known;
        const auto decision=l1_.access(a);
        if (!decision.forwarded_to_l2) return;
        // BYPASS has no cache filtering; a legacy-disabled L1 may report mask0.
        const auto forwarded=decision.outcome==g::PerSmL1Outcome::BYPASS
            ? requested : decision.forwarded_sector_mask;
        require(forwarded!=0 && (forwarded&requested)==forwarded,
                "direct Ada L1 forwarding mask invalid");
        for(unsigned sector=0;sector<4;++sector) {
            const auto bit=std::uint8_t(1U<<sector);
            if(!(forwarded&bit)) continue;
            auto found=l2_.find(key);
            const bool new_line=found==l2_.end();
            if(new_line) {
                // ON_MISS allocates the tag; pinned send_read_request queues
                // the read before rd_miss_base queues the selected dirty victim.
                if(!write){++sector_read_misses_;emit(key,sector*32,32,false,context);}
                if(const auto* selected=lru_.victim(key.line)) {
                    const auto victim=*selected;auto prior=l2_.find(victim);
                    require(prior!=l2_.end(),"direct Ada LRU victim missing");
                    const auto bits=prior->second.dirty;
                    if(bits) {
                        const unsigned sectors=g::sector_popcount(bits);
                        --resident_dirty_lines_;resident_dirty_sectors_-=sectors;
                        dirty_.evicted_dirty_sectors+=sectors;
                        ++dirty_.eviction_masks.at(bits);++dirty_.eviction_popcounts.at(sectors);
                        for(unsigned s=0;s<4;++s)if(bits&(1U<<s)) {
                            emit(victim,s*32,32,true,context);++dirty_.writeback_run_lengths.at(1);
                        }
                        ++dirty_.eviction_run_counts.at(sectors);
                    } else ++clean_evictions_;
                    lru_.erase(victim.line,prior->second.position);l2_.erase(prior);
                }
                auto position=lru_.insert_mru(key.line,key);
                found=l2_.emplace(key,Entry{0,position}).first;
            }
            auto& entry=found->second;
            lru_.touch(key.line,entry.position);
            const bool present=(entry.present&bit)!=0;
            if(write) {
                require(known[sector]!=0,"direct Ada store has no known byte coverage");
                if(present)++hits_;else ++lazy_store_allocations_;
                if(!(entry.dirty&bit)) {
                    ++dirty_.dirty_sector_creations;++resident_dirty_sectors_;
                    if(!entry.dirty)++resident_dirty_lines_;
                }
                entry.present|=bit;entry.dirty|=bit;
                entry.known[sector]|=known[sector];
                if(entry.known[sector]==UINT32_MAX)entry.readable|=bit;
            } else if(entry.readable&bit) {
                ++hits_;
            } else {
                if(!new_line) {
                    ++sector_read_misses_;
                    if(present && (entry.dirty&bit))++sector_merge_reads_;
                    emit(key,sector*32,32,false,context);
                }
                entry.present|=bit;entry.readable|=bit;entry.known[sector]=UINT32_MAX;
                // A merge read preserves dirty state and never duplicates a store.
            }
        }
        if(decision.read_ticket.valid)
            require(l1_.complete_read(decision.read_ticket),"direct Ada immediate L1 read completion failed");
    }
public:
    FunctionalCache(const g::PerSmL1Config& l1,U l2_bytes,
                    std::function<U(int,U)> mapper,
                    std::function<void(const native_trace::Record&)> output,
                    const g::L2GeometryConfig& geometry = g::L2GeometryConfig())
        :l1_(l1),max_lines_(l2_bytes/128),lru_(geometry,l2_bytes,128),
         map_(std::move(mapper)),emit_(std::move(output)) {
        ada_sector_mode_=geometry.mode==g::L2GeometryMode::ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE;
        require(!ada_sector_mode_ || l1.mode==g::PerSmL1Mode::BYPASS ||
                    (l1.sector32 && l1.write_allocate),
                "direct Ada modeled L1 requires sector32 and lazy write allocation");
        require(l2_bytes>0&&l2_bytes%128==0,"direct L2 requires positive128B capacity");
        require(l1.line_bytes==128,"direct L1 requires native128B lines");
        l2_.reserve(std::size_t(max_lines_));
    }
    void reconfigure_l1(U capacity_bytes,std::uint32_t ways) {
        l1_.configure_capacity_bytes_per_sm(capacity_bytes,ways);
    }
    void begin_kernel() {l1_.begin_kernel();}
    void instruction(int matrix,bool write,bool bypass,
                     const std::vector<g::ExplicitMemorySubop>& subops,const Context& context) {
        require(matrix>=0&&!subops.empty(),"direct requires original explicit memory subops");
        ++source_instructions_;
        hash(context.call_index);hash(context.cta);hash(context.node_id);hash(write);hash(bypass);
        hash(subops.size());
        for(std::size_t si=0;si<subops.size();++si) {
            const auto& sub=subops[si];U bytes=0;std::vector<U> lines;
            hash(sub.requested_bytes);hash(sub.ranges.size());
            for(const auto& range:sub.ranges) {
                require(range.byte_count>0&&range.offset_bytes<=UINT64_MAX-(range.byte_count-1),
                        "direct empty or overflowing explicit range");
                require(bytes<=UINT64_MAX-range.byte_count,"direct source byte overflow");
                bytes+=range.byte_count;++source_ranges_;
                hash(U(range.source_member_ordinal));hash(range.offset_bytes);hash(range.byte_count);
                const U last=(range.offset_bytes+range.byte_count-1)/128*128;
                for(U line=range.offset_bytes/128*128;;line+=128) {
                    // Same first occurrence order, independently for each
                    // original native subop. Never coalesce distinct subops.
                    if(std::find(lines.begin(),lines.end(),line)==lines.end())lines.push_back(line);
                    if(line==last)break;
                }
            }
            require(bytes==sub.requested_bytes,"direct explicit requested-byte multiplicity differs");
            (write?write_source_bytes_:read_source_bytes_)+=bytes;
            for(U line:lines) {
                const auto mask=write?g::explicit_store_sector_mask_program(matrix,128,subops,matrix,line,int(si),dirty_):0;
                if(!ada_sector_mode_) {
                    access_legacy({matrix,line},write,std::uint8_t(mask),bypass,context);
                } else {
                    std::array<std::uint32_t,4> coverage{};
                    std::uint8_t sectors=0;
                    for(const auto& range:sub.ranges) {
                        const U first=std::max(line,range.offset_bytes);
                        const U last=std::min(line+127,range.offset_bytes+range.byte_count-1);
                        if(first>last)continue;
                        for(unsigned sector=unsigned((first-line)/32);
                            sector<=unsigned((last-line)/32);++sector) {
                            const U sector_start=line+sector*32;
                            const unsigned offset=unsigned(std::max(first,sector_start)-sector_start);
                            const unsigned width=unsigned(std::min(last,sector_start+31)-
                                                          std::max(first,sector_start)+1);
                            coverage[sector]|=std::uint32_t(((U{1}<<width)-1)<<offset);
                            sectors|=std::uint8_t(1U<<sector);
                        }
                    }
                    access_ada({matrix,line},write,sectors,coverage,bypass,context);
                }
            }
        }
    }
    void verify_resident_ledger() const {
        U resident_lines=0,resident_sectors=0;
        for(const auto& [key,entry]:l2_) {
            if(entry.dirty){++resident_lines;resident_sectors+=g::sector_popcount(entry.dirty);}
            if(ada_sector_mode_) {
                require((entry.dirty&~entry.present)==0 && (entry.readable&~entry.present)==0,
                        "direct Ada sector state must have an allocated sector");
                for(unsigned s=0;s<4;++s)
                    if(entry.dirty&(1U<<s))require(entry.known[s]!=0,"dirty sector lost byte coverage");
            }
        }
        require(resident_lines==resident_dirty_lines_&&resident_sectors==resident_dirty_sectors_,
                "direct independent resident dirty gauge audit failed");
        require(lru_.size()==l2_.size()&&l2_.size()<=max_lines_,"direct group/tag capacity ledger failed");
    }
    J snapshot() const {
        require(dirty_.dirty_sector_creations==resident_dirty_sectors_+dirty_.evicted_dirty_sectors,
                "direct dirty sector conservation failed");
        require(writebacks_==dirty_.evicted_dirty_sectors&&l1_.live_read_tickets()==0,
                "direct writeback/read-ticket conservation failed");
        const auto l1=l1_.statistics();
        return {{"source_read_bytes",read_source_bytes_},{"source_write_bytes",write_source_bytes_},
            {"source_memory_instructions",source_instructions_},{"source_ranges",source_ranges_},
            {"source_projection_fnv1a64",source_hash_},{"DRAM_read_bytes",dram_read_bytes_},
            {"DRAM_write_bytes",dram_write_bytes_},{"DRAM_read_requests",fills_},
            {"DRAM_write_requests",writebacks_},{"trace_records",requests_},
            {"L2_hits",hits_},{"L2_clean_evictions",clean_evictions_},
            {"L2_resident_lines",l2_.size()},{"L2_capacity_lines",max_lines_},
            {"L1_pre_reads",l1.pre_l1_reads},{"L1_pre_writes",l1.pre_l1_writes},
            {"L1_read_hits",l1.read_hits},{"L1_read_misses",l1.read_misses},
            {"L1_write_hits",l1.write_hits},{"L1_write_misses",l1.write_misses},
            {"L1_bypass",l1.bypassed_transactions},{"L1_evictions",l1.evictions},
            {"dirty_sector_creations",dirty_.dirty_sector_creations},
            {"evicted_dirty_sectors",dirty_.evicted_dirty_sectors},
            {"resident_dirty_lines",resident_dirty_lines_},{"resident_dirty_sectors",resident_dirty_sectors_},
            {"dirty_sector_ledger_closed",true},{"writeback_byte_ledger_closed",true},
            {"store_mask_popcounts",dirty_.store_mask_popcounts},
            {"eviction_masks",dirty_.eviction_masks},{"eviction_popcounts",dirty_.eviction_popcounts},
            {"writeback_run_lengths",dirty_.writeback_run_lengths},
            {"eviction_run_counts",dirty_.eviction_run_counts},
            {"accelsim_sector_lazy_mode",ada_sector_mode_},
            {"L2_sector_read_misses",sector_read_misses_},
            {"L2_sector_merge_reads",sector_merge_reads_},
            {"L2_lazy_store_sector_allocations",lazy_store_allocations_},
            {"L1_unmodeled_reservation_stalls",l1.unmodeled_reservation_stalls},
            {"L1_capacity_reconfigurations",l1.capacity_reconfigurations}};
    }
};
} // namespace direct_native
