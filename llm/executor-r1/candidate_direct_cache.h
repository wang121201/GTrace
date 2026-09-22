#pragma once
// Functional projection of the native cache policy. Requests complete before
// the next source request; this is deliberately not a GPU timing simulation.
#include "native_trace.h"
#include "llm_l1_adapter.h"
#include "work/tilegen-full-r1/core-native-copy-r2/include/dirty_sector_eval.h"
#include "frozen_cache_geometry.h"
#include "ef_hit_throttle.h"
#include <functional>
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
    bool low_priority=false; int role_id=0;
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
struct DiagnosticOptions { bool skip_store_rfo=false; bool bypass_streaming_reads=false; bool ef_fill_only=false; int ef_hit_rate=-1; U dirty_age_accesses=0; };
inline bool dirty_age_due(U now,U last,U budget){require(last<=now,"dirty age sequence regressed");return budget&&now-last>=budget;}
struct DirtyOwner { U first=native_trace::unknown,last=native_trace::unknown; int first_role=0,last_role=0; };
using OwnerObserver=std::function<void(U,const DirtyOwner&,const Context&,bool)>;
class FunctionalCache {
    struct Entry { std::uint8_t dirty; std::list<CacheKey>::iterator position; std::array<DirtyOwner,4> owners{}; U last_write_sequence=0; bool age_cleaned=false; std::list<CacheKey>::iterator age_position{}; };
    DiagnosticOptions options_; OwnerObserver observe_;
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
    std::array<U,16> age_masks_{};
    std::array<U,65> capacity_age_sector_hist_{},age_written_sector_hist_{};
    static unsigned age_bucket(U age){unsigned bucket=0;while(age){++bucket;age>>=1;}return bucket;}
    static J age_histogram(const std::array<U,65>& counts){J rows=J::array();for(unsigned b=0;b<65;++b)if(counts[b])rows.push_back({{"min_L2_accesses",b?U{1}<<(b-1):0},{"max_L2_accesses",b==64?UINT64_MAX:b?(U{1}<<b)-1:0},{"dirty_sectors",counts[b]},{"bytes",counts[b]*32}});return rows;}
    void age_store(const CacheKey& key,Entry& entry){
        if(entry.dirty)++age_rewrites_;else if(entry.age_cleaned){++age_redirty_;entry.age_cleaned=false;}
        entry.last_write_sequence=l2_access_sequence_;
        if(!options_.dirty_age_accesses)return;
        if(entry.dirty)age_queue_.splice(age_queue_.end(),age_queue_,entry.age_position);
        else{age_queue_.push_back(key);entry.age_position=std::prev(age_queue_.end());age_queue_peak_=std::max<U>(age_queue_peak_,age_queue_.size());}
    }
    void age_forget(Entry& entry){if(options_.dirty_age_accesses)age_queue_.erase(entry.age_position);}
    void age_enforce(const Context& context,bool trigger_write){
        if(!options_.dirty_age_accesses)return;bool triggered=false;
        while(!age_queue_.empty()){
            const auto key=age_queue_.front();auto found=l2_.find(key);require(found!=l2_.end()&&found->second.dirty,"age queue invalid");auto& entry=found->second;
            if(!dirty_age_due(l2_access_sequence_,entry.last_write_sequence,options_.dirty_age_accesses))break;
            triggered=true;const auto bits=entry.dirty;const U sectors=g::sector_popcount(bits);
            --resident_dirty_lines_;dirty_group_remove(key.line);resident_dirty_sectors_-=sectors;age_forget(entry);
            ++age_lines_;age_sectors_+=sectors;++age_masks_.at(bits);age_written_sector_hist_.at(age_bucket(l2_access_sequence_-entry.last_write_sequence))+=sectors;
            for(unsigned sector=0;sector<4;++sector)if(bits&(1U<<sector)){
                if(observe_)observe_(key.line+sector*32,entry.owners[sector],context,trigger_write);
                emit(key,sector*32,32,true,context);
            }
            entry.dirty=0;entry.owners={};entry.age_cleaned=true;
        }
        age_triggers_+=triggered;
    }
    void dirty_group_add(U line){auto& n=dirty_lines_by_group_.at(lru_.group(line));++n;peak_dirty_lines_per_group_=std::max(peak_dirty_lines_per_group_,n);}
    void dirty_group_remove(U line){auto& n=dirty_lines_by_group_.at(lru_.group(line));require(n>0,"dirty group gauge underflow");--n;}
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
        r.l2_subpartition_id=native_trace::unknown;
        r.cause=write?native_trace::Cause::DirtyWriteback:native_trace::Cause::ReadFillOrRfo;
        r.call_index=context.call_index;r.cta=context.cta;r.warp=context.warp;r.pc=context.pc;
        // All four time fields keep the Writer's mandatory unknown value.
        emit_(r);++requests_;if(write)++writebacks_;else ++fills_;
    }
    void access(const CacheKey& key,bool write,std::uint8_t mask,std::uint8_t requested_mask,bool bypass,const Context& context) {
        require(context.sm_id<l1_.config().num_sms,"direct source SM out of range");
        const g::PerSmL1Access a{int(context.sm_id),key.matrix,key.line,write,-1,bypass,requested_mask};
        const auto decision=l1_.access(a);
        if (!decision.forwarded_to_l2) return;
        require(l2_access_sequence_<UINT64_MAX,"L2 access sequence overflow");++l2_access_sequence_;
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
                if(mask)age_store(key,found->second);
                const auto created=g::sector_popcount(std::uint8_t(mask&~found->second.dirty));
                dirty_.dirty_sector_creations+=created;resident_dirty_sectors_+=created;
                if(!found->second.dirty&&mask){++resident_dirty_lines_;dirty_group_add(key.line);}
                mark_owner(found->second,mask,context);found->second.dirty|=mask;
            }
        } else {
            // Native L2 issues fill/RFO before selecting a victim on completion.
            if(write&&options_.skip_store_rfo)++skipped_store_rfo_;
            else emit(key,0,128,false,context);
            if(const auto* selected=lru_.victim(key.line)) {
                const auto victim=*selected;auto prior=l2_.find(victim);
                require(prior!=l2_.end(),"direct LRU victim missing");
                const auto bits=prior->second.dirty;
                if(bits) {
                    const unsigned sectors=g::sector_popcount(bits);
                    capacity_age_sector_hist_.at(age_bucket(l2_access_sequence_-prior->second.last_write_sequence))+=sectors;age_forget(prior->second);
                    --resident_dirty_lines_;dirty_group_remove(victim.line);resident_dirty_sectors_-=sectors;
                    dirty_.evicted_dirty_sectors+=sectors;
                    ++dirty_.eviction_masks.at(bits);++dirty_.eviction_popcounts.at(sectors);
                    for(unsigned s=0;s<4;++s)if(bits&(1U<<s)) {
                        if(observe_)observe_(victim.line+s*32,prior->second.owners[s],context,write);
                        emit(victim,s*32,32,true,context);++dirty_.writeback_run_lengths.at(1);
                    }
                    ++dirty_.eviction_run_counts.at(sectors);
                } else ++clean_evictions_;
                lru_.erase(victim.line,prior->second.position);l2_.erase(prior);
            }
            auto position=lru_.insert_mru(key.line,key);
            if(context.low_priority&&!write)lru_.touch_lru(key.line,position);
            Entry entry{0,position,{}};mark_owner(entry,mask,context);
            auto added=l2_.emplace(key,std::move(entry));if(mask)age_store(key,added.first->second);added.first->second.dirty=mask;
            dirty_.dirty_sector_creations+=g::sector_popcount(mask);
            if(mask){++resident_dirty_lines_;dirty_group_add(key.line);resident_dirty_sectors_+=g::sector_popcount(mask);}
        }
        age_enforce(context,write);
        if(decision.read_ticket.valid)
            require(l1_.complete_read(decision.read_ticket),"direct immediate L1 read completion failed");
    }
public:
    FunctionalCache(const g::PerSmL1Config& l1,U l2_bytes,
                    std::function<U(int,U)> mapper,
                    std::function<void(const native_trace::Record&)> output,
                    const g::L2GeometryConfig& geometry = g::L2GeometryConfig(), DiagnosticOptions options={}, OwnerObserver observer={})
        :options_(options),observe_(std::move(observer)),ef_hit_throttle_(options.ef_hit_rate<0?0:std::uint32_t(options.ef_hit_rate)),l1_(l1),max_lines_(l2_bytes/128),lru_(geometry,l2_bytes,128),
         map_(std::move(mapper)),emit_(std::move(output)) {
        require(l2_bytes>0&&l2_bytes%128==0,"direct L2 requires positive128B capacity");
        require(l1.line_bytes==128,"direct L1 requires native128B lines");
        require(!options_.ef_fill_only||(l1.sector32&&!options_.skip_store_rfo&&!options_.bypass_streaming_reads),"EF fill-only diagnostic requires sector L1 and unchanged RFO/admission");
        require(options_.ef_hit_rate>=-1&&options_.ef_hit_rate<=65536,"EF hit rate out of range");
        require(options_.ef_hit_rate<0||(l1.sector32&&!options_.ef_fill_only&&!options_.skip_store_rfo&&!options_.bypass_streaming_reads),"EF hit calibration requires sector L1 and no simultaneous cache override");
        require(!options_.dirty_age_accesses||(!options_.skip_store_rfo&&!options_.bypass_streaming_reads&&!options_.ef_fill_only&&options_.ef_hit_rate>=0),"dirty age requires unchanged frozen admission/RFO and explicit EF rate");
        l2_.reserve(std::size_t(max_lines_));dirty_lines_by_group_.assign(lru_.geometry().group_count(),0);
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
                std::uint8_t requested_mask=0;
                for(const auto& range:sub.ranges){
                    const U begin=std::max(line,range.offset_bytes),end=std::min(line|U{127},range.offset_bytes+range.byte_count-1);
                    if(begin<=end)for(unsigned sector=unsigned((begin-line)/32);sector<=unsigned((end-line)/32);++sector)requested_mask|=std::uint8_t(1U<<sector);
                }
                require(requested_mask>0&&requested_mask<16,"source line has empty sector coverage");
                access({matrix,line},write,std::uint8_t(mask),requested_mask,bypass,context);
            }
        }
    }
    void verify_resident_ledger() const {
        U resident_lines=0,resident_sectors=0;std::vector<U> groups(dirty_lines_by_group_.size(),0);
        for(const auto& [key,entry]:l2_)if(entry.dirty){++resident_lines;++groups.at(lru_.group(key.line));resident_sectors+=g::sector_popcount(entry.dirty);}
        require(groups==dirty_lines_by_group_,"independent dirty group gauge audit failed");
        if(options_.dirty_age_accesses){
            require(age_queue_.size()==resident_lines,"age queue gauge mismatch");std::unordered_map<CacheKey,bool,CacheKeyHash> seen;U previous=0;
            for(auto it=age_queue_.begin();it!=age_queue_.end();++it){auto found=l2_.find(*it);require(found!=l2_.end()&&found->second.dirty&&found->second.age_position==it&&seen.emplace(*it,true).second,"age queue identity mismatch");require(found->second.last_write_sequence>=previous&&found->second.last_write_sequence<=l2_access_sequence_,"age queue time order");previous=found->second.last_write_sequence;require(!dirty_age_due(l2_access_sequence_,previous,options_.dirty_age_accesses),"expired dirty line remained");}
        }else require(age_queue_.empty(),"disabled age queue must stay empty");
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
        verify_resident_ledger();std::array<U,65> resident{};
        for(const auto& [key,entry]:l2_)if(entry.dirty)resident.at(age_bucket(l2_access_sequence_-entry.last_write_sequence))+=g::sector_popcount(entry.dirty);
        return {{"clock","FORWARDED_L2_128B_LINE_ACCESSES_NOT_TIME"},{"age_budget_accesses",options_.dirty_age_accesses},{"off",options_.dirty_age_accesses==0},{"now",l2_access_sequence_},{"resident_dirty_age_histogram",age_histogram(resident)},{"capacity_evicted_dirty_age_histogram",age_histogram(capacity_age_sector_hist_)},{"age_written_dirty_age_histogram",age_histogram(age_written_sector_hist_)},{"dirty_queue_entries",age_queue_.size()},{"dirty_queue_peak_entries",age_queue_peak_},{"store_refresh_precedes_same_tick_expiry",true},{"independent_queue_scan_verified",true}};
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
        require(dirty_.dirty_sector_creations==resident_dirty_sectors_+dirty_.evicted_dirty_sectors+age_sectors_,
                "direct dirty sector conservation failed");
        require(writebacks_==dirty_.evicted_dirty_sectors+age_sectors_&&l1_.live_read_tickets()==0,
                "direct writeback/read-ticket conservation failed");
        const auto l1=l1_.statistics();
        return {{"L2_forwarded_access_sequence",l2_access_sequence_},{"age_writeback_triggers",age_triggers_},{"age_writeback_lines",age_lines_},{"age_writeback_sectors",age_sectors_},{"age_writeback_bytes",age_sectors_*32},{"age_writeback_masks",age_masks_},{"age_rewrite_while_dirty_accesses",age_rewrites_},{"age_redirty_retained_lines",age_redirty_},{"capacity_eviction_writeback_bytes",dirty_.evicted_dirty_sectors*32},{"dirty_age_budget_accesses",options_.dirty_age_accesses},{"diagnostic_skipped_store_RFO_requests",skipped_store_rfo_},{"diagnostic_streaming_bypass_requests",streaming_bypass_},{"source_read_bytes",read_source_bytes_},{"source_write_bytes",write_source_bytes_},
            {"source_memory_instructions",source_instructions_},{"source_ranges",source_ranges_},
            {"source_projection_fnv1a64",source_hash_},{"DRAM_read_bytes",fills_*128},
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
    }
};
} // namespace direct_native
