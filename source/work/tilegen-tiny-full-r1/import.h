#pragma once
#include "source.h"
#include <map>

namespace tiny_full {
inline unsigned pipeline(const std::string& s) {
    if(s=="SIMD")return 0;if(s=="SFU")return 1;if(s=="SHFL")return 2;
    if(s=="LD")return 3;if(s=="ST")return 4;if(s=="Tensor")return 5;
    throw std::invalid_argument("unknown tiny pipeline");
}
inline rt::Group singleton(const SourceNode& n,const g::SimulatorConfig& cfg) {
    rt::Group a;a.warp=n.warp;a.members.push_back({n.id,0,0,0});
    a.external_descriptor=n.id;a.modeled_compute_elements=n.compute_elements;
    a.modeled_tensor_fma=n.tensor_fma;
    switch(n.kind) {
    case Kind::Compute: case Kind::Control: {
        a.kind=n.kind==Kind::Control?rt::GroupKind::ControlSingleton:rt::GroupKind::ComputeSingleton;
        a.pipeline=pipeline(n.pipeline);p::need(a.pipeline<3,"scalar compute resource");
        int throughput=a.pipeline==0?cfg.simd_throughput:a.pipeline==1?cfg.sfu_throughput:(1<<30);
        int width=a.pipeline==0?cfg.simd_width:a.pipeline==1?cfg.sfu_width:(1<<30);
        int latency=a.pipeline==0?cfg.simd_latency_cycles:a.pipeline==1?cfg.sfu_latency_cycles:cfg.shfl_latency_cycles;
        p::need(n.compute_elements>0&&throughput>0&&width>0&&latency>=0,"scalar work profile");
        a.subops=unsigned((n.compute_elements+U(throughput)-1)/U(throughput));
        a.issue_span=(U(throughput)+U(width)-1)/U(width);a.latency=U(latency);break;
    }
    case Kind::Tensor:
        p::need(cfg.tensor_issue_work_semantics==g::TensorIssueWorkSemantics::DECLARED_FMA_WORK&&
            n.tensor_fma>0&&n.compute_elements==0&&cfg.tensor_core_width>0&&cfg.tensor_core_latency_cycles>=0,
            "ordinary MMA requires declared FMA work");
        a.kind=rt::GroupKind::ComputeSingleton;a.pipeline=5;a.subops=1;
        a.issue_span=(n.tensor_fma+U(cfg.tensor_core_width)-1)/U(cfg.tensor_core_width);
        a.latency=U(cfg.tensor_core_latency_cycles);break;
    case Kind::Barrier:
        a.kind=rt::GroupKind::BarrierSingleton;a.barrier_latency=1;break;
    case Kind::Global: case Kind::Shared: case Kind::AsyncCopy:
        a.kind=n.kind==Kind::Global?rt::GroupKind::GlobalSingleton:n.kind==Kind::Shared?
            rt::GroupKind::SharedSingleton:rt::GroupKind::ExternalSingleton;
        a.external_kind=n.kind==Kind::Global?1:n.kind==Kind::Shared?2:3;
        a.pipeline=n.write?4:3;break;
    }
    return a;
}

// Same bounded scalar packet recurrence as the original independent compiler.
// Tensor and all memory/control/barrier members remain individual operations.
inline std::shared_ptr<const rt::Program> import_program(const KernelBinding& binding,U cta,
        unsigned q,const g::SimulatorConfig& cfg) {
    p::need(q==1||q==2||q==4||q==8||q==16,"tiny packet quantum");
    const auto nodes=binding.nodes(cta);
    p::need(!nodes.empty()&&nodes.size()<=UINT32_MAX,"tiny source member range");
    std::vector<rt::Group> singles;std::vector<std::vector<unsigned>> warps(binding.warps(cta));
    for(const auto& n:nodes) {
        p::need(n.id==singles.size()&&n.warp<warps.size(),"dense source topology");
        for(const auto* deps:{&n.completion_dependencies,&n.issue_dependencies}) {
            std::set<unsigned> seen;
            for(auto d:*deps)p::need(d<n.id&&seen.insert(d).second,"backward unique typed source edge");
        }
        singles.push_back(singleton(n,cfg));warps[n.warp].push_back(n.id);
    }
    auto eligible=[&](unsigned id){return nodes[id].kind==Kind::Compute&&singles[id].subops==1;};
    auto schedule=[&](const std::vector<unsigned>& members){
        std::vector<rt::Member> out;std::map<unsigned,rt::Member> local;
        U slot=0,reserve=0;
        for(auto id:members) {
            U at=std::max(slot,reserve);
            for(auto d:nodes[id].completion_dependencies)if(local.count(d))at=std::max(at,rt::add(local.at(d).completion,1));
            for(auto d:nodes[id].issue_dependencies)if(local.count(d))at=std::max(at,rt::add(local.at(d).dispatch,1));
            rt::Member m{id,at,rt::add(at,singles[id].issue_span),0};m.completion=rt::add(m.reservation_end,singles[id].latency);
            out.push_back(m);local.emplace(id,m);slot=rt::add(at,1);reserve=m.reservation_end;
        }return out;
    };
    std::vector<std::vector<unsigned>> groups;
    for(const auto& warp:warps) {
        std::vector<unsigned> active;
        auto flush=[&](){if(!active.empty()){groups.push_back(std::move(active));active.clear();}};
        for(auto id:warp) {
            if(q==1||!eligible(id)){flush();groups.push_back({id});continue;}
            if(!active.empty()&&nodes[active[0]].pipeline==nodes[id].pipeline&&active.size()<64&&
                nodes[active.back()].ordinal+1==nodes[id].ordinal) {
                auto attempt=active;attempt.push_back(id);
                if(schedule(attempt).back().reservation_end<=q){active.push_back(id);continue;}
            }
            flush();active.push_back(id);
        }flush();
    }
    std::sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){return a.front()<b.front();});
    auto out=std::make_shared<rt::Program>();out->pipeline_count=6;out->logical_members=nodes.size();
    out->warp_groups.resize(warps.size());std::vector<unsigned> owner(nodes.size());
    for(unsigned g=0;g<groups.size();++g) {
        const auto& members=groups[g];auto a=singles[members.front()];
        if(members.size()>1) {
            a.kind=rt::GroupKind::ComputePacket;a.members=schedule(members);
            a.issue_release=a.reservation_end=a.completion=a.modeled_compute_elements=0;
            for(const auto& m:a.members){a.issue_release=std::max(a.issue_release,m.dispatch);
                a.reservation_end=std::max(a.reservation_end,m.reservation_end);a.completion=std::max(a.completion,m.completion);
                a.modeled_compute_elements=p::add(a.modeled_compute_elements,nodes[m.id].compute_elements);}
            p::need(a.reservation_end<=q,"packet reservation horizon");
        }
        for(auto id:members)owner[id]=g;
        out->warp_groups[a.warp].push_back(g);out->groups.push_back(std::move(a));
    }
    std::map<std::tuple<unsigned,unsigned,unsigned>,U> edges;
    for(const auto& n:nodes)for(unsigned type=0;type<2;++type)
        for(auto d:type?n.issue_dependencies:n.completion_dependencies) {
            const auto a=owner[d],b=owner[n.id];
            if(a==b)++out->internal_logical_edges;
            else{++edges[{a,b,type}];++out->logical_edges;}
        }
    for(const auto& [key,count]:edges)out->edges.push_back({std::get<0>(key),std::get<1>(key),rt::EdgeType(std::get<2>(key)),count});
    out->finalize();return out;
}
} // namespace tiny_full
