#pragma once
// SOURCE-ONLY component. This compiles a shared phase partition; no memory or
// Runtime execution is implemented here. Parent remains the frozen cache q16.
// Parent packet_runtime types are defined by runtime.h before this header.
#include <map>

namespace memory_phase {
namespace rt = packet_runtime;
using U = std::uint64_t;
enum class SourceKind { Other, SimdCompute, GlobalRead };
// Derived from the original SourceNode, never inferred from a memory address.
struct SourceFact { unsigned member=0, warp=0; SourceKind kind=SourceKind::Other; };
enum class Milestone { OriginalIssue, OriginalCompletion, IssueTail, ComputeTail, ReadReturnsTail };
struct VirtualCompute {
    unsigned original_group=0, source_member=0;
    rt::Cycle dispatch=0, reservation_end=0, completion=0;
};
struct Checkpoint {
    unsigned original_group=0, source_member=0;
    rt::Cycle earliest_dispatch=0;
    // Newly traversed computes since previous memory checkpoint. Immutable,
    // not a list of dynamic per-CTA states. Their complete times may be later
    // than this checkpoint. Retrying must not replay this prefix.
    unsigned compute_begin=0, compute_end=0;
};
struct Unit {
    unsigned warp=0;
    bool phase=false;
    std::vector<unsigned> original_groups;
    std::vector<VirtualCompute> compute;
    std::vector<Checkpoint> checkpoints;
    rt::Cycle isolated_issue_tail=0, isolated_compute_tail=0, isolated_issue_end=0;
    U logical_members=0, compute_elements=0, original_internal_edges=0;
};
struct Gate {
    unsigned source=0, target=0;
    Milestone milestone=Milestone::OriginalCompletion;
    rt::EdgeType original_type=rt::EdgeType::Completion;
    U original_multiplicity=0;
};
struct Plan {
    std::shared_ptr<const rt::Program> original;
    unsigned width=1;
    bool use_legacy_runtime=true;
    std::vector<Unit> units;
    std::vector<unsigned> original_group_to_unit;
    std::vector<std::vector<unsigned>> warp_units;
    std::vector<Gate> gates;
    U phase_count=0, original_groups_folded=0;
    U original_logical_members=0, original_logical_edges=0;
    U internal_edges_newly_compiled=0, external_edge_multiplicity=0;
};
inline Milestone milestone_for(bool phase,rt::GroupKind source_kind,rt::EdgeType type) {
    if(!phase)return type==rt::EdgeType::Issue?Milestone::OriginalIssue:Milestone::OriginalCompletion;
    if(type==rt::EdgeType::Issue)return Milestone::IssueTail;
    return source_kind==rt::GroupKind::GlobalSingleton?Milestone::ReadReturnsTail:Milestone::ComputeTail;
}
inline Plan make_plan(std::shared_ptr<const rt::Program> p,
                      const std::vector<SourceFact>& facts,unsigned width) {
    rt::require(p && (width==1||width==4||width==16),"phase width 1/4/16");
    rt::require(p->outgoing.size()==p->groups.size() && p->indegree.size()==p->groups.size(),"finalized original program required");
    rt::require(facts.size()==p->logical_members && facts.size()<=std::numeric_limits<unsigned>::max(),"phase source fact coverage");
    for(unsigned i=0;i<facts.size();++i)rt::require(facts[i].member==i,"dense source facts");
    Plan out;out.original=p;out.width=width;out.original_logical_members=p->logical_members;
    out.original_logical_edges=rt::add(p->logical_edges,p->internal_logical_edges);
    const auto count=p->groups.size();out.original_group_to_unit.resize(count);
    std::vector<std::vector<unsigned>> incoming(count);
    std::vector<bool> cross(count,false),read_input(count,false);
    for(unsigned e=0;e<p->edges.size();++e) {
        const auto& edge=p->edges[e];incoming[edge.target].push_back(e);
        if(p->groups[edge.source].warp!=p->groups[edge.target].warp)cross[edge.source]=cross[edge.target]=true;
        if(edge.type==rt::EdgeType::Completion && p->groups[edge.source].kind==rt::GroupKind::GlobalSingleton)
            read_input[edge.target]=true;
    }
    auto eligible=[&](unsigned id) {
        const auto& g=p->groups[id];
        if(cross[id]||read_input[id]||g.members.size()!=1)return false;
        const auto& f=facts[g.members[0].id];rt::require(f.warp==g.warp,"source/group warp");
        if(g.kind==rt::GroupKind::GlobalSingleton)
            return f.kind==SourceKind::GlobalRead && g.external_kind==1;
        return g.kind==rt::GroupKind::ComputeSingleton && f.kind==SourceKind::SimdCompute &&
            g.pipeline==0 && g.subops==1 && g.issue_span>0;
    };
    auto is_read=[&](unsigned id){return p->groups[id].kind==rt::GroupKind::GlobalSingleton;};
    // Only partitioning is new. A width-1 driver MUST invoke original Runtime
    // directly, not replay this metadata through a new scheduling code path.
    std::vector<std::vector<unsigned>> partition;
    if(width==1) {
        for(unsigned id=0;id<count;++id)partition.push_back({id});
    } else for(const auto& warp:p->warp_groups) {
        for(std::size_t i=0;i<warp.size();) {
            std::size_t stop=i;unsigned reads=0;
            if(eligible(warp[i])&&is_read(warp[i])) {
                for(std::size_t j=i;j<warp.size()&&j-i<64;++j) {
                    if(!eligible(warp[j]))break;
                    if(is_read(warp[j])) {++reads;stop=j+1;if(reads==width)break;}
                }
            }
            if(reads>=2) {partition.emplace_back(warp.begin()+i,warp.begin()+stop);i=stop;}
            else {partition.push_back({warp[i]});++i;}
        }
        // Original dense member order is the canonical unit numbering. Warp
        // sequence is reconstructed separately, never guessed from this sort.
        std::sort(partition.begin(),partition.end(),[](const auto& a,const auto& b){return a.front()<b.front();});
    }
    std::vector<bool> seen(count,false);
    U members=0;
    for(unsigned id=0;id<partition.size();++id) {
        Unit u;u.original_groups=partition[id];u.phase=u.original_groups.size()>1;
        u.warp=p->groups[u.original_groups.front()].warp;
        std::map<unsigned,std::pair<rt::Cycle,rt::Cycle>> local_times; // dispatch, compute completion
        rt::Cycle next_issue=0,simd_free=0,last_compute=0;
        unsigned compute_prefix=0;
        for(auto old:u.original_groups) {
            rt::require(old<count&&!seen[old]&&p->groups[old].warp==u.warp,"original group coverage");
            seen[old]=true;out.original_group_to_unit[old]=id;const auto& g=p->groups[old];
            u.logical_members=rt::add(u.logical_members,g.members.size());
            u.compute_elements=rt::add(u.compute_elements,g.modeled_compute_elements);
            if(!u.phase)continue;
            rt::require(eligible(old),"phase member eligibility");
            rt::Cycle at=next_issue;
            if(!is_read(old))at=std::max(at,simd_free);
            for(auto index:incoming[old]) {
                const auto& edge=p->edges[index];auto found=local_times.find(edge.source);
                if(found==local_times.end())continue; // external gate is waited at entry
                if(edge.type==rt::EdgeType::Completion)rt::require(!is_read(edge.source),"unknown read return inside phase");
                at=std::max(at,rt::add(edge.type==rt::EdgeType::Issue?found->second.first:found->second.second,1));
            }
            if(is_read(old)) {
                u.checkpoints.push_back({old,g.members[0].id,at,compute_prefix,unsigned(u.compute.size())});
                compute_prefix=unsigned(u.compute.size());local_times.emplace(old,std::make_pair(at,0));
            } else {
                auto reserve=rt::add(at,g.issue_span);auto done=rt::add(reserve,g.latency);
                u.compute.push_back({old,g.members[0].id,at,reserve,done});simd_free=reserve;
                last_compute=std::max(last_compute,done);local_times.emplace(old,std::make_pair(at,done));
            }
            next_issue=rt::add(at,1);
        }
        if(u.phase) {
            rt::require(u.checkpoints.size()>=2&&u.checkpoints.size()<=width&&
                is_read(u.original_groups.front())&&is_read(u.original_groups.back()),"phase read boundaries");
            rt::require(u.checkpoints.front().earliest_dispatch==0,"entry memory checkpoint");
            u.isolated_issue_tail=u.checkpoints.back().earliest_dispatch;
            u.isolated_compute_tail=std::max(last_compute,u.isolated_issue_tail);
            u.isolated_issue_end=rt::add(u.isolated_issue_tail,1);
            ++out.phase_count;out.original_groups_folded=rt::add(out.original_groups_folded,u.original_groups.size());
        }
        members=rt::add(members,u.logical_members);out.units.push_back(std::move(u));
    }
    rt::require(members==p->logical_members&&std::all_of(seen.begin(),seen.end(),[](bool x){return x;}),"phase member closure");
    out.warp_units.resize(p->warp_groups.size());
    for(unsigned w=0;w<p->warp_groups.size();++w)for(auto old:p->warp_groups[w]) {
        auto id=out.original_group_to_unit[old];auto& warp=out.warp_units[w];
        if(warp.empty()||warp.back()!=id)warp.push_back(id);
    }
    using Key=std::tuple<unsigned,unsigned,Milestone,rt::EdgeType>;
    std::map<Key,U> merged;
    for(const auto& edge:p->edges) {
        auto a=out.original_group_to_unit[edge.source],b=out.original_group_to_unit[edge.target];
        if(a==b) {
            rt::require(out.units[a].phase,"self edge in unchanged original group");
            out.internal_edges_newly_compiled=rt::add(out.internal_edges_newly_compiled,edge.logical_multiplicity);
            out.units[a].original_internal_edges=rt::add(out.units[a].original_internal_edges,edge.logical_multiplicity);
        } else {
            auto m=milestone_for(out.units[a].phase,p->groups[edge.source].kind,edge.type);
            out.external_edge_multiplicity=rt::add(out.external_edge_multiplicity,edge.logical_multiplicity);
            if(width==1)out.gates.push_back({a,b,m,edge.type,edge.logical_multiplicity});
            else {auto& value=merged[{a,b,m,edge.type}];value=rt::add(value,edge.logical_multiplicity);}
        }
    }
    for(const auto& [key,value]:merged)out.gates.push_back({std::get<0>(key),std::get<1>(key),std::get<2>(key),std::get<3>(key),value});
    rt::require(rt::add(out.internal_edges_newly_compiled,out.external_edge_multiplicity)==p->logical_edges,"phase typed edge multiplicity closure");
    // Actual quotient closure includes implicit per-warp issue-cursor order.
    std::vector<std::vector<unsigned>> children(out.units.size());std::vector<U> degree(out.units.size());
    auto edge=[&](unsigned a,unsigned b){rt::require(a!=b,"unit self cycle");children[a].push_back(b);degree[b]=rt::add(degree[b],1);};
    for(const auto& g:out.gates)edge(g.source,g.target);
    for(const auto& warp:out.warp_units)for(unsigned i=1;i<warp.size();++i)edge(warp[i-1],warp[i]);
    std::queue<unsigned> ready;for(unsigned i=0;i<degree.size();++i)if(!degree[i])ready.push(i);
    std::size_t visited=0;while(!ready.empty()){auto a=ready.front();ready.pop();++visited;for(auto b:children[a])if(--degree[b]==0)ready.push(b);}
    rt::require(visited==out.units.size(),"phase quotient cycle; reject partition, never remove dependencies");
    out.use_legacy_runtime=out.phase_count==0;return out;
}
} // namespace memory_phase
