#pragma once
// Static structure only. No Runtime, memory service, event queue or times are executed.
// Include the qualified packet_runtime Program definition before this header.
// This avoids importing a second Runtime definition through a different VFS name.
#include <map>

namespace prefill_compute_phase {
namespace rt = packet_runtime;
using U = std::uint64_t;
using Id = std::uint32_t;
enum class SourceKind { Compute, Tensor, Control, Global, Shared, AsyncCopy, Barrier };
enum class BoundaryReason { None, Control, Global, Shared, AsyncCopy, Barrier, UnsupportedComputeProfile };
struct SourceFact {
    Id id=0, warp=0;
    SourceKind kind=SourceKind::Compute;
    U compute_elements=0, tensor_fma=0;
};
// Offsets here are the unchanged source GROUP's profile relative to its own issue.
// They are not calculated phase times and have no timing-oracle qualification.
struct ResourceStep {
    Id group=0, pipeline=0;
    bool packet=false;
    U sp_next_issue_offset=0, pipeline_occupied_until_offset=0;
    U issue_release_offset=0, completion_offset=0;
};
struct SegmentRecipe {
    Id warp=0;
    std::vector<Id> groups; // original warp_groups order, never numeric sorting
    BoundaryReason hard_boundary=BoundaryReason::None;
    bool cycle_fallback=false;
    std::vector<Id> internal_edge_indices; // original edge order
    std::vector<ResourceStep> resource_steps;
    U logical_members=0, compute_elements=0, tensor_fma=0;
    // Deliberately unknown until a separately qualified resource recurrence exists.
    std::optional<U> sp_next_issue, issue_tail, completion_tail, retirement_tail;
    std::vector<std::optional<U>> pipeline_free;
    bool timing_oracle_qualified=false;
};
struct Gate {
    Id source=0, target=0;
    rt::EdgeType type=rt::EdgeType::Completion;
    std::vector<Id> original_edge_indices;
    U logical_multiplicity=0;
};
struct Statistics {
    U original_groups=0, original_members=0, compute_elements=0, tensor_fma=0;
    U eligible_groups=0, hard_boundary_groups=0;
    U initial_multi_segments=0, initial_groups_in_multi_segments=0;
    U final_multi_segments=0, final_groups_in_multi_segments=0;
    U cycle_fallback_rounds=0, cyclic_SCC_visits=0;
    U fallback_multi_segments=0, fallback_original_groups=0;
    U original_typed_group_edges=0, original_external_logical_edges=0;
    U preexisting_packet_internal_logical_edges=0;
    U new_internal_typed_group_edges=0, new_internal_logical_edges=0;
    U cross_original_typed_group_edges=0, cross_logical_edges=0;
    U aggregate_typed_gates=0, cross_edge_aggregation_count=0;
    std::map<Id,U> final_segment_group_histogram;
    std::map<BoundaryReason,U> hard_boundary_counts;
};
struct Plan {
    std::shared_ptr<const rt::Program> source; // owns all immutable source profiles/members
    unsigned width=1;
    std::vector<SegmentRecipe> segments;
    std::vector<Id> group_to_segment;
    std::vector<std::vector<Id>> warp_segments;
    std::vector<Gate> gates; // first contributing original edge order
    Statistics stats;
    bool typed_and_cursor_DAG_checked=false;
    bool width1_identity_checked=false;
    bool timing_oracle_qualified=false, runtime_ready=false;
};
namespace detail {
inline void need(bool b,const char* why){rt::require(b,why);}
inline BoundaryReason reason(const rt::Group& g,const std::vector<SourceFact>& facts) {
    SourceKind kind=facts.at(g.members.front().id).kind;
    for(const auto& m:g.members)need(facts.at(m.id).kind==kind,"mixed source kind in imported group");
    switch(kind) {
    case SourceKind::Compute:
        need((g.kind==rt::GroupKind::ComputePacket||g.kind==rt::GroupKind::ComputeSingleton)&&g.modeled_tensor_fma==0,"Compute source/group binding");
        return g.pipeline<3&&(g.kind==rt::GroupKind::ComputePacket||g.subops==1)?BoundaryReason::None:BoundaryReason::UnsupportedComputeProfile;
    case SourceKind::Tensor:
        need(g.kind==rt::GroupKind::ComputeSingleton&&g.members.size()==1&&g.modeled_compute_elements==0&&g.modeled_tensor_fma>0,"Tensor source/group binding");
        return g.pipeline==5&&g.subops==1?BoundaryReason::None:BoundaryReason::UnsupportedComputeProfile;
    case SourceKind::Control:need(g.kind==rt::GroupKind::ControlSingleton,"Control source/group binding");return BoundaryReason::Control;
    case SourceKind::Global:need(g.kind==rt::GroupKind::GlobalSingleton,"Global source/group binding");return BoundaryReason::Global;
    case SourceKind::Shared:need(g.kind==rt::GroupKind::SharedSingleton,"Shared source/group binding");return BoundaryReason::Shared;
    case SourceKind::AsyncCopy:need(g.kind==rt::GroupKind::ExternalSingleton,"Async source/group binding");return BoundaryReason::AsyncCopy;
    case SourceKind::Barrier:need(g.kind==rt::GroupKind::BarrierSingleton,"Barrier source/group binding");return BoundaryReason::Barrier;
    }
    throw std::invalid_argument("unknown SourceKind");
}
inline ResourceStep step(Id id,const rt::Group& g) {
    ResourceStep s;s.group=id;s.pipeline=g.pipeline;s.packet=g.kind==rt::GroupKind::ComputePacket;
    if(s.packet) {
        s.sp_next_issue_offset=g.reservation_end;s.pipeline_occupied_until_offset=g.reservation_end;
        s.issue_release_offset=g.issue_release;s.completion_offset=g.completion;
    } else {
        need(g.kind==rt::GroupKind::ComputeSingleton&&g.subops==1,"single compute recurrence profile");
        s.sp_next_issue_offset=1;s.pipeline_occupied_until_offset=g.issue_span;
        s.issue_release_offset=0;s.completion_offset=rt::add(g.issue_span,g.latency);
    }
    return s;
}
inline void remap(Plan& out) {
    // This is only a stable ID assignment. Within each segment the original warp
    // order is preserved. Width1 necessarily produces original group IDs.
    std::sort(out.segments.begin(),out.segments.end(),[](const auto& a,const auto& b){
        return *std::min_element(a.groups.begin(),a.groups.end())<*std::min_element(b.groups.begin(),b.groups.end());});
    out.group_to_segment.assign(out.source->groups.size(),std::numeric_limits<Id>::max());
    for(Id s=0;s<out.segments.size();++s)for(Id g:out.segments[s].groups) {
        need(g<out.group_to_segment.size()&&out.group_to_segment[g]==std::numeric_limits<Id>::max(),"group partition unique");
        need(out.source->groups[g].warp==out.segments[s].warp,"segment same warp");out.group_to_segment[g]=s;
    }
    out.warp_segments.assign(out.source->warp_groups.size(),{});
    std::vector<unsigned> occurrences(out.segments.size());
    for(Id w=0;w<out.source->warp_groups.size();++w) {
        auto& dest=out.warp_segments[w];std::vector<Id> rebuilt;
        for(Id g:out.source->warp_groups[w]) {
            Id s=out.group_to_segment[g];need(s<out.segments.size(),"complete group partition");
            if(dest.empty()||dest.back()!=s){dest.push_back(s);++occurrences[s];}
        }
        for(Id s:dest)rebuilt.insert(rebuilt.end(),out.segments[s].groups.begin(),out.segments[s].groups.end());
        need(rebuilt==out.source->warp_groups[w],"contiguous original warp order");
    }
    for(auto n:occurrences)need(n==1,"one contiguous segment interval");
}
struct Graph {std::vector<std::vector<Id>> children,parents;};
inline Graph graph(const Plan& out) {
    Graph g;g.children.resize(out.segments.size());g.parents.resize(out.segments.size());
    auto add=[&](Id a,Id b){if(a!=b){g.children[a].push_back(b);g.parents[b].push_back(a);}};
    for(const auto& e:out.source->edges)add(out.group_to_segment[e.source],out.group_to_segment[e.target]);
    for(const auto& w:out.warp_segments)for(std::size_t i=1;i<w.size();++i)add(w[i-1],w[i]);
    return g; // typed kinds and cursor may share endpoints; parallel edges are kept
}
inline std::vector<std::vector<Id>> cyclic_components(const Graph& g) {
    const auto n=g.children.size();std::vector<unsigned char> seen(n);std::vector<Id> order;
    // Iterative Kosaraju: bounded by source graph size, no recursive C++ stack.
    for(Id root=0;root<n;++root)if(!seen[root]) {
        std::vector<std::pair<Id,std::size_t>> stack{{root,0}};seen[root]=1;
        while(!stack.empty()) {
            auto& top=stack.back();const Id at=top.first;
            if(top.second<g.children[at].size()) {
                const Id next=g.children[at][top.second++];
                if(!seen[next]){seen[next]=1;stack.emplace_back(next,0);}
            } else {order.push_back(at);stack.pop_back();}
        }
    }
    std::fill(seen.begin(),seen.end(),0);std::vector<std::vector<Id>> cyclic;
    for(auto it=order.rbegin();it!=order.rend();++it)if(!seen[*it]) {
        std::vector<Id> stack{*it},component;seen[*it]=1;
        while(!stack.empty()) {
            Id at=stack.back();stack.pop_back();component.push_back(at);
            for(Id next:g.parents[at])if(!seen[next]){seen[next]=1;stack.push_back(next);}
        }
        if(component.size()>1){std::sort(component.begin(),component.end());cyclic.push_back(std::move(component));}
    }
    std::sort(cyclic.begin(),cyclic.end(),[](const auto& a,const auto& b){return a.front()<b.front();});
    return cyclic;
}
inline void check_DAG(const Graph& g) {
    std::vector<std::size_t> indegree;for(const auto& p:g.parents)indegree.push_back(p.size());
    std::queue<Id> q;for(Id s=0;s<indegree.size();++s)if(!indegree[s])q.push(s);
    U visits=0;while(!q.empty()){Id s=q.front();q.pop();++visits;for(Id t:g.children[s])if(--indegree[t]==0)q.push(t);}
    need(visits==g.children.size(),"contracted typed plus warp graph acyclic");
}
} // detail

inline Plan make_plan(std::shared_ptr<const rt::Program> source,const std::vector<SourceFact>& facts,unsigned width) {
    using namespace detail;
    need(source!=nullptr&&(width==1||width==4||width==16),"source and closed phase widths");
    need(source->groups.size()<=200000&&source->edges.size()<=2000000,"bounded static planner domain");
    // Static qualification copies and validates, never mutates the published Program.
    {rt::Program checked=*source;checked.finalize();need(checked.outgoing==source->outgoing&&checked.indegree==source->indegree,"finalized source indexes");}
    need(source->pipeline_count==6&&facts.size()==source->logical_members&&facts.size()<=std::numeric_limits<Id>::max(),"source facts/resource shape");
    for(Id i=0;i<facts.size();++i)need(facts[i].id==i,"dense source fact identity");
    Plan out;out.source=std::move(source);out.width=width;auto& stats=out.stats;
    stats.original_groups=out.source->groups.size();stats.original_members=out.source->logical_members;
    stats.original_typed_group_edges=out.source->edges.size();stats.original_external_logical_edges=out.source->logical_edges;
    stats.preexisting_packet_internal_logical_edges=out.source->internal_logical_edges;
    std::vector<BoundaryReason> reasons;
    for(const auto& group:out.source->groups) {
        U elements=0,fma=0;for(const auto& m:group.members) {
            const auto& f=facts.at(m.id);need(f.warp==group.warp,"source fact warp");
            elements=rt::add(elements,f.compute_elements);fma=rt::add(fma,f.tensor_fma);
        }
        need(elements==group.modeled_compute_elements&&fma==group.modeled_tensor_fma,"group source work/FMA closure");
        stats.compute_elements=rt::add(stats.compute_elements,elements);stats.tensor_fma=rt::add(stats.tensor_fma,fma);
        reasons.push_back(reason(group,facts));
        if(reasons.back()==BoundaryReason::None)++stats.eligible_groups;
        else{++stats.hard_boundary_groups;++stats.hard_boundary_counts[reasons.back()];}
    }
    for(Id w=0;w<out.source->warp_groups.size();++w) {
        const auto& warp=out.source->warp_groups[w];
        for(std::size_t at=0;at<warp.size();) {
            SegmentRecipe seg;seg.warp=w;seg.hard_boundary=reasons[warp[at]];
            seg.groups.push_back(warp[at++]);
            while(seg.hard_boundary==BoundaryReason::None&&seg.groups.size()<width&&at<warp.size()&&reasons[warp[at]]==BoundaryReason::None)
                seg.groups.push_back(warp[at++]);
            if(seg.groups.size()>1){++stats.initial_multi_segments;stats.initial_groups_in_multi_segments=rt::add(stats.initial_groups_in_multi_segments,seg.groups.size());}
            out.segments.push_back(std::move(seg));
        }
    }
    for(;;) {
        remap(out);const auto cyclic=cyclic_components(graph(out));if(cyclic.empty())break;
        ++stats.cycle_fallback_rounds;stats.cyclic_SCC_visits=rt::add(stats.cyclic_SCC_visits,cyclic.size());
        std::vector<bool> split(out.segments.size());U progress=0;
        for(const auto& component:cyclic) {
            bool can_split=false;
            for(Id s:component)if(out.segments[s].groups.size()>1){split[s]=true;can_split=true;}
            need(can_split,"singleton cycle contradicts finalized source graph");
        }
        std::vector<SegmentRecipe> next;
        for(Id s=0;s<out.segments.size();++s) {
            auto& seg=out.segments[s];
            if(!split[s]){next.push_back(std::move(seg));continue;}
            ++progress;++stats.fallback_multi_segments;
            stats.fallback_original_groups=rt::add(stats.fallback_original_groups,seg.groups.size());
            for(Id g:seg.groups){SegmentRecipe one;one.warp=seg.warp;one.groups={g};one.hard_boundary=reasons[g];one.cycle_fallback=true;next.push_back(std::move(one));}
        }
        need(progress>0&&stats.cycle_fallback_rounds<=stats.initial_multi_segments,"finite deterministic cycle fallback");
        out.segments=std::move(next);
    }
    check_DAG(graph(out));out.typed_and_cursor_DAG_checked=true;
    std::map<std::tuple<Id,Id,unsigned>,Id> gate_index;
    std::vector<unsigned> edge_seen(out.source->edges.size());
    for(Id i=0;i<out.source->edges.size();++i) {
        const auto& e=out.source->edges[i];Id a=out.group_to_segment[e.source],b=out.group_to_segment[e.target];
        if(a==b) {
            auto& seg=out.segments[a];need(seg.groups.size()>1&&seg.hard_boundary==BoundaryReason::None,"only merged compute edge internalized");
            need(std::find(seg.groups.begin(),seg.groups.end(),e.source)<std::find(seg.groups.begin(),seg.groups.end(),e.target),"internal typed edge follows warp cursor");
            seg.internal_edge_indices.push_back(i);++stats.new_internal_typed_group_edges;
            stats.new_internal_logical_edges=rt::add(stats.new_internal_logical_edges,e.logical_multiplicity);
        } else {
            auto key=std::make_tuple(a,b,unsigned(e.type));auto found=gate_index.find(key);
            if(found==gate_index.end()) {
                const Id idx=Id(out.gates.size());gate_index.emplace(key,idx);
                Gate gate;gate.source=a;gate.target=b;gate.type=e.type;out.gates.push_back(std::move(gate));found=gate_index.find(key);
            }
            auto& gate=out.gates[found->second];gate.original_edge_indices.push_back(i);
            gate.logical_multiplicity=rt::add(gate.logical_multiplicity,e.logical_multiplicity);
            ++stats.cross_original_typed_group_edges;stats.cross_logical_edges=rt::add(stats.cross_logical_edges,e.logical_multiplicity);
        }
    }
    U members=0,elements=0,fma=0;
    for(auto& seg:out.segments) {
        ++stats.final_segment_group_histogram[Id(seg.groups.size())];seg.pipeline_free.resize(out.source->pipeline_count);
        if(seg.groups.size()>1){++stats.final_multi_segments;stats.final_groups_in_multi_segments=rt::add(stats.final_groups_in_multi_segments,seg.groups.size());}
        if(seg.hard_boundary!=BoundaryReason::None)need(seg.groups.size()==1,"hard boundary retained");
        for(Id g:seg.groups) {
            const auto& original=out.source->groups[g];seg.logical_members=rt::add(seg.logical_members,original.members.size());
            seg.compute_elements=rt::add(seg.compute_elements,original.modeled_compute_elements);seg.tensor_fma=rt::add(seg.tensor_fma,original.modeled_tensor_fma);
            if(seg.hard_boundary==BoundaryReason::None)seg.resource_steps.push_back(step(g,original));
        }
        for(Id i:seg.internal_edge_indices)++edge_seen[i];
        members=rt::add(members,seg.logical_members);elements=rt::add(elements,seg.compute_elements);fma=rt::add(fma,seg.tensor_fma);
    }
    for(const auto& gate:out.gates)for(Id i:gate.original_edge_indices)++edge_seen[i];
    for(unsigned n:edge_seen)need(n==1,"each original typed edge exactly once");
    stats.aggregate_typed_gates=out.gates.size();
    need(stats.cross_original_typed_group_edges>=stats.aggregate_typed_gates,"gate aggregation bound");
    stats.cross_edge_aggregation_count=stats.cross_original_typed_group_edges-stats.aggregate_typed_gates;
    need(rt::add(stats.new_internal_typed_group_edges,stats.cross_original_typed_group_edges)==stats.original_typed_group_edges,"typed group edge partition");
    need(rt::add(stats.new_internal_logical_edges,stats.cross_logical_edges)==stats.original_external_logical_edges,"typed logical multiplicity partition");
    need(members==stats.original_members&&elements==stats.compute_elements&&fma==stats.tensor_fma,"segment work/FMA conservation");
    if(width==1) {
        need(out.segments.size()==out.source->groups.size()&&out.warp_segments==out.source->warp_groups&&out.gates.size()==out.source->edges.size(),"width1 original indexes");
        for(Id g=0;g<out.source->groups.size();++g)need(out.group_to_segment[g]==g&&out.segments[g].groups==std::vector<Id>{g},"width1 group identity");
        for(Id i=0;i<out.source->edges.size();++i) {
            const auto& a=out.gates[i];const auto& b=out.source->edges[i];
            need(a.source==b.source&&a.target==b.target&&a.type==b.type&&a.logical_multiplicity==b.logical_multiplicity&&a.original_edge_indices==std::vector<Id>{i},"width1 original edge ordering");
        }
        out.width1_identity_checked=true;
    }
    return out;
}
} // namespace prefill_compute_phase
