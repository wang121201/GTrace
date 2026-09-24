#pragma once
// Functional projection of the native cache policy. Requests complete before
// the next source request; this is deliberately not a GPU timing simulation.
#include "native_trace.h"
#include "llm_l1_adapter.h"
#include "work/tilegen-full-r1/core-native-copy-r2/include/dirty_sector_eval.h"
#include "frozen_cache_geometry.h"
#include "ef_hit_throttle.h"
#include <functional>
#include <algorithm>
#include <tuple>
#include <bit>
#include <array>
#include <map>
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
    bool low_priority=false; int role_id=0; bool atomic_rmw=false;
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
enum class DataPolicy { OLD128, SECTOR32 };
enum class DirtyAgeClock { GLOBAL_FORWARDED_LINE, SET_FORWARDED_LINE };
inline const char* dirty_age_clock_name(DirtyAgeClock clock){return clock==DirtyAgeClock::GLOBAL_FORWARDED_LINE?"global":"set";}
inline const char* dirty_age_budget_unit(DirtyAgeClock clock){return clock==DirtyAgeClock::GLOBAL_FORWARDED_LINE?"GLOBAL_FORWARDED_L2_128B_LINE_ACCESSES":"SAME_GROUP_FORWARDED_L2_128B_LINE_ACCESSES";}
enum class ReadReason : unsigned { LOAD_FILL, READ_MERGE, ATOMIC_READ, CAPACITY_MERGE, AGE_MERGE, DRAIN_MERGE, OLD_STORE_RFO, OLD_ATOMIC_RFO, COUNT };
inline const char* read_reason_name(ReadReason r){static constexpr const char* names[]={"load_fill","read_merge","atomic_read","capacity_merge","age_merge","drain_merge","old_store_RFO","old_atomic_RFO"};return names[unsigned(r)];}
enum class WritebackReason : unsigned { CAPACITY, AGE, DRAIN };
inline const char* writeback_reason_name(WritebackReason r){return r==WritebackReason::CAPACITY?"capacity":r==WritebackReason::AGE?"age":"drain";}
struct DiagnosticOptions { bool skip_store_rfo=false; bool bypass_streaming_reads=false; bool ef_fill_only=false; int ef_hit_rate=-1; U dirty_age_accesses=0; DataPolicy data_policy=DataPolicy::OLD128; DirtyAgeClock dirty_age_clock=DirtyAgeClock::GLOBAL_FORWARDED_LINE; };
inline bool dirty_age_due(U now,U last,U budget){require(last<=now,"dirty age sequence regressed");return budget&&now-last>=budget;}
struct DirtyOwner { U first=native_trace::unknown,last=native_trace::unknown; int first_role=0,last_role=0; };
using OwnerObserver=std::function<void(U,const DirtyOwner&,const Context&,bool)>;
using WritebackObserver=std::function<void(const DirtyOwner&,const Context&,WritebackReason,std::uint32_t)>;
class FunctionalCache {
    struct Entry { std::uint8_t dirty; std::list<CacheKey>::iterator position; std::array<DirtyOwner,4> owners{}; U last_write_sequence=0,last_write_age_tick=0; bool age_cleaned=false; std::list<CacheKey>::iterator age_position{}; std::array<std::uint32_t,4> known{},dirty_bytes{}; };
    DiagnosticOptions options_; OwnerObserver observe_; WritebackObserver observe_writeback_;
    EfHitThrottle ef_hit_throttle_;
    U skipped_store_rfo_=0,streaming_bypass_=0,ef_read_hits_=0;
    static void mark_owner(Entry& e,std::uint8_t mask,const Context& c){
        for(unsigned s=0;s<4;++s)if(mask&(1U<<s)){
            auto& w=e.owners[s]; if(!(e.dirty&(1U<<s))){w.first=c.call_index;w.first_role=c.role_id;}
            w.last=c.call_index;w.last_role=c.role_id;
        }
    }
    llm_l1::Adapter l1_;
    U max_lines_;
    g::L2GroupedLru<CacheKey> lru_;
    std::unordered_map<CacheKey,Entry,CacheKeyHash> l2_;
    std::function<U(int,U)> map_;
    std::function<void(const native_trace::Record&)> emit_;
    U requests_=0, fills_=0, writebacks_=0, hits_=0, clean_evictions_=0;
    U read_bytes_=0,sector_read_hits_=0,sector_read_misses_=0,drain_sectors_=0,drains_=0;
    U masked_writeback_requests_=0,writeback_enabled_byte_coverage_=0,writeback_mask_hash_=14695981039346656037ULL;
    std::array<U,unsigned(ReadReason::COUNT)> read_reason_requests_{},read_reason_bytes_{};
    U read_source_bytes_=0, write_source_bytes_=0, source_instructions_=0;
    U source_ranges_=0, source_hash_=14695981039346656037ULL;
    U resident_dirty_lines_=0,resident_dirty_sectors_=0;
    g::DirtySectorEvalStatistics dirty_{};
    // Passive observation only: never influences victim, insertion or touch.
    std::vector<U> dirty_lines_by_group_;
    U peak_dirty_lines_per_group_=0;
    // Candidate clock is forwarded L2 line accesses, never time or phase count.
    U l2_access_sequence_=0,age_triggers_=0,age_lines_=0,age_sectors_=0,age_rewrites_=0,age_redirty_=0,age_queue_peak_=0;
    std::list<CacheKey> age_queue_;
    std::vector<std::list<CacheKey>> set_age_queues_;
    std::vector<U> set_age_ticks_,set_age_writeback_bytes_;
    U age_queue_entries_=0;
    bool set_age()const{return options_.dirty_age_clock==DirtyAgeClock::SET_FORWARDED_LINE;}
    U age_group(const CacheKey& key)const{return set_age()?lru_.group(key.line):0;}
    U age_now(U group)const{return set_age()?set_age_ticks_.at(group):l2_access_sequence_;}
    std::list<CacheKey>& age_queue(U group){return set_age()?set_age_queues_.at(group):age_queue_;}
    const std::list<CacheKey>& age_queue(U group)const{return set_age()?set_age_queues_.at(group):age_queue_;}
    U age_tick(const CacheKey& key){
        require(l2_access_sequence_<UINT64_MAX,"L2 access sequence overflow");++l2_access_sequence_;
        const U group=age_group(key);
        if(set_age()){auto& tick=set_age_ticks_.at(group);require(tick<UINT64_MAX,"set age sequence overflow");++tick;}
        return group;
    }
    std::array<U,16> age_masks_{};
    std::array<U,65> capacity_age_sector_hist_{},age_written_sector_hist_{},selected_capacity_age_hist_{},selected_written_age_hist_{};
    static unsigned age_bucket(U age){unsigned bucket=0;while(age){++bucket;age>>=1;}return bucket;}
    static J age_histogram(const std::array<U,65>& counts){J rows=J::array();for(unsigned b=0;b<65;++b)if(counts[b])rows.push_back({{"min_L2_accesses",b?U{1}<<(b-1):0},{"max_L2_accesses",b==64?UINT64_MAX:b?(U{1}<<b)-1:0},{"dirty_sectors",counts[b]},{"bytes",counts[b]*32}});return rows;}
    void age_store(const CacheKey& key,Entry& entry,U group){
        if(entry.dirty)++age_rewrites_;else if(entry.age_cleaned){++age_redirty_;entry.age_cleaned=false;}
        entry.last_write_sequence=l2_access_sequence_;entry.last_write_age_tick=age_now(group);
        if(!options_.dirty_age_accesses)return;
        auto& queue=age_queue(group);
        if(entry.dirty)queue.splice(queue.end(),queue,entry.age_position);
        else{queue.push_back(key);entry.age_position=std::prev(queue.end());++age_queue_entries_;age_queue_peak_=std::max(age_queue_peak_,age_queue_entries_);}
    }
    void age_forget(const CacheKey& key,Entry& entry){
        if(options_.dirty_age_accesses){age_queue(age_group(key)).erase(entry.age_position);require(age_queue_entries_>0,"age queue gauge underflow");--age_queue_entries_;}
    }
    void age_enforce(const Context& context,bool trigger_write,U group){
        if(!options_.dirty_age_accesses)return;bool triggered=false;auto& queue=age_queue(group);
        while(!queue.empty()){
            const auto key=queue.front();auto found=l2_.find(key);require(found!=l2_.end()&&found->second.dirty,"age queue invalid");auto& entry=found->second;
            if(!dirty_age_due(age_now(group),entry.last_write_age_tick,options_.dirty_age_accesses))break;
            triggered=true;const auto bits=entry.dirty;const U sectors=g::sector_popcount(bits);
            --resident_dirty_lines_;dirty_group_remove(key.line);resident_dirty_sectors_-=sectors;age_forget(key,entry);
            ++age_lines_;age_sectors_+=sectors;++age_masks_.at(bits);age_written_sector_hist_.at(age_bucket(l2_access_sequence_-entry.last_write_sequence))+=sectors;
            selected_written_age_hist_.at(age_bucket(age_now(group)-entry.last_write_age_tick))+=sectors;
            if(set_age())set_age_writeback_bytes_.at(group)+=sectors*32;
            for(unsigned sector=0;sector<4;++sector)if(bits&(1U<<sector)){
                writeback_sector(key,entry,sector,context,WritebackReason::AGE,trigger_write);
            }
            entry.dirty=0;entry.dirty_bytes={};entry.owners={};entry.age_cleaned=true;
        }
        age_triggers_+=triggered;
    }
    void dirty_group_add(U line){auto& n=dirty_lines_by_group_.at(lru_.group(line));++n;peak_dirty_lines_per_group_=std::max(peak_dirty_lines_per_group_,n);}
    void dirty_group_remove(U line){auto& n=dirty_lines_by_group_.at(lru_.group(line));require(n>0,"dirty group gauge underflow");--n;}
    void hash(U value) {
        for (unsigned i=0;i<8;++i) {source_hash_^=(value>>(8*i))&255;source_hash_*=1099511628211ULL;}
    }
    void emit(const CacheKey& key,U offset,U bytes,bool write,const Context& context,ReadReason reason=ReadReason::LOAD_FILL) {
        native_trace::Record r;
        r.request_id=requests_;r.source_sequence=requests_;
        r.source_matrix_id=U(key.matrix);r.source_line_address=key.line;
        r.service_address=map_(key.matrix,key.line)+offset;r.bytes=bytes;
        r.node_id=context.node_id;r.sm_id=context.sm_id;
        // Direct mode never schedules a GPU subpartition. Original warp/CTA
        // identity is retained separately without inventing L2 attribution.
        r.l2_subpartition_id=native_trace::unknown;
        r.cause=write?native_trace::Cause::DirtyWriteback:native_trace::Cause::ReadFillOrRfo;
        r.call_index=context.call_index;r.cta=context.cta;r.warp=context.warp;r.pc=context.pc;
        // All four time fields keep the Writer's mandatory unknown value.
        emit_(r);++requests_;if(write)++writebacks_;else {++fills_;read_bytes_+=bytes;++read_reason_requests_.at(unsigned(reason));read_reason_bytes_.at(unsigned(reason))+=bytes;}
    }
    void writeback_sector(const CacheKey& key,Entry& entry,unsigned sector,const Context& context,WritebackReason reason,bool trigger_write){
        // Pinned Accel-Sim writeback carries the dirty byte mask. Unwritten
        // bytes are preserved at memory; no invented read-before-write is needed.
        // A 32B transaction is counted even when only a few bytes are enabled.
        const std::uint32_t mask=options_.data_policy==DataPolicy::SECTOR32?entry.dirty_bytes[sector]:UINT32_MAX;
        require(mask!=0,"dirty sector lost byte mask");
        writeback_enabled_byte_coverage_+=std::popcount(mask);if(mask!=UINT32_MAX)++masked_writeback_requests_;
        for(U v:{U(key.matrix),key.line+sector*32,U(mask),U(reason),context.call_index})for(unsigned i=0;i<8;++i){writeback_mask_hash_^=(v>>(i*8))&255;writeback_mask_hash_*=1099511628211ULL;}
        if(observe_)observe_(key.line+sector*32,entry.owners[sector],context,trigger_write);
        if(observe_writeback_)observe_writeback_(entry.owners[sector],context,reason,mask);
        emit(key,sector*32,32,true,context);
    }
    void evict_for_sector(const CacheKey& key,const Context& context,bool write){
        const auto* selected=lru_.victim(key.line);if(!selected)return;
        const auto victim=*selected;auto prior=l2_.find(victim);require(prior!=l2_.end(),"sector victim missing");auto& entry=prior->second;
        const auto bits=entry.dirty;
        if(bits){
            const unsigned sectors=g::sector_popcount(bits);
            capacity_age_sector_hist_.at(age_bucket(l2_access_sequence_-entry.last_write_sequence))+=sectors;selected_capacity_age_hist_.at(age_bucket(age_now(age_group(victim))-entry.last_write_age_tick))+=sectors;age_forget(victim,entry);
            --resident_dirty_lines_;dirty_group_remove(victim.line);resident_dirty_sectors_-=sectors;
            dirty_.evicted_dirty_sectors+=sectors;++dirty_.eviction_masks.at(bits);++dirty_.eviction_popcounts.at(sectors);
            for(unsigned sector=0;sector<4;++sector)if(bits&(1U<<sector)){writeback_sector(victim,entry,sector,context,WritebackReason::CAPACITY,write);++dirty_.writeback_run_lengths.at(1);}
            ++dirty_.eviction_run_counts.at(sectors);
        }else ++clean_evictions_;
        lru_.erase(victim.line,entry.position);l2_.erase(prior);
    }
    void access_sector(const CacheKey& key,bool write,std::uint8_t requested,const std::array<std::uint32_t,4>& coverage,bool bypass,const Context& context){
        require(context.sm_id<l1_.config().num_sms,"sector source SM out of range");
        g::PerSmL1Access a{int(context.sm_id),key.matrix,key.line,write,-1,bypass||context.atomic_rmw,requested};a.known_byte_masks=coverage;
        const auto decision=l1_.access(a);if(!decision.forwarded_to_l2)return;
        const auto forwarded=decision.forwarded_sector_mask;
        require(forwarded && (forwarded&requested)==forwarded,"L1 forwarded sector mask invalid");
        const U group=age_tick(key);
        auto found=l2_.find(key);const bool new_tag=found==l2_.end();
        // Complete one forwarded line request synchronously, preserving source line order.
        // An absent tag read is issued before capacity writeback, as in old128.
        std::array<std::uint32_t,4> fresh_known{};
        if(new_tag && (!write||context.atomic_rmw))for(unsigned sector=0;sector<4;++sector)if(forwarded&(1U<<sector)){
            emit(key,sector*32,32,false,context,context.atomic_rmw?ReadReason::ATOMIC_READ:ReadReason::LOAD_FILL);fresh_known[sector]=UINT32_MAX;++sector_read_misses_;
        }
        if(new_tag){
            evict_for_sector(key,context,write);auto position=lru_.insert_mru(key.line,key);
            if(context.low_priority&&!write)lru_.touch_lru(key.line,position);
            Entry entry{0,position,{}};entry.known=fresh_known;found=l2_.emplace(key,std::move(entry)).first;
        }else{
            ++hits_;bool promote=options_.ef_fill_only;
            if(context.low_priority&&!write&&options_.ef_hit_rate>=0)promote=ef_hit_throttle_.choose();
            if(context.low_priority&&!write&&promote)++ef_read_hits_;
            if(context.low_priority&&!write&&!promote)lru_.touch_lru(key.line,found->second.position);else lru_.touch(key.line,found->second.position);
        }
        auto& entry=found->second;
        if(!new_tag && (!write||context.atomic_rmw))for(unsigned sector=0;sector<4;++sector)if(forwarded&(1U<<sector)){
            if(entry.known[sector]==UINT32_MAX){++sector_read_hits_;continue;}
            ++sector_read_misses_;const auto reason=context.atomic_rmw?ReadReason::ATOMIC_READ:entry.known[sector]?ReadReason::READ_MERGE:ReadReason::LOAD_FILL;
            emit(key,sector*32,32,false,context,reason);entry.known[sector]=UINT32_MAX;
        }
        if(write){
            age_store(key,entry,group);const auto created=g::sector_popcount(std::uint8_t(forwarded&~entry.dirty));
            dirty_.dirty_sector_creations+=created;resident_dirty_sectors_+=created;
            if(!entry.dirty){++resident_dirty_lines_;dirty_group_add(key.line);}
            mark_owner(entry,forwarded,context);entry.dirty|=forwarded;
            for(unsigned sector=0;sector<4;++sector)if(forwarded&(1U<<sector)){require(coverage[sector],"store lacks byte coverage");entry.known[sector]|=coverage[sector];entry.dirty_bytes[sector]|=coverage[sector];}
        }
        age_enforce(context,write,group);
        if(decision.read_ticket.valid)require(l1_.complete_read(decision.read_ticket),"sector immediate L1 completion failed");
    }
    void access(const CacheKey& key,bool write,std::uint8_t mask,std::uint8_t requested_mask,bool bypass,const Context& context) {
        require(context.sm_id<l1_.config().num_sms,"direct source SM out of range");
        const g::PerSmL1Access a{int(context.sm_id),key.matrix,key.line,write,-1,bypass,requested_mask};
        const auto decision=l1_.access(a);
        if (!decision.forwarded_to_l2) return;
        const U group=age_tick(key);
        if(!write&&context.low_priority&&options_.bypass_streaming_reads){
            emit(key,0,128,false,context);++streaming_bypass_;
            if(decision.read_ticket.valid)require(l1_.complete_read(decision.read_ticket),"bypass read completion");
            return;
        }
        auto found=l2_.find(key);
        if(found!=l2_.end()) {
            ++hits_;
            bool promote=options_.ef_fill_only;
            if(context.low_priority&&!write&&options_.ef_hit_rate>=0)promote=ef_hit_throttle_.choose();
            if(context.low_priority&&!write&&promote)++ef_read_hits_;
            if(context.low_priority&&!write&&!promote)lru_.touch_lru(key.line,found->second.position);
            else lru_.touch(key.line,found->second.position);
            if(write) {
                if(mask)age_store(key,found->second,group);
                const auto created=g::sector_popcount(std::uint8_t(mask&~found->second.dirty));
                dirty_.dirty_sector_creations+=created;resident_dirty_sectors_+=created;
                if(!found->second.dirty&&mask){++resident_dirty_lines_;dirty_group_add(key.line);}
                mark_owner(found->second,mask,context);found->second.dirty|=mask;
            }
        } else {
            // Native L2 issues fill/RFO before selecting a victim on completion.
            if(write&&options_.skip_store_rfo)++skipped_store_rfo_;
            else emit(key,0,128,false,context,write?(context.atomic_rmw?ReadReason::OLD_ATOMIC_RFO:ReadReason::OLD_STORE_RFO):ReadReason::LOAD_FILL);
            if(const auto* selected=lru_.victim(key.line)) {
                const auto victim=*selected;auto prior=l2_.find(victim);
                require(prior!=l2_.end(),"direct LRU victim missing");
                const auto bits=prior->second.dirty;
                if(bits) {
                    const unsigned sectors=g::sector_popcount(bits);
                    capacity_age_sector_hist_.at(age_bucket(l2_access_sequence_-prior->second.last_write_sequence))+=sectors;selected_capacity_age_hist_.at(age_bucket(age_now(age_group(victim))-prior->second.last_write_age_tick))+=sectors;age_forget(victim,prior->second);
                    --resident_dirty_lines_;dirty_group_remove(victim.line);resident_dirty_sectors_-=sectors;
                    dirty_.evicted_dirty_sectors+=sectors;
                    ++dirty_.eviction_masks.at(bits);++dirty_.eviction_popcounts.at(sectors);
                    for(unsigned s=0;s<4;++s)if(bits&(1U<<s)) {
                        writeback_sector(victim,prior->second,s,context,WritebackReason::CAPACITY,write);++dirty_.writeback_run_lengths.at(1);
                    }
                    ++dirty_.eviction_run_counts.at(sectors);
                } else ++clean_evictions_;
                lru_.erase(victim.line,prior->second.position);l2_.erase(prior);
            }
            auto position=lru_.insert_mru(key.line,key);
            if(context.low_priority&&!write)lru_.touch_lru(key.line,position);
            Entry entry{0,position,{}};mark_owner(entry,mask,context);
            auto added=l2_.emplace(key,std::move(entry));if(mask)age_store(key,added.first->second,group);added.first->second.dirty=mask;
            dirty_.dirty_sector_creations+=g::sector_popcount(mask);
            if(mask){++resident_dirty_lines_;dirty_group_add(key.line);resident_dirty_sectors_+=g::sector_popcount(mask);}
        }
        age_enforce(context,write,group);
        if(decision.read_ticket.valid)
            require(l1_.complete_read(decision.read_ticket),"direct immediate L1 read completion failed");
    }
public:
    FunctionalCache(const g::PerSmL1Config& l1,U l2_bytes,
                    std::function<U(int,U)> mapper,
                    std::function<void(const native_trace::Record&)> output,
                    const g::L2GeometryConfig& geometry = g::L2GeometryConfig(), DiagnosticOptions options={}, OwnerObserver observer={}, WritebackObserver writeback_observer={})
        :options_(options),observe_(std::move(observer)),observe_writeback_(std::move(writeback_observer)),ef_hit_throttle_(options.ef_hit_rate<0?0:std::uint32_t(options.ef_hit_rate)),l1_(l1),max_lines_(l2_bytes/128),lru_(geometry,l2_bytes,128),
         map_(std::move(mapper)),emit_(std::move(output)) {
        require(l2_bytes>0&&l2_bytes%128==0,"direct L2 requires positive128B capacity");
        require(l1.line_bytes==128,"direct L1 requires native128B lines");
        require(!options_.ef_fill_only||(l1.sector32&&!options_.skip_store_rfo&&!options_.bypass_streaming_reads),"EF fill-only diagnostic requires sector L1 and unchanged RFO/admission");
        require(options_.ef_hit_rate>=-1&&options_.ef_hit_rate<=65536,"EF hit rate out of range");
        require(options_.ef_hit_rate<0||(l1.sector32&&!options_.ef_fill_only&&!options_.skip_store_rfo&&!options_.bypass_streaming_reads),"EF hit calibration requires sector L1 and no simultaneous cache override");
        require(!options_.dirty_age_accesses||(!options_.skip_store_rfo&&!options_.bypass_streaming_reads&&!options_.ef_fill_only&&options_.ef_hit_rate>=0),"dirty age requires unchanged frozen admission/RFO and explicit EF rate");
        require(options_.data_policy==DataPolicy::OLD128||(!options_.skip_store_rfo&&!options_.bypass_streaming_reads&&!options_.ef_fill_only),"sector mode rejects legacy admission overrides");
        require(options_.dirty_age_clock==DirtyAgeClock::GLOBAL_FORWARDED_LINE||options_.dirty_age_clock==DirtyAgeClock::SET_FORWARDED_LINE,"unknown dirty age clock");
        l2_.reserve(std::size_t(max_lines_));dirty_lines_by_group_.assign(lru_.geometry().group_count(),0);
        if(set_age()){const auto count=lru_.geometry().group_count();set_age_queues_.resize(count);set_age_ticks_.assign(count,0);set_age_writeback_bytes_.assign(count,0);}
    }
    J ef_fill_only_observation()const{return {{"enabled",options_.ef_fill_only||options_.ef_hit_rate>0},{"resident_EF_read_hits_promoted_to_MRU",ef_read_hits_},{"synchronous_completion",true},{"hardware_replacement_qualified",false}};}
    J ef_hit_throttle_observation()const{return {{"schema","LLM_EF_HIT_THROTTLE_CALIBRATION_V1"},{"enabled",options_.ef_hit_rate>=0},
        {"numerator",ef_hit_throttle_.numerator()},{"denominator",EfHitThrottle::denominator},
        {"algorithm","xorshift64star_v1_top16_threshold"},{"fixed_seed_hex","0x9e3779b97f4a7c15"},
        {"eligible_resident_EF_read_hits",ef_hit_throttle_.eligible()},{"promoted_to_MRU",ef_hit_throttle_.promoted()},
        {"empirical_calibration",true},{"hardware_equivalence_claimed",false}};}
    void begin_kernel(const J& metadata=J::object()) {l1_.begin_kernel(metadata);}
    void allocation_metadata(const J& node){l1_.allocation(node);}
    J l1_observation()const{return l1_.observation();}
    J l1_configuration()const{return l1_.configuration();}
    void instruction(int matrix,bool write,bool bypass,
                     const std::vector<g::ExplicitMemorySubop>& subops,const Context& context) {
        require(matrix>=0&&!subops.empty(),"direct requires original explicit memory subops");
        require(!context.atomic_rmw||write,"atomic RMW requires read-modify-write operation");
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
                std::uint8_t requested_mask=0;std::array<std::uint32_t,4> coverage{};
                for(const auto& range:sub.ranges){
                    const U begin=std::max(line,range.offset_bytes),end=std::min(line|U{127},range.offset_bytes+range.byte_count-1);
                    if(begin<=end)for(unsigned sector=unsigned((begin-line)/32);sector<=unsigned((end-line)/32);++sector){
                        requested_mask|=std::uint8_t(1U<<sector);
                        const U sb=line+sector*32,first=std::max(begin,sb),last=std::min(end,sb+31);const unsigned n=unsigned(last-first+1),shift=unsigned(first-sb);
                        coverage[sector]|=n==32?UINT32_MAX:std::uint32_t(((U{1}<<n)-1)<<shift);
                    }
                }
                require(requested_mask>0&&requested_mask<16,"source line has empty sector coverage");
                if(options_.data_policy==DataPolicy::SECTOR32)access_sector({matrix,line},write,requested_mask,coverage,bypass,context);
                else access({matrix,line},write,std::uint8_t(mask),requested_mask,bypass,context);
            }
        }
    }
    void drain(const Context& context){
        require(l1_.live_read_tickets()==0,"drain with pending read");++drains_;
        // Deterministic service ordering; no tag invalidation, LRU touch, or age tick.
        std::vector<CacheKey> keys;for(const auto& [key,entry]:l2_)if(entry.dirty)keys.push_back(key);
        std::sort(keys.begin(),keys.end(),[](const auto&a,const auto&b){return std::tie(a.matrix,a.line)<std::tie(b.matrix,b.line);});
        for(const auto& key:keys){auto& entry=l2_.at(key);const auto bits=entry.dirty;const U sectors=g::sector_popcount(bits);
            for(unsigned sector=0;sector<4;++sector)if(bits&(1U<<sector))writeback_sector(key,entry,sector,context,WritebackReason::DRAIN,false);
            drain_sectors_+=sectors;--resident_dirty_lines_;resident_dirty_sectors_-=sectors;dirty_group_remove(key.line);age_forget(key,entry);entry.dirty=0;entry.dirty_bytes={};entry.owners={};entry.age_cleaned=true;
        }
    }
    void verify_resident_ledger() const {
        U resident_lines=0,resident_sectors=0;std::vector<U> groups(dirty_lines_by_group_.size(),0);
        for(const auto& [key,entry]:l2_)if(entry.dirty){++resident_lines;++groups.at(lru_.group(key.line));resident_sectors+=g::sector_popcount(entry.dirty);}
        if(options_.data_policy==DataPolicy::SECTOR32)for(const auto& [key,entry]:l2_)for(unsigned sector=0;sector<4;++sector){
            require(bool(entry.dirty&(1U<<sector))==bool(entry.dirty_bytes[sector]),"dirty sector/byte mask differs");
            require((entry.dirty_bytes[sector]&~entry.known[sector])==0,"dirty bytes not known");
        }
        require(groups==dirty_lines_by_group_,"independent dirty group gauge audit failed");
        if(set_age()){
            U ticks=0;for(U tick:set_age_ticks_){require(tick<=UINT64_MAX-ticks,"set tick sum overflow");ticks+=tick;}
            require(ticks==l2_access_sequence_,"set ticks do not partition global forwarded sequence");
            U age_bytes=0;for(U bytes:set_age_writeback_bytes_)age_bytes+=bytes;require(age_bytes==age_sectors_*32,"group age writeback bytes do not sum to global age bytes");
        }
        if(options_.dirty_age_accesses){
            require(age_queue_entries_==resident_lines,"age queue gauge mismatch");std::unordered_map<CacheKey,bool,CacheKeyHash> seen;U entries=0;
            for(U group=0;group<(set_age()?set_age_queues_.size():1);++group){U previous=0;const auto& queue=age_queue(group);
                for(auto it=queue.begin();it!=queue.end();++it){auto found=l2_.find(*it);require(found!=l2_.end()&&found->second.dirty&&found->second.age_position==it&&seen.emplace(*it,true).second,"age queue identity mismatch");require(age_group(*it)==group,"dirty queue group mismatch");const auto last=found->second.last_write_age_tick;require(last>=previous&&last<=age_now(group),"age queue time order");previous=last;require(!dirty_age_due(age_now(group),last,options_.dirty_age_accesses),"expired dirty line remained");require(found->second.last_write_sequence<=l2_access_sequence_,"global store sequence regressed");++entries;}
            }
            require(entries==age_queue_entries_,"age queue scan count mismatch");
        }else {require(age_queue_.empty()&&age_queue_entries_==0,"disabled age queue must stay empty");for(const auto& queue:set_age_queues_)require(queue.empty(),"disabled set age queue must stay empty");}
        require(resident_lines==resident_dirty_lines_&&resident_sectors==resident_dirty_sectors_,
                "direct independent resident dirty gauge audit failed");
        require(lru_.size()==l2_.size()&&l2_.size()<=max_lines_,"direct group/tag capacity ledger failed");
    }
    J dirty_group_observation()const{
        verify_resident_ledger();std::map<U,U> histogram;U maximum=0;
        for(U n:dirty_lines_by_group_){++histogram[n];maximum=std::max(maximum,n);}
        J rows=J::array();for(auto [n,count]:histogram)rows.push_back({{"resident_dirty_lines",n},{"group_count",count}});
        return {{"passive_no_policy_change",options_.dirty_age_accesses==0},{"group_mapping","frozen L2GroupedLru.group(line)"},{"group_capacity",lru_.capacity_per_group()},{"resident_dirty_lines_by_group",dirty_lines_by_group_},{"resident_dirty_lines_histogram",rows},{"resident_max_dirty_lines_per_group",maximum},{"peak_dirty_lines_in_any_group",peak_dirty_lines_per_group_},{"independent_final_scan_verified",true}};
    }
    J dirty_age_observation()const{
        verify_resident_ledger();std::array<U,65> resident{},selected_resident{};
        for(const auto& [key,entry]:l2_)if(entry.dirty){const auto count=g::sector_popcount(entry.dirty);resident.at(age_bucket(l2_access_sequence_-entry.last_write_sequence))+=count;selected_resident.at(age_bucket(age_now(age_group(key))-entry.last_write_age_tick))+=count;}
        // The original histograms remain GLOBAL forwarded-line units in both modes.
        J result={{"clock","FORWARDED_L2_128B_LINE_ACCESSES_NOT_TIME"},{"age_budget_accesses",options_.dirty_age_accesses},{"off",options_.dirty_age_accesses==0},{"now",l2_access_sequence_},{"resident_dirty_age_histogram",age_histogram(resident)},{"capacity_evicted_dirty_age_histogram",age_histogram(capacity_age_sector_hist_)},{"age_written_dirty_age_histogram",age_histogram(age_written_sector_hist_)},{"dirty_queue_entries",age_queue_entries_},{"dirty_queue_peak_entries",age_queue_peak_},{"store_refresh_precedes_same_tick_expiry",true},{"independent_queue_scan_verified",true}};
        result["selected_clock"]=dirty_age_clock_name(options_.dirty_age_clock);result["age_budget_unit"]=dirty_age_budget_unit(options_.dirty_age_clock);result["global_histogram_unit"]="GLOBAL_FORWARDED_L2_128B_LINE_ACCESSES";
        auto selected_hist=[](const auto& counts){J rows=age_histogram(counts);for(auto& row:rows){row["min_selected_ticks"]=row["min_L2_accesses"];row["max_selected_ticks"]=row["max_L2_accesses"];row.erase("min_L2_accesses");row.erase("max_L2_accesses");}return rows;};
        result["selected_resident_dirty_age_histogram"]=selected_hist(selected_resident);result["selected_capacity_evicted_dirty_age_histogram"]=selected_hist(selected_capacity_age_hist_);result["selected_age_written_dirty_age_histogram"]=selected_hist(selected_written_age_hist_);
        result["group_count"]=lru_.geometry().group_count();result["group_mapping"]="frozen L2GroupedLru.group(original_byte_VA); matrix namespace excluded";
        result["whole_line_store_timer"]=true;result["implicit_end_flush"]=false;
        if(set_age()){
            std::map<U,U> counts;U sum=0;for(U tick:set_age_ticks_){++counts[tick];sum+=tick;}
            J histogram=J::array();for(auto [tick,count]:counts)histogram.push_back({{"selected_ticks",tick},{"groups",count}});
            result["group_ticks"]=set_age_ticks_;result["group_ticks_histogram"]=histogram;result["group_ticks_sum"]=sum;
            result["group_age_writeback_bytes"]=set_age_writeback_bytes_;result["group_resident_dirty_lines"]=dirty_lines_by_group_;
            result["set_clock_static_vector_payload_bytes"]=set_age_ticks_.size()*sizeof(U)+set_age_writeback_bytes_.size()*sizeof(U)+set_age_queues_.size()*sizeof(std::list<CacheKey>);
        }
        result["entry_selected_age_tick_bytes"]=sizeof(U);return result;
    }
    J dirty_owner_tail()const{
        std::map<std::array<U,4>,U> counts;
        for(const auto& [key,e]:l2_)for(unsigned s=0;s<4;++s)if(e.dirty&(1U<<s)){
            const auto& w=e.owners[s];counts[{w.first,U(w.first_role),w.last,U(w.last_role)}]+=32;
        }
        J rows=J::array();for(const auto& [k,n]:counts)rows.push_back({{"first_writer",k[0]},{"first_role",k[1]},{"last_writer",k[2]},{"last_role",k[3]},{"bytes",n}});
        return rows;
    }
    J snapshot() const {
        require(dirty_.dirty_sector_creations==resident_dirty_sectors_+dirty_.evicted_dirty_sectors+age_sectors_+drain_sectors_,
                "direct dirty sector conservation failed");
        require(writebacks_==dirty_.evicted_dirty_sectors+age_sectors_+drain_sectors_&&l1_.live_read_tickets()==0,
                "direct writeback/read-ticket conservation failed");
        const auto l1=l1_.statistics();
        J result={{"L2_forwarded_access_sequence",l2_access_sequence_},{"age_writeback_triggers",age_triggers_},{"age_writeback_lines",age_lines_},{"age_writeback_sectors",age_sectors_},{"age_writeback_bytes",age_sectors_*32},{"age_writeback_masks",age_masks_},{"age_rewrite_while_dirty_accesses",age_rewrites_},{"age_redirty_retained_lines",age_redirty_},{"capacity_eviction_writeback_bytes",dirty_.evicted_dirty_sectors*32},{"dirty_age_budget_accesses",options_.dirty_age_accesses},{"diagnostic_skipped_store_RFO_requests",skipped_store_rfo_},{"diagnostic_streaming_bypass_requests",streaming_bypass_},{"source_read_bytes",read_source_bytes_},{"source_write_bytes",write_source_bytes_},
            {"source_memory_instructions",source_instructions_},{"source_ranges",source_ranges_},
            {"source_projection_fnv1a64",source_hash_},{"DRAM_read_bytes",read_bytes_},
            {"DRAM_write_bytes",writebacks_*32},{"DRAM_read_requests",fills_},
            {"DRAM_write_requests",writebacks_},{"trace_records",requests_},
            {"L2_hits",hits_},{"L2_clean_evictions",clean_evictions_},
            {"L2_resident_lines",l2_.size()},{"L2_capacity_lines",max_lines_},
            {"L1_pre_reads",l1.pre_l1_reads},{"L1_pre_writes",l1.pre_l1_writes},
            {"L1_read_hits",l1.read_hits},{"L1_read_misses",l1.read_misses},
            {"L1_write_hits",l1.write_hits},{"L1_write_misses",l1.write_misses},
            {"L1_bypass",l1.bypassed_transactions},{"L1_evictions",l1.evictions},
            {"L1_bypassed_read_sector_requests",l1.bypassed_read_sector_requests},{"L1_bypassed_write_sector_requests",l1.bypassed_write_sector_requests},
            {"L1_read_sector_requests",l1.read_sector_requests},{"L1_read_sector_hits",l1.read_sector_hits},{"L1_read_sector_misses",l1.read_sector_misses},
            {"L1_write_sector_requests",l1.write_sector_requests},{"L1_write_sector_hits",l1.write_sector_hits},{"L1_write_sector_misses",l1.write_sector_misses},
            {"L1_forwarded_read_sector_requests",l1.forwarded_read_sector_requests},{"L1_forwarded_write_sector_requests",l1.forwarded_write_sector_requests},
            {"dirty_sector_creations",dirty_.dirty_sector_creations},
            {"evicted_dirty_sectors",dirty_.evicted_dirty_sectors},
            {"resident_dirty_lines",resident_dirty_lines_},{"resident_dirty_sectors",resident_dirty_sectors_},
            {"dirty_sector_ledger_closed",true},{"writeback_byte_ledger_closed",true},
            {"store_mask_popcounts",dirty_.store_mask_popcounts},
            {"eviction_masks",dirty_.eviction_masks},{"eviction_popcounts",dirty_.eviction_popcounts},
            {"writeback_run_lengths",dirty_.writeback_run_lengths},
            {"eviction_run_counts",dirty_.eviction_run_counts}};
        result["masked_writeback_requests"]=masked_writeback_requests_;result["writeback_enabled_byte_coverage"]=writeback_enabled_byte_coverage_;result["writeback_byte_mask_fnv1a64"]=writeback_mask_hash_;
        result["L2_sector_read_hits"]=sector_read_hits_;result["L2_sector_read_misses"]=sector_read_misses_;
        result["explicit_drain_count"]=drains_;result["drain_writeback_bytes"]=drain_sectors_*32;
        for(unsigned reason=0;reason<unsigned(ReadReason::COUNT);++reason){const std::string name=read_reason_name(ReadReason(reason));result["DRAM_"+name+"_requests"]=read_reason_requests_[reason];result["DRAM_"+name+"_bytes"]=read_reason_bytes_[reason];}
        // Retain old public aliases; new merge reads have distinct, explicit names.
        result["DRAM_store_RFO_requests"]=read_reason_requests_[unsigned(ReadReason::OLD_STORE_RFO)];result["DRAM_store_RFO_bytes"]=read_reason_bytes_[unsigned(ReadReason::OLD_STORE_RFO)];
        result["DRAM_atomic_RFO_requests"]=read_reason_requests_[unsigned(ReadReason::OLD_ATOMIC_RFO)];result["DRAM_atomic_RFO_bytes"]=read_reason_bytes_[unsigned(ReadReason::OLD_ATOMIC_RFO)];
        return result;
    }
};
} // namespace direct_native
