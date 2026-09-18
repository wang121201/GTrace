#pragma once
// Generic resident-CTA representation. Original dispatch/retire and scheduling
// policies are unchanged. No stage-specific or kernel-specific graph rewriting.
#include "gpu.h"
#include "cta_sparse_slots.h"
#include "cta_warp_placement.h"
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace GTSim {
class CtaGraphStore {
public:
    struct Span { int first_node,node_count; };
    struct Spec {
        int cta_count,warps_per_cta,sm_count;
        std::size_t total_nodes;
        std::vector<Span> spans;
        int resident_cta_limit_per_sm=4;
        bool per_sm_warp_placement=false;
        // Explicit abstract contracts, not native HMMA/cp.async admission.
        bool allow_declared_tensor_work=false;
        bool allow_abstract_async_copy=false;
        bool allow_observed_async_shared_service=false;
    };
    using Owned=std::vector<std::unique_ptr<DAGNode>>;
    using BuildCallback=std::function<Owned(int)>;
    using RetireCallback=std::function<void(int,const std::vector<DAGNode*>&,Cycle)>;
    struct Limits {
        std::size_t max_nodes_per_cta = 200000;
        std::size_t max_explicit_ranges_per_cta = 2000000;
        std::size_t max_live_nodes = 4000000;
        std::size_t max_live_explicit_ranges = 16000000;
        bool detailed_storage_telemetry = false;
    };
private:
    enum class State : unsigned char { Unmaterialized, Resident, RetirePending, Retired };
    struct Live { Owned owned; std::vector<DAGNode*> nodes; std::size_t ranges=0; };
    Spec spec_;
    Limits limits_;
    BuildCallback build_;
    RetireCallback retire_;
    std::vector<State> state_;
    std::map<int,Live> live_;
    std::vector<std::pair<int,Cycle>> pending_;
    GPU* gpu_=nullptr;
    CtaSparseSlots<std::uint8_t>* scoreboard_=nullptr;
    CtaSparseSlots<Subpartition*>* to_sp_=nullptr;
    CtaSparseSlots<int>* to_sm_=nullptr;
    bool initialized_=false,initial_dispatch_=false,detached_=false,failed_=false;
    std::map<std::string,std::uint64_t> counts_;
    static void require(bool ok,const char* why) {
        if(!ok) throw std::logic_error(why);
    }
    bool contains(int c,int id) const {
        const auto& span=spec_.spans.at(c);
        return id>=span.first_node && id-span.first_node<span.node_count;
    }
    enum StorageGroup : std::size_t { S_SCOREBOARD, S_NODE_TO_SP, S_NODE_TO_SM, S_NODE_TO_TB, S_TMEM_WARP_THROTTLE, S_SCHEDULER_WARP, S_READY_WARP, S_WARP_HEAD, S_LAST_PIPELINE, S_LAST_ISSUE, S_RESIDENT_FLAG, S_TMA_THROTTLE, S_TMA_INTERVAL, S_MEMORY_THROTTLE, STORAGE_GROUPS };
    enum StorageField : std::size_t { F_ENTRIES, F_LOGICAL_LENGTH, F_PAYLOAD_BYTES, F_BUCKETS, F_OWNERS, STORAGE_FIELDS };
    enum DirectField : std::size_t {
        D_NODE_DICT=std::size_t(STORAGE_GROUPS)*STORAGE_FIELDS, D_TB_MAP, D_TB_NODES, STORAGE_VALUES
    };
    using StorageValues=std::uint64_t[STORAGE_VALUES];
    template<class T> void add_slots(StorageGroup group,const CtaSparseSlots<T>& slots,
                                      StorageValues& totals) const {
        const std::size_t offset=std::size_t(group)*STORAGE_FIELDS;
        totals[offset+F_ENTRIES]+=slots.entries();
        totals[offset+F_LOGICAL_LENGTH]+=slots.size();
        totals[offset+F_PAYLOAD_BYTES]+=slots.entry_payload_bytes();
        totals[offset+F_BUCKETS]+=slots.bucket_count();
        totals[offset+F_OWNERS]+=1;
    }
    void sample_storage() {
        if (!limits_.detailed_storage_telemetry) return;
        StorageValues values{};
        add_slots(S_SCOREBOARD,*scoreboard_,values);
        add_slots(S_NODE_TO_SP,*to_sp_,values); add_slots(S_NODE_TO_SM,*to_sm_,values);
        for(auto* sm:gpu_->sms) {
            add_slots(S_NODE_TO_TB,sm->node_id_to_tb_id,values);
            add_slots(S_TMEM_WARP_THROTTLE,sm->tmem->cta_warp_slots(),values);
            for(auto* sp:sm->sps) {
                const auto& s=*sp->sp_scheduler;
                add_slots(S_SCHEDULER_WARP,s.scheduler_warp,values);
                add_slots(S_READY_WARP,s.ready_warp,values);
                add_slots(S_WARP_HEAD,s.warp_head_index,values);
                add_slots(S_LAST_PIPELINE,s.last_issued_pipeline,values);
                add_slots(S_LAST_ISSUE,s.last_issue_cycle,values);
                add_slots(S_RESIDENT_FLAG,s.warp_is_resident,values);
                add_slots(S_TMA_THROTTLE,s.tma_next_issue_cycle,values);
                add_slots(S_TMA_INTERVAL,s.tma_issue_interval_cache,values);
                for(const auto& v:s.memory_next_issue_cycle)add_slots(S_MEMORY_THROTTLE,v,values);
                values[D_NODE_DICT]+=s.node_dict.size();
                values[D_TB_MAP]+=s.tb_to_warps.size();
            }
            values[D_TB_NODES]+=sm->tb_to_nodes.size();
        }
        // Same 73 values and lexicographic publication order as the old map.
        // String construction occurs only here, never in the per-owner scan.
        struct PublishedField { std::size_t index; const char* name; };
        static constexpr PublishedField fields[] = {
            {std::size_t(S_LAST_ISSUE)*STORAGE_FIELDS+F_BUCKETS,"last_issue_buckets"},
            {std::size_t(S_LAST_ISSUE)*STORAGE_FIELDS+F_ENTRIES,"last_issue_entries"},
            {std::size_t(S_LAST_ISSUE)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"last_issue_logical_length"},
            {std::size_t(S_LAST_ISSUE)*STORAGE_FIELDS+F_OWNERS,"last_issue_owners"},
            {std::size_t(S_LAST_ISSUE)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"last_issue_payload_bytes"},
            {std::size_t(S_LAST_PIPELINE)*STORAGE_FIELDS+F_BUCKETS,"last_pipeline_buckets"},
            {std::size_t(S_LAST_PIPELINE)*STORAGE_FIELDS+F_ENTRIES,"last_pipeline_entries"},
            {std::size_t(S_LAST_PIPELINE)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"last_pipeline_logical_length"},
            {std::size_t(S_LAST_PIPELINE)*STORAGE_FIELDS+F_OWNERS,"last_pipeline_owners"},
            {std::size_t(S_LAST_PIPELINE)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"last_pipeline_payload_bytes"},
            {std::size_t(S_MEMORY_THROTTLE)*STORAGE_FIELDS+F_BUCKETS,"memory_throttle_buckets"},
            {std::size_t(S_MEMORY_THROTTLE)*STORAGE_FIELDS+F_ENTRIES,"memory_throttle_entries"},
            {std::size_t(S_MEMORY_THROTTLE)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"memory_throttle_logical_length"},
            {std::size_t(S_MEMORY_THROTTLE)*STORAGE_FIELDS+F_OWNERS,"memory_throttle_owners"},
            {std::size_t(S_MEMORY_THROTTLE)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"memory_throttle_payload_bytes"},
            {std::size_t(S_NODE_TO_SM)*STORAGE_FIELDS+F_BUCKETS,"node_to_sm_buckets"},
            {std::size_t(S_NODE_TO_SM)*STORAGE_FIELDS+F_ENTRIES,"node_to_sm_entries"},
            {std::size_t(S_NODE_TO_SM)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"node_to_sm_logical_length"},
            {std::size_t(S_NODE_TO_SM)*STORAGE_FIELDS+F_OWNERS,"node_to_sm_owners"},
            {std::size_t(S_NODE_TO_SM)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"node_to_sm_payload_bytes"},
            {std::size_t(S_NODE_TO_SP)*STORAGE_FIELDS+F_BUCKETS,"node_to_sp_buckets"},
            {std::size_t(S_NODE_TO_SP)*STORAGE_FIELDS+F_ENTRIES,"node_to_sp_entries"},
            {std::size_t(S_NODE_TO_SP)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"node_to_sp_logical_length"},
            {std::size_t(S_NODE_TO_SP)*STORAGE_FIELDS+F_OWNERS,"node_to_sp_owners"},
            {std::size_t(S_NODE_TO_SP)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"node_to_sp_payload_bytes"},
            {std::size_t(S_NODE_TO_TB)*STORAGE_FIELDS+F_BUCKETS,"node_to_tb_buckets"},
            {std::size_t(S_NODE_TO_TB)*STORAGE_FIELDS+F_ENTRIES,"node_to_tb_entries"},
            {std::size_t(S_NODE_TO_TB)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"node_to_tb_logical_length"},
            {std::size_t(S_NODE_TO_TB)*STORAGE_FIELDS+F_OWNERS,"node_to_tb_owners"},
            {std::size_t(S_NODE_TO_TB)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"node_to_tb_payload_bytes"},
            {std::size_t(S_READY_WARP)*STORAGE_FIELDS+F_BUCKETS,"ready_warp_buckets"},
            {std::size_t(S_READY_WARP)*STORAGE_FIELDS+F_ENTRIES,"ready_warp_entries"},
            {std::size_t(S_READY_WARP)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"ready_warp_logical_length"},
            {std::size_t(S_READY_WARP)*STORAGE_FIELDS+F_OWNERS,"ready_warp_owners"},
            {std::size_t(S_READY_WARP)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"ready_warp_payload_bytes"},
            {std::size_t(S_RESIDENT_FLAG)*STORAGE_FIELDS+F_BUCKETS,"resident_flag_buckets"},
            {std::size_t(S_RESIDENT_FLAG)*STORAGE_FIELDS+F_ENTRIES,"resident_flag_entries"},
            {std::size_t(S_RESIDENT_FLAG)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"resident_flag_logical_length"},
            {std::size_t(S_RESIDENT_FLAG)*STORAGE_FIELDS+F_OWNERS,"resident_flag_owners"},
            {std::size_t(S_RESIDENT_FLAG)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"resident_flag_payload_bytes"},
            {D_NODE_DICT,"scheduler_node_dict_entries"},
            {D_TB_MAP,"scheduler_tb_map_entries"},
            {std::size_t(S_SCHEDULER_WARP)*STORAGE_FIELDS+F_BUCKETS,"scheduler_warp_buckets"},
            {std::size_t(S_SCHEDULER_WARP)*STORAGE_FIELDS+F_ENTRIES,"scheduler_warp_entries"},
            {std::size_t(S_SCHEDULER_WARP)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"scheduler_warp_logical_length"},
            {std::size_t(S_SCHEDULER_WARP)*STORAGE_FIELDS+F_OWNERS,"scheduler_warp_owners"},
            {std::size_t(S_SCHEDULER_WARP)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"scheduler_warp_payload_bytes"},
            {std::size_t(S_SCOREBOARD)*STORAGE_FIELDS+F_BUCKETS,"scoreboard_buckets"},
            {std::size_t(S_SCOREBOARD)*STORAGE_FIELDS+F_ENTRIES,"scoreboard_entries"},
            {std::size_t(S_SCOREBOARD)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"scoreboard_logical_length"},
            {std::size_t(S_SCOREBOARD)*STORAGE_FIELDS+F_OWNERS,"scoreboard_owners"},
            {std::size_t(S_SCOREBOARD)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"scoreboard_payload_bytes"},
            {D_TB_NODES,"sm_tb_nodes_entries"},
            {std::size_t(S_TMA_INTERVAL)*STORAGE_FIELDS+F_BUCKETS,"tma_interval_buckets"},
            {std::size_t(S_TMA_INTERVAL)*STORAGE_FIELDS+F_ENTRIES,"tma_interval_entries"},
            {std::size_t(S_TMA_INTERVAL)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"tma_interval_logical_length"},
            {std::size_t(S_TMA_INTERVAL)*STORAGE_FIELDS+F_OWNERS,"tma_interval_owners"},
            {std::size_t(S_TMA_INTERVAL)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"tma_interval_payload_bytes"},
            {std::size_t(S_TMA_THROTTLE)*STORAGE_FIELDS+F_BUCKETS,"tma_throttle_buckets"},
            {std::size_t(S_TMA_THROTTLE)*STORAGE_FIELDS+F_ENTRIES,"tma_throttle_entries"},
            {std::size_t(S_TMA_THROTTLE)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"tma_throttle_logical_length"},
            {std::size_t(S_TMA_THROTTLE)*STORAGE_FIELDS+F_OWNERS,"tma_throttle_owners"},
            {std::size_t(S_TMA_THROTTLE)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"tma_throttle_payload_bytes"},
            {std::size_t(S_TMEM_WARP_THROTTLE)*STORAGE_FIELDS+F_BUCKETS,"tmem_warp_throttle_buckets"},
            {std::size_t(S_TMEM_WARP_THROTTLE)*STORAGE_FIELDS+F_ENTRIES,"tmem_warp_throttle_entries"},
            {std::size_t(S_TMEM_WARP_THROTTLE)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"tmem_warp_throttle_logical_length"},
            {std::size_t(S_TMEM_WARP_THROTTLE)*STORAGE_FIELDS+F_OWNERS,"tmem_warp_throttle_owners"},
            {std::size_t(S_TMEM_WARP_THROTTLE)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"tmem_warp_throttle_payload_bytes"},
            {std::size_t(S_WARP_HEAD)*STORAGE_FIELDS+F_BUCKETS,"warp_head_buckets"},
            {std::size_t(S_WARP_HEAD)*STORAGE_FIELDS+F_ENTRIES,"warp_head_entries"},
            {std::size_t(S_WARP_HEAD)*STORAGE_FIELDS+F_LOGICAL_LENGTH,"warp_head_logical_length"},
            {std::size_t(S_WARP_HEAD)*STORAGE_FIELDS+F_OWNERS,"warp_head_owners"},
            {std::size_t(S_WARP_HEAD)*STORAGE_FIELDS+F_PAYLOAD_BYTES,"warp_head_payload_bytes"},
        };
        static_assert(STORAGE_GROUPS==14 && STORAGE_FIELDS==5 && STORAGE_VALUES==73,
                      "fixed storage telemetry field contract");
        static_assert(sizeof(fields)/sizeof(fields[0])==STORAGE_VALUES,
                      "one publication per original field");
        for(const auto& field:fields) {
            const std::string key=field.name;
            const auto value=values[field.index];
            counts_[key]=value;
            counts_["peak_"+key]=std::max(counts_["peak_"+key],value);
        }
    }
    void materialize(int c) {
        require(!failed_ && state_.at(c)==State::Unmaterialized,"CTA duplicate materialization");
        require(static_cast<std::size_t>(spec_.spans.at(c).node_count)<=
                limits_.max_live_nodes-counts_["live_nodes"],"CTA live node budget exceeded");
        Live value; value.owned=build_(c);
        validate_cta(c,value.owned);
        for (const auto& n:value.owned) for (const auto& sub:n->explicit_memory_subops)
            value.ranges+=sub.ranges.size();
        for (const auto& n:value.owned) for (const auto& sub:n->async_copy_shared_subops) value.ranges+=sub.ranges.size();
        require(value.ranges<=limits_.max_live_explicit_ranges-counts_["live_explicit_ranges"],
                "CTA live explicit range budget exceeded");
        const int first=spec_.spans.at(c).first_node;
        for(const auto& node:value.owned)value.nodes.push_back(node.get());
        // Identical dependency-link construction order to DAG::build_dependency_graph.
        for(auto* n:value.nodes) {
            n->remaining_deps=static_cast<int>(n->depends_on.size()+n->issue_depends_on.size());
            n->issue_done=false;n->issue_deps_resolved=false;
        }
        for(auto* n:value.nodes) {
            for(int d:n->depends_on)value.nodes[d-first]->children.push_back(n);
            for(int d:n->issue_depends_on)value.nodes[d-first]->issue_children.push_back(n);
        }
        auto& live=live_.emplace(c,std::move(value)).first->second;
        auto* sm=gpu_->sms.at(c%spec_.sm_count);
        sm->tb_to_nodes.emplace(c,live.nodes);
        for(auto* sp:sm->sps)sp->sp_scheduler->host_note_node_membership_change();
        for(auto* n:live.nodes) {
            auto* sp=sm->sps.at(n->warp_id%4);auto& s=*sp->sp_scheduler;
            require(s.node_dict.emplace(n->id,n).second,"CTA node map collision");
            s.scheduler_warp[n->warp_id].push_back(n);
            if(n->remaining_deps==0)s.ready_warp[n->warp_id].push_back(n);
            (*to_sp_)[n->id]=sp;(*to_sm_)[n->id]=sm->sm_id;
            sm->node_id_to_tb_id[n->id]=c;
        }
        // Ascending CTA-local warps yield ascending scheduler tokens within each SP.
        for(int local_warp=0;local_warp<spec_.warps_per_cta;++local_warp) {
            const int w=warp_token(c,local_warp);
            auto& s=*sm->sps.at(w%4)->sp_scheduler;
            // Eager init_with_nodes registers only warps carrying source nodes.
            if(!static_cast<const decltype(s.scheduler_warp)&>(s.scheduler_warp)[w].empty())
                s.tb_to_warps[c].push_back(w);
        }
        state_[c]=State::Resident;
        counts_["built_ctas"]++;counts_["built_nodes"]+=live.nodes.size();
        if(initial_dispatch_)counts_["initial_materialized_ctas"]++;
        else counts_["dispatch_materialized_ctas"]++;
        counts_["live_nodes"]+=live.nodes.size();counts_["live_ctas"]++;
        counts_["live_explicit_ranges"]+=live.ranges;
        counts_["peak_live_explicit_ranges"]=std::max(counts_["peak_live_explicit_ranges"],counts_["live_explicit_ranges"]);
        counts_["peak_live_nodes"]=std::max(counts_["peak_live_nodes"],counts_["live_nodes"]);
        counts_["peak_live_ctas"]=std::max(counts_["peak_live_ctas"],counts_["live_ctas"]);
        sample_storage();
    }
    void mark_retired(int c,Cycle cycle) {
        require(state_.at(c)==State::Resident,"CTA invalid original retire notification");
        state_[c]=State::RetirePending;pending_.push_back({c,cycle});
        counts_["deferred_retire_ctas"]++;
    }
    void check_obligations(int c) {
        auto check_id=[&](int id){require(!contains(c,id),"CTA still has live completion/retry obligation");};
        const auto& span=spec_.spans.at(c);
        require(gpu_->l2->cta_pending_node_references(span.first_node,
                    span.first_node+span.node_count)==0,"CTA has L2 waiter/completion obligation");
        for(auto* sm:gpu_->sms) {
            require(sm->tma->pending_node_references(span.first_node,
                        span.first_node+span.node_count)==0,
                    "CTA has bulk-copy pointer/issue obligation");
            for(auto* mem:{sm->sram,sm->tmem_mem}) {
                for(const auto& q:mem->memory_queue)for(int id:std::get<0>(q))check_id(id);
                for(const auto& q:mem->memory_write_queue)for(int id:std::get<0>(q))check_id(id);
            }
            for(auto* sp:sm->sps) {
                for(auto* p:sp->pipelines)for(const auto& q:p->pipeline_executing_nodes)check_id(std::get<0>(q));
                for(const auto& q:sp->barrier_queue)check_id(q.first);
                for(const auto& q:sp->retry_queue_sram)check_id(q.node_id);
                for(const auto& q:sp->retry_queue_l2)check_id(q.node_id);
                for(const auto& q:sp->pending_memory_ops)check_id(q.first);
                for(const auto& q:sp->async_shared_ready)check_id(q.first);
                for(const auto& q:sp->whole_tile_frontend_completions)check_id(q.first);
                for(int id:sp->deferred_tmem_pair_completions)check_id(id);
            }
        }
        counts_["reclamation_obligation_checks"]++;
    }
    void reclaim(int c,Cycle cycle) {
        auto& value=live_.at(c);auto* sm=gpu_->sms.at(c%spec_.sm_count);
        require(!sm->is_tb_resident(c) && sm->tb_remaining_count.at(c)==0,"CTA reclaimed before original retirement");
        for(auto* n:value.nodes) {
            require(n->finished && n->issue_done && n->issue_deps_resolved &&
                    (n->op_type==OpType::BARRIER
                        ? n->pending_transactions==1 && n->total_transactions==1
                        : n->pending_transactions==0) && n->remaining_deps==0 &&
                    n->next_transaction_index==n->total_transactions,"CTA unfinished node reclamation");
            for(auto* child:n->children)require(contains(c,child->id),"CTA external child pointer");
            for(auto* child:n->issue_children)require(contains(c,child->id),"CTA external issue child pointer");
        }
        check_obligations(c);
        // Audit before freeing; callback exceptions are fail-stop, never retried.
        retire_(c,value.nodes,cycle);
        for(auto* sp:sm->sps)sp->sp_scheduler->host_note_node_membership_change();
        for(auto* n:value.nodes) {
            auto& s=*sm->sps.at(n->warp_id%4)->sp_scheduler;
            require(s.node_dict.at(n->id)==n && (*to_sp_)[n->id]==sm->sps.at(n->warp_id%4),"CTA pointer identity lost");
            s.node_dict.erase(n->id);to_sp_->erase_slot(n->id);to_sm_->erase_slot(n->id);
            scoreboard_->erase_slot(n->id);sm->node_id_to_tb_id.erase_slot(n->id);
        }
        for(int local_warp=0;local_warp<spec_.warps_per_cta;++local_warp) {
            const int w=warp_token(c,local_warp);
            // Four peer queries may have inspected this global warp on any SP.
            for(auto* sp:sm->sps) {
                auto& s=*sp->sp_scheduler;
                require(std::find(s.resident_warps.begin(),s.resident_warps.end(),w)==s.resident_warps.end(),"CTA resident warp remains");
                s.scheduler_warp.erase_slot(w);s.ready_warp.erase_slot(w);
                s.warp_head_index.erase_slot(w);s.last_issued_pipeline.erase_slot(w);
                s.last_issue_cycle.erase_slot(w);s.warp_is_resident.erase_slot(w);
                s.tma_next_issue_cycle.erase_slot(w);s.tma_issue_interval_cache.erase_slot(w);
                for(auto& v:s.memory_next_issue_cycle)v.erase_slot(w);
            }
            sm->tmem->cta_erase_warp_slot(w);
        }
        for(auto* sp:sm->sps)sp->sp_scheduler->tb_to_warps.erase(c);
        sm->tb_to_nodes.erase(c);sm->tb_remaining_count.erase(c);
        counts_["live_nodes"]-=value.nodes.size();counts_["live_ctas"]--;
        counts_["live_explicit_ranges"]-=value.ranges;
        counts_["retired_nodes"]+=value.nodes.size();counts_["retired_ctas"]++;
        live_.erase(c);state_[c]=State::Retired;
        sample_storage();
    }
public:
    // Pure pre-install gate, also used by bounded negative tests. No GPU action.
    void validate_cta(int c,const Owned& owned) const {
        require(c>=0 && c<spec_.cta_count,"CTA outside declared layout");
        const auto& span=spec_.spans.at(c);
        require(owned.size()==static_cast<std::size_t>(span.node_count),"CTA node count");
        const int first=span.first_node;
        // The owner keeps every name alive throughout this read-only gate.
        std::unordered_set<std::string_view> names;names.reserve(owned.size());
        std::array<int,32> warp_tokens{};
        for(int local=0;local<spec_.warps_per_cta;++local)warp_tokens[local]=warp_token(c,local);
        // A per-node generation preserves the shared completion/issue duplicate
        // domain without allocating a tree for each node. Check bounds first.
        std::vector<int> edge_generation(span.node_count,-1);
        std::size_t ranges=0;
        // Validate the ENTIRE CTA before installing any pointer/ready entry.
        for(int i=0;i<span.node_count;++i) {
            auto* n=owned[i].get();
            require(n && n->id==first+i && n->thread_block_id==c &&
                    n->sm_id==c%spec_.sm_count,"CTA global identity/placement");
            const auto warp_end=warp_tokens.begin()+spec_.warps_per_cta;
            const bool valid_warp=std::find(warp_tokens.begin(),warp_end,n->warp_id)!=warp_end;
            require(valid_warp,"CTA scheduler warp token identity");
            require(names.insert(n->name).second,"CTA duplicate node name");
            require(n->op_type==OpType::LD_DRAM2REG || n->op_type==OpType::ST_REG2DRAM ||
                    n->op_type==OpType::LD_SRAM2REG || n->op_type==OpType::ST_REG2SRAM ||
                    (n->op_type==OpType::MMA && spec_.allow_declared_tensor_work) ||
                    (n->op_type==OpType::CP_DRAM2SRAM_LDGSTS && (spec_.allow_abstract_async_copy || spec_.allow_observed_async_shared_service)) ||
                    (n->op_type==OpType::COMPUTE && (n->pipeline_type=="SIMD" ||
                        n->pipeline_type=="SFU" || n->pipeline_type=="SHFL")) ||
                    (n->op_type==OpType::BARRIER && n->pipeline_type=="BARRIER"),
                    "resident CTA unsupported operation");
            if(n->op_type==OpType::MMA) {
                require(n->pipeline_type=="Tensor" && n->tile.ndims==2 &&
                        n->tensor_reduction_extent>0 && n->setup_latency==0 &&
                        n->explicit_memory_subops.empty() && !n->has_explicit_global_line_span,
                        "resident MMA requires declared 2D FMA work without memory/timing override");
                const auto work=n->tensor_fma_work_per_subpartition();
                require(work>0 && work<=std::numeric_limits<int>::max(),
                        "resident MMA declared work budget");
            }
            require(!n->async_copy_bypass_l1 ||
                        (n->explicit_async_shared_service_v1 && n->op_type==OpType::CP_DRAM2SRAM_LDGSTS),
                    "per-request copy bypass requires explicit observed copy service");
            require(n->explicit_async_shared_service_v1==!n->async_copy_shared_subops.empty(),
                    "copy destination service flag/ranges mismatch");
            if(n->explicit_async_shared_service_v1) {
                require(spec_.allow_observed_async_shared_service && n->op_type==OpType::CP_DRAM2SRAM_LDGSTS && n->async_copy_phase==0 &&
                        n->async_copy_shared_subops.size()==1 && n->explicit_memory_subops.size()==1,
                        "observed copy requires one source/destination warp subop and explicit opt-in");
                const auto& dst=n->async_copy_shared_subops.front();
                const auto svc=describe_explicit_sram_ranges(dst);
                require(svc.same_word_collapses==0,"overlapping copy destinations unmodeled");
                require(dst.source_member_ordinals.size()==dst.ranges.size(),"copy destination lane identity");
                std::map<int,MemoryByteRange> lanes;
                for(std::size_t k=0;k<dst.ranges.size();++k){const auto& r=dst.ranges[k];
                    require(dst.source_member_ordinals[k]==r.source_member_ordinal && r.byte_count==16 && r.offset_bytes%16==0,
                            "observed copy destination must be aligned actual 16B lane");lanes.emplace(r.source_member_ordinal,r);}
                for(const auto& r:n->explicit_memory_subops.front().ranges)
                    require(lanes.count(r.source_member_ordinal) && r.byte_count==16,"copy source lane lacks destination");
            }
            if(n->op_type==OpType::CP_DRAM2SRAM_LDGSTS) {
                require(n->pipeline_type=="LD" && n->setup_latency==0 &&
                        n->tma_setup_latency==-1 && n->tma_issue_interval==-1 &&
                        n->tma_issue_rate_bytes_per_cycle==-1 &&
                        !n->has_explicit_global_line_span && !n->explicit_memory_subops.empty(),
                        "resident async copy requires explicit global lanes and inherited engine timing");
                for(const auto& sub:n->explicit_memory_subops) {
                    require((!sub.ranges.empty() || n->explicit_async_shared_service_v1) && sub.ranges.size()<=32 &&
                            sub.source_member_ordinals.size()==sub.ranges.size(),
                            "resident async copy nonempty lane subop required");
                    std::uint64_t bytes=0;std::set<int> members;
                    for(std::size_t k=0;k<sub.ranges.size();++k) {
                        const auto& range=sub.ranges[k];
                        require(range.source_member_ordinal>=0 && range.source_member_ordinal<32 &&
                                members.insert(range.source_member_ordinal).second &&
                                sub.source_member_ordinals[k]==range.source_member_ordinal &&
                                (range.byte_count==4 || range.byte_count==8 || range.byte_count==16) &&
                                range.offset_bytes%range.byte_count==0 &&
                                range.offset_bytes<=std::numeric_limits<std::uint64_t>::max()-range.byte_count,
                                "resident async copy lane identity/alignment/width");
                        bytes+=range.byte_count;
                    }
                    require(bytes==sub.requested_bytes,"resident async copy requested-byte census");
                }
            }
            const bool shared=n->op_type==OpType::LD_SRAM2REG || n->op_type==OpType::ST_REG2SRAM;
            require(shared==n->explicit_sram_bank_service_v1,
                    "resident shared instruction requires explicit local SRAM service opt-in only");
            if(shared) {
                require(!n->explicit_memory_subops.empty(),"resident shared instruction requires lane subops");
                for(std::size_t k=0;k<n->explicit_memory_subops.size();++k)
                    (void)describe_explicit_sram_service(*n,static_cast<int>(k));
            }
            require(n->issue_group_kind==IssueGroupKind::NONE && n->cta_group_size==1 &&
                    n->peer_cta_id==-1 && n->target_sm_id==-1,"F1 unsupported group/remote coupling");
            require(!n->finished && n->start==-1 && n->end==-1 &&
                    n->children.empty() && n->issue_children.empty(),"CTA not fresh source node");
            const auto unique_edge=[&](int dep) {
                if(dep<first||dep>=n->id)return false;
                auto& generation=edge_generation[dep-first];
                if(generation==i)return false;
                generation=i;return true;
            };
            for(int dep:n->depends_on)require(unique_edge(dep),"CTA nonlocal/non-topological/duplicate completion dependency");
            for(int dep:n->issue_depends_on)require(unique_edge(dep),"CTA nonlocal/non-topological/duplicate issue dependency");
            for(const auto& sub:n->explicit_memory_subops) {
                require(sub.ranges.size()<=limits_.max_explicit_ranges_per_cta-ranges,
                        "CTA explicit range budget exceeded");
                ranges+=sub.ranges.size();
            }
            for(const auto& sub:n->async_copy_shared_subops) {
                require(sub.ranges.size()<=limits_.max_explicit_ranges_per_cta-ranges,
                        "CTA async destination explicit range budget exceeded");
                ranges+=sub.ranges.size();
            }
        }
    }
    CtaGraphStore(Spec spec,BuildCallback build,RetireCallback retire)
      :CtaGraphStore(std::move(spec),std::move(build),std::move(retire),Limits{}) {}
    CtaGraphStore(Spec spec,BuildCallback build,RetireCallback retire,Limits limits)
      :spec_(std::move(spec)),limits_(limits),build_(std::move(build)),retire_(std::move(retire)) {
        const auto maximum=static_cast<std::size_t>(std::numeric_limits<int>::max());
        require(spec_.warps_per_cta>=1 && spec_.warps_per_cta<=32 &&
                spec_.cta_count>0 && spec_.cta_count<=std::numeric_limits<int>::max()/spec_.warps_per_cta &&
                spec_.sm_count==48 && spec_.resident_cta_limit_per_sm>=1 &&
                spec_.resident_cta_limit_per_sm<=32 &&
                spec_.total_nodes>0 && spec_.total_nodes<=maximum &&
                spec_.spans.size()==static_cast<std::size_t>(spec_.cta_count) &&
                build_ && retire_,"resident CTA specification");
        (void)warp_count(); // Bound the complete scheduler-token domain before installation.
        require(limits_.max_nodes_per_cta>0 && limits_.max_live_nodes>0 &&
                limits_.max_explicit_ranges_per_cta>0 && limits_.max_live_explicit_ranges>0,
                "CTA positive graph limits required");
        std::size_t next=0;
        for(const auto& span:spec_.spans) {
            require(span.first_node>=0 && span.node_count>0 &&
                    static_cast<std::size_t>(span.first_node)==next &&
                    static_cast<std::size_t>(span.node_count)<=spec_.total_nodes-next &&
                    static_cast<std::size_t>(span.node_count)<=limits_.max_nodes_per_cta,
                    "CTA spans must cover the node domain exactly in original order");
            next+=static_cast<std::size_t>(span.node_count);
        }
        require(next==spec_.total_nodes,"CTA spans leave an uncovered node suffix");
        state_.assign(spec_.cta_count,State::Unmaterialized);
        for(const auto* k:{"live_nodes","peak_live_nodes","live_ctas","peak_live_ctas","built_ctas",
                          "built_nodes","retired_ctas","retired_nodes","initial_materialized_ctas",
                          "dispatch_materialized_ctas","deferred_retire_ctas","reclamation_obligation_checks",
                          "live_explicit_ranges","peak_live_explicit_ranges"})counts_[k]=0;
        counts_["expected_nodes"]=node_count();counts_["expected_ctas"]=spec_.cta_count;
        counts_["sizeof_DAGNode"]=sizeof(DAGNode);
        counts_["cta_state_bytes"]=state_.size()*sizeof(State);
        counts_["cta_layout_bytes"]=spec_.spans.size()*sizeof(Span);
        counts_["detailed_storage_telemetry"]=limits_.detailed_storage_telemetry;
        counts_["max_nodes_per_cta"]=limits_.max_nodes_per_cta;
        counts_["max_live_nodes"]=limits_.max_live_nodes;
        counts_["max_explicit_ranges_per_cta"]=limits_.max_explicit_ranges_per_cta;
        counts_["max_live_explicit_ranges"]=limits_.max_live_explicit_ranges;
        counts_["resident_cta_limit_per_sm"]=spec_.resident_cta_limit_per_sm;
    }
    std::size_t node_count() const { return spec_.total_nodes; }
    int warp_token(int c,int w) const { return cta_placement::token(c,w,spec_.warps_per_cta,spec_.sm_count,4,spec_.per_sm_warp_placement); }
    int warp_count() const { return cta_placement::domain(spec_.cta_count,spec_.warps_per_cta,spec_.sm_count,4,spec_.per_sm_warp_placement); }
    void initialize(GPU* gpu,CtaSparseSlots<std::uint8_t>* scoreboard,
                    CtaSparseSlots<Subpartition*>* to_sp,CtaSparseSlots<int>* to_sm,
                    std::size_t* completed) {
        require(!initialized_ && gpu && !gpu->noc && !gpu->l2->uses_whole_tiles() &&
                gpu->sms.size()==static_cast<std::size_t>(spec_.sm_count),"F1 core scope/reuse");
        initialized_=true;gpu_=gpu;scoreboard_=scoreboard;to_sp_=to_sp;to_sm_=to_sm;
        scoreboard->assign(node_count(),0);to_sp->assign(node_count(),nullptr);to_sm->assign(node_count(),-1);
        for(auto* sm:gpu->sms) {
            require(sm->sps.size()==4 && sm->max_resident_tbs==spec_.resident_cta_limit_per_sm,
                    "resident CTA admission profile differs from explicit specification");
            if(spec_.allow_abstract_async_copy || spec_.allow_observed_async_shared_service)
                require(sm->tma->mechanism()==BulkCopyMechanism::ADA_LDGSTS,
                        "resident async copy requires explicit ADA_LDGSTS engine");
            if(spec_.allow_declared_tensor_work)for(auto* sp:sm->sps)for(auto* pipe:sp->pipelines)
                if(pipe->pipeline_name=="Tensor")require(pipe->tensor_issue_work_semantics==
                    TensorIssueWorkSemantics::DECLARED_FMA_WORK,
                    "resident MMA requires declared FMA work consumer");
            sm->node_id_to_tb_id.assign(node_count(),-1);
            for(auto* sp:sm->sps) {
                auto& s=*sp->sp_scheduler;
                s.scoreboard=scoreboard;s.subpartition_id=sp->subpartition_id;
                s.init_memory_issue_cycles(warp_count());
                sp->set_node_to_sp_map(to_sp);sp->set_completed_node_counter(completed);
            }
            sm->cta_materialize=[this](int c){materialize(c);};
            sm->cta_retire=[this](int c,Cycle cycle){mark_retired(c,cycle);};
        }
        gpu->set_node_to_sp_map(to_sp);gpu->set_node_to_sm_map(to_sm);
        for(int c=0;c<spec_.cta_count;++c) {
            auto* sm=gpu->sms.at(c%spec_.sm_count);
            sm->all_tb_ids.insert(c);sm->tb_remaining_count.emplace(c,spec_.spans.at(c).node_count);
        }
        initial_dispatch_=true;
        for(auto* sm:gpu->sms)sm->initialize_tb_scheduler({});
        initial_dispatch_=false;
    }
    void collect(Cycle cycle) {
        require(initialized_ && !detached_ && !failed_,"F1 collect lifecycle");
        try {
            if(!pending_.empty())sample_storage();
            for(const auto& p:pending_) {require(p.second==cycle,"F1 deferred collection cycle");reclaim(p.first,p.second);}
            pending_.clear();
        } catch(...) {failed_=true;throw;}
    }
    // Called while GPU exists, before its owning Simulator destructor deletes it.
    // Failure cleanup retains node ownership until all native containers are destroyed.
    void detach() noexcept {
        if(detached_ || !initialized_)return;
        for(auto* sm:gpu_->sms){
            sm->cta_materialize={};sm->cta_retire={};
        }
        detached_=true;gpu_=nullptr;scoreboard_=nullptr;to_sp_=nullptr;to_sm_=nullptr;
    }
    // Read-only post-stop diagnostic. Ownership remains here after detach;
    // no scheduler/cache pointer is read and no node may escape this call.
    // live_ is ordered by source CTA and each node vector is dense node-ID order.
    template<class Visitor> void visit_live_nodes(const Visitor& visit) const {
        for(const auto& entry:live_)for(const auto* node:entry.second.nodes)
            visit(entry.first,static_cast<const DAGNode*>(node));
    }
    std::map<std::string,std::uint64_t> telemetry() const { return counts_; }
};
} // namespace GTSim
