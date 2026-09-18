#pragma once
#include "cycle.h"
#include "dag_node.h"
#include "pipeline.h"
#include <array>
#include <memory>
#include <queue>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace GTSim::cycle_overlap {
using U=std::uint64_t;using J=nlohmann::json;
inline void need(bool ok,const char* why){if(!ok)throw std::logic_error(why);}
inline U add(U a,U b){need(b<=UINT64_MAX-a,"overlap counter overflow");return a+b;}
inline U product(U a,U b){need(!a||b<=UINT64_MAX/a,"overlap denominator overflow");return a*b;}
inline constexpr std::size_t max_live_events=1u<<20;
struct Unit {
    Cycle last=0,last_issue=-1;
    U compute=0,memory=0,peak_compute=0,peak_memory=0,compute_issue_events=0,compute_issue_cycles=0;
    std::array<U,4> bins{}; // neither, C only, M only, both
    void accrue(Cycle t){need(t>=last,"overlap interval regressed");bins[(compute?1:0)|(memory?2:0)]=add(bins[(compute?1:0)|(memory?2:0)],U(t-last));last=t;}
    void change(Cycle t,int dc,int dm){accrue(t);if(dc<0){need(compute>0,"compute inflight underflow");--compute;}else if(dc>0)++compute;
        if(dm<0){need(memory>0,"memory inflight underflow");--memory;}else if(dm>0)++memory;
        peak_compute=std::max(peak_compute,compute);peak_memory=std::max(peak_memory,memory);}
    void issue(Cycle t){++compute_issue_events;if(last_issue!=t){++compute_issue_cycles;last_issue=t;}}
};
struct PipeTag {const Pipeline* pipe=nullptr;bool compute=false;};
struct SpUnit {Unit time;std::array<PipeTag,16> pipes{};std::size_t pipe_count=0;};
struct End {Cycle cycle;std::size_t slot;U sequence;};
struct EndLater {bool operator()(const End& a,const End& b)const{return a.cycle!=b.cycle?a.cycle>b.cycle:a.sequence>b.sequence;}};
struct Coverage {
    U first_node_issues=0,compute_nodes=0,global_memory_nodes=0,barrier_nodes=0,shared_memory_nodes=0,unsupported_nodes=0,invalid_placement_nodes=0;
    U compute_subop_issues=0,global_memory_subop_issues=0,excluded_pipeline_subop_issues=0;
    U memory_nodes_completed=0,compute_subops_completed=0,peak_compute_end_events=0,peak_live_memory_nodes=0;
};
struct State {
    std::size_t sms,sps;Cycle start,frontier;Unit gpu;std::vector<Unit> sm;std::vector<SpUnit> sp;
    std::priority_queue<End,std::vector<End>,EndLater> ends;
    std::unordered_map<const DAGNode*,std::size_t> memory_nodes;
    Coverage coverage;U sequence=0;
    State(std::size_t nsm,std::size_t nsp,Cycle c):sms(nsm),sps(nsp),start(c),frontier(c),sm(nsm),sp(nsm*nsp){gpu.last=c;for(auto& x:sm)x.last=c;for(auto& x:sp)x.time.last=c;}
    bool placement(int sm_id,int sp_id)const{return sm_id>=0&&std::size_t(sm_id)<sms&&sp_id>=0&&std::size_t(sp_id)<sps;}
    std::size_t slot(int sm_id,int sp_id)const{return std::size_t(sm_id)*sps+std::size_t(sp_id);}
    void change(std::size_t i,Cycle t,int dc,int dm){sp[i].time.change(t,dc,dm);sm[i/sps].change(t,dc,dm);gpu.change(t,dc,dm);}
    void advance(Cycle c){need(c>=frontier,"overlap callback time regressed");while(!ends.empty()&&ends.top().cycle<=c){const auto e=ends.top();ends.pop();change(e.slot,e.cycle,-1,0);++coverage.compute_subops_completed;}frontier=c;}
    bool compute_pipe(std::size_t i,const Pipeline* p){auto& u=sp[i];for(std::size_t k=0;k<u.pipe_count;++k)if(u.pipes[k].pipe==p)return u.pipes[k].compute;
        need(u.pipe_count<u.pipes.size(),"bounded overlap pipeline-tag registry exceeded");
        const bool yes=p->pipeline_name=="SIMD"||p->pipeline_name=="SFU"||p->pipeline_name=="SHFL";
        u.pipes[u.pipe_count++]={p,yes};return yes;}
};
inline thread_local std::unique_ptr<State> state;
inline thread_local bool enabled=false;
inline thread_local Cycle requested_start=0;
inline bool active()noexcept{return enabled;}
inline bool global_memory(OpType op)noexcept{return op==OpType::LD_DRAM2REG||op==OpType::ST_REG2DRAM||op==OpType::CP_DRAM2SRAM||op==OpType::CP_SRAM2DRAM;}
inline bool shared_memory(OpType op)noexcept{return op==OpType::LD_SRAM2REG||op==OpType::ST_REG2SRAM||op==OpType::LD_SRAM2REG_DSM||op==OpType::ST_REG2SRAM_DSM;}
inline bool bulk(OpType op)noexcept{return op==OpType::CP_DRAM2SRAM_TMA||op==OpType::CP_SRAM2DRAM_TMA||op==OpType::CP_DRAM2SRAM_LDGSTS;}
inline bool compute_op(OpType op)noexcept{return op==OpType::COMPUTE||op==OpType::OTHER;}
inline void begin(std::size_t sm_count,std::size_t sp_per_sm,Cycle start,bool enable){
    need(!enabled,"nested overlap collection is unsupported");need(start>=0,"negative overlap start");requested_start=start;
    if(!enable)return; // No allocation, reset, array walk or per-event state when off.
    need(sm_count>0&&sm_count<=1024&&sp_per_sm>0&&sp_per_sm<=32,"bounded overlap SM/SP geometry required");
    state=std::make_unique<State>(sm_count,sp_per_sm,start);enabled=true;
}
// Called once before the existing issue routine changes first-issue fields.
inline void on_first_issue(const DAGNode* n,int sp_id,Cycle c){
    if(!enabled)return;auto& s=*state;s.advance(c);auto& v=s.coverage;++v.first_node_issues;
    if(!s.placement(n->sm_id,sp_id)){++v.invalid_placement_nodes;return;}
    if(global_memory(n->op_type)){
        ++v.global_memory_nodes;need(s.memory_nodes.size()<max_live_events,"overlap memory live-node bound");const auto i=s.slot(n->sm_id,sp_id);
        need(s.memory_nodes.emplace(n,i).second,"duplicate memory first-issue interval");s.change(i,c,0,1);v.peak_live_memory_nodes=std::max<U>(v.peak_live_memory_nodes,s.memory_nodes.size());
    }else if(n->op_type==OpType::BARRIER)++v.barrier_nodes;
    else if(shared_memory(n->op_type))++v.shared_memory_nodes;
    else if(bulk(n->op_type))++v.unsupported_nodes;
    // Remaining normal nodes are classified from the actual selected pipeline.
}
// The finish value is read from the actual just-enqueued pipeline entry.
inline void on_pipeline_issue(const DAGNode* n,const Pipeline* p,int sp_id,int subop,Cycle issue,Cycle finish){
    if(!enabled)return;auto& s=*state;s.advance(issue);auto& v=s.coverage;
    if(!s.placement(n->sm_id,sp_id)){++v.excluded_pipeline_subop_issues;return;}
    if(global_memory(n->op_type)){++v.global_memory_subop_issues;return;}
    const bool preclassified=shared_memory(n->op_type)||n->op_type==OpType::BARRIER||bulk(n->op_type);
    const auto i=s.slot(n->sm_id,sp_id);const bool selected_compute=compute_op(n->op_type)&&s.compute_pipe(i,p);
    if(!selected_compute){++v.excluded_pipeline_subop_issues;if(subop==0&&!preclassified)++v.unsupported_nodes;return;}
    if(subop==0)++v.compute_nodes;
    need(finish>issue,"nonpositive actual compute pipeline interval");need(s.ends.size()<max_live_events,"overlap compute end-event bound");
    s.change(i,issue,1,0);s.sp[i].time.issue(issue);s.sm[i/s.sps].issue(issue);s.gpu.issue(issue);
    s.ends.push({finish,i,s.sequence++});++v.compute_subop_issues;v.peak_compute_end_events=std::max<U>(v.peak_compute_end_events,s.ends.size());
}
inline void on_node_complete(const DAGNode* n,int sp_id,Cycle c){
    if(!enabled)return;auto& s=*state;s.advance(c);
    if(!global_memory(n->op_type)||!s.placement(n->sm_id,sp_id))return;
    const auto it=s.memory_nodes.find(n);need(it!=s.memory_nodes.end(),"global memory completion without first issue");
    s.change(it->second,c,0,-1);s.memory_nodes.erase(it);++s.coverage.memory_nodes_completed;
}
inline J unit_json(Unit& u,Cycle end,U window){
    u.accrue(end);U sum=0;for(U x:u.bins)sum=add(sum,x);need(sum==window,"overlap four-state window does not close");
    return {{"compute_only_cycles",u.bins[1]},{"memory_only_cycles",u.bins[2]},{"both_cycles",u.bins[3]},{"neither_cycles",u.bins[0]},
        {"window_cycles",window},{"closed",true},{"compute_inflight_union_cycles",add(u.bins[1],u.bins[3])},{"memory_outstanding_union_cycles",add(u.bins[2],u.bins[3])},
        {"compute_issue_subop_events",u.compute_issue_events},{"cycles_with_compute_issue",u.compute_issue_cycles},
        {"peak_compute_inflight_subops",u.peak_compute},{"peak_memory_outstanding_nodes",u.peak_memory},{"final_compute_inflight_subops",u.compute},{"final_memory_outstanding_nodes",u.memory}};
}
inline J finish(Cycle end){
    need(end>=requested_start,"overlap end before start");
    if(!enabled)return {{"schema","GTSIM_CYCLE_OVERLAP_V1"},{"enabled",false},{"start_cycle",requested_start},{"kernel_end_cycle",end},{"window_cycles",end-requested_start},{"state_scanned",false}};
    auto& s=*state;s.advance(end);enabled=false;const U window=U(end-s.start);
    need(s.ends.empty()&&s.memory_nodes.empty()&&s.gpu.compute==0&&s.gpu.memory==0,"kernel overlap has unclosed inflight work");
    const auto& v=s.coverage;
    need(v.first_node_issues==v.compute_nodes+v.global_memory_nodes+v.barrier_nodes+v.shared_memory_nodes+v.unsupported_nodes+v.invalid_placement_nodes,"overlap node coverage does not close");
    need(v.compute_subop_issues==v.compute_subops_completed&&v.global_memory_nodes==v.memory_nodes_completed,"overlap event conservation differs");
    J sm=J::array(),sp=J::array();std::array<U,4> pooled_sm{},pooled_sp{};
    for(std::size_t i=0;i<s.sms;++i){auto j=unit_json(s.sm[i],end,window);j["sm_id"]=i;sm.push_back(std::move(j));for(std::size_t k=0;k<4;++k)pooled_sm[k]=add(pooled_sm[k],s.sm[i].bins[k]);}
    for(std::size_t i=0;i<s.sp.size();++i){auto j=unit_json(s.sp[i].time,end,window);j["sm_id"]=i/s.sps;j["sp_id"]=i%s.sps;sp.push_back(std::move(j));for(std::size_t k=0;k<4;++k)pooled_sp[k]=add(pooled_sp[k],s.sp[i].time.bins[k]);}
    auto pool=[](const std::array<U,4>& x,U denominator){U sum=0;for(U y:x)sum=add(sum,y);need(sum==denominator,"pooled overlap denominator does not close");return J{{"compute_only_cycles",x[1]},{"memory_only_cycles",x[2]},{"both_cycles",x[3]},{"neither_cycles",x[0]},{"population_cycles",denominator},{"closed",true}};};
    J cov={{"first_node_issues",v.first_node_issues},{"supported_compute_nodes",v.compute_nodes},{"supported_global_memory_nodes",v.global_memory_nodes},{"excluded_barrier_nodes",v.barrier_nodes},{"excluded_shared_memory_nodes",v.shared_memory_nodes},{"unsupported_nodes",v.unsupported_nodes},{"invalid_placement_nodes",v.invalid_placement_nodes},
        {"compute_subop_issues",v.compute_subop_issues},{"compute_subops_completed",v.compute_subops_completed},{"global_memory_subop_issues",v.global_memory_subop_issues},{"excluded_pipeline_subop_issues",v.excluded_pipeline_subop_issues},{"memory_nodes_completed",v.memory_nodes_completed},{"peak_compute_end_events",v.peak_compute_end_events},{"peak_live_memory_nodes",v.peak_live_memory_nodes},{"node_coverage_closed",true}};
    return {{"schema","GTSIM_CYCLE_OVERLAP_V1"},{"enabled",true},{"start_cycle",s.start},{"kernel_end_cycle",end},{"window_cycles",window},
        {"interval_convention","half-open [issue, completion); clipped to [start_cycle, kernel_end_cycle)"},
        {"compute_definition","actual SIMD/SFU/SHFL subop pipeline-inflight, from existing issue to scheduled pipeline completion; not execution-unit busy"},
        {"memory_definition","global memory node first issue to actual final node completion, including LS/retry/cache/return; not physical DRAM busy"},
        {"neither_definition","neither of the two tracked classes; not GPU idle"},{"gpu_union_definition","any supported compute anywhere AND any global memory anywhere; overlap may span different SMs"},
        {"issue_count_definition","separate actual compute subop issue events and cycles with >=1 issue; not inflight duration"},
        {"event_mode_accounting","advance only through actual current callback/finish frontier; mature scheduled compute ends in timestamp order; no host-call-to-cycle conversion"},
        {"drain_included",false},{"shared_memory_work_added",false},{"full_trace_saved",false},{"per_event_full_state_scan",false},
        {"gpu",unit_json(s.gpu,end,window)},{"per_sm",sm},{"per_sp",sp},{"pooled_sm_time",pool(pooled_sm,product(s.sms,window))},{"pooled_sp_time",pool(pooled_sp,product(s.sp.size(),window))},{"coverage",cov}};
}
} // namespace GTSim::cycle_overlap
