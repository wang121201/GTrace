#pragma once
#include "native_memory_program.h"
#include "family_admission.h"
#include <queue>

// Bounded, observed warp programs. This adapter supplies dependencies, never
// an NCU-derived duration. Dense node IDs are assigned after a true topo sort.
namespace native_register {
namespace p=native_program;using J=p::J;using U=p::U;
struct Node {
    int warp,local,flat,output=-1,elements=1,round=-1,memory=-1;
    U pc=0,function=0;std::uint32_t active=0,mask=0;
    std::string kind,opcode,pipeline;char op=0;int width=0;
    std::vector<std::pair<int,U>> lanes;
    std::set<int> completion,issue;
};
struct Program {
    const p::Program& memory;
    std::vector<Node> nodes;std::vector<int> order;
    std::vector<std::vector<int>> warp_nodes,memory_indices;
    std::vector<std::vector<int>> barriers;
    U global_nodes=0,shared_nodes=0,compute_nodes=0,barrier_nodes=0;
    J identity,assumptions;
    Program(const J& j,const p::Program& mem,bool fixture=false):memory(mem) {
        const bool fused=!fixture&&native_family::is_fused_key(j.at("source_launch_key").get<std::string>());
        const bool norm=!fixture&&native_family::is_norm_key(j.at("source_launch_key").get<std::string>());
        if(fused)native_family::fused_register_envelope(j,mem.identity.at("begin"));
        else if(norm)native_family::norm_register_envelope(j,mem.identity.at("begin"));
        else p::need(j.at("schema")=="TILEGEN_NATIVE_SIMT_REGISTER_PROGRAM_V2","register program schema");
        if(!norm&&!fused)p::need(j.at("status")== (fixture?"SYNTHETIC_CONSUMER_FIXTURE_ONLY":"QUALIFIED_SAMPLED_PROGRAM_WITH_EXPLICIT_MICROARCH_APPROXIMATION"),"register program source qualification");
        p::need(j.value("synthetic_fixture_only",false)==fixture,"synthetic qualification mismatch");
        if(!fixture) {
            p::need(j.at("source_begin")==mem.identity.at("begin"),"exact fresh compute/memory source BEGIN binding");
            const auto key=j.at("source_launch_key").get<std::string>();
            const auto& descriptor=native_family::begin_descriptor(j.at("source_begin"));
            p::need(j.at("source_begin").at("source_launch_key")==key,"source key binding");
            p::need(j.at("grid")==descriptor.at("grid")&&j.at("block")==descriptor.at("block"),"sealed selected-source geometry");
        }
        U shared_bytes=p::natural(j.at("shared_allocation_bytes"),1024*1024);p::need(shared_bytes>=4 || (!fixture&&native_family::is_silu_key(j.at("source_launch_key").get<std::string>())&&shared_bytes==0),"native shared allocation required unless sealed no-shared SiLU");
        p::need(j.at("code_sha256")==mem.identity.at("begin").at("code_sha256"),"compute/memory code binding");
        p::need(j.at("grid")==mem.identity.at("begin").at("grid")&&j.at("block")==mem.identity.at("begin").at("block"),"compute/memory geometry binding");
        p::need(mem.bodies.size()==1&&(mem.kind=="affine_periodic"||(!fixture&&(native_family::is_silu_key(j.at("source_launch_key").get<std::string>())||norm||fused)&&mem.kind=="exact_anchors"&&mem.ctas==1)),"register reuse requires one affine body or sealed exact singleton SiLU");
        p::need(j.at("cycle_accuracy_claimed")==false&&j.at("pipeline_parameters_fitted")==false,"explicit microarchitecture qualification");
        assumptions=fused?native_family::fused_assumptions(j.at("source_launch_key").get<std::string>()):norm?native_family::norm_assumptions(j.at("source_launch_key").get<std::string>()):j.at("assumptions");p::need(assumptions.is_array()&&!assumptions.empty(),"register model assumptions required");
        identity={{"code_sha256",j.at("code_sha256")},{"schema",j.at("schema")},{"program_sha256",tiny_sha::sha256(j.dump())}};
        warp_nodes.resize(mem.warps);memory_indices.resize(mem.warps);barriers.resize(mem.warps);
        for(std::size_t i=0;i<mem.bodies[0].records.size();++i)memory_indices[mem.bodies[0].records[i].warp].push_back(int(i));
        const auto& warps=j.at("warps");p::need(warps.is_array()&&warps.size()==U(mem.warps),"complete warp programs required");
        for(int w=0;w<mem.warps;++w){const auto& source=warps[w];p::need(p::natural(source.at("warp"))==U(w),"ordered unique warp programs");
            const auto& ns=source.at("nodes");p::need(ns.is_array()&&!ns.empty()&&nodes.size()+ns.size()<=200000,"bounded nonempty register program");
            int base=int(nodes.size()),next_memory=0;
            for(std::size_t i=0;i<ns.size();++i){const auto& v=ns[i];p::need(p::natural(v.at("id"))==i,"dense warp local ids");Node n;n.warp=w;n.local=int(i);n.flat=int(nodes.size());n.pc=p::natural(v.at("pc"));n.function=p::natural(v.at("function_id"));n.kind=v.at("kind").get<std::string>();n.opcode=v.at("opcode").get<std::string>();
                p::need(!n.opcode.empty()&&n.opcode.size()<=128,"bounded opcode");n.active=std::uint32_t(p::natural(v.at("active_mask"),UINT32_MAX));n.mask=std::uint32_t(p::natural(v.at("effective_mask"),UINT32_MAX));p::need(n.active&&!(n.mask&~n.active),"register program mask");
                n.elements=int(p::natural(v.at("elements"),32));p::need(n.elements==std::max(1,__builtin_popcount(n.mask)),"compute element/mask contract");
                for(auto pair:{std::pair<const char*,std::set<int>*>{"depends_on",&n.completion},{"issue_depends_on",&n.issue}}){const auto& ds=v.at(pair.first);p::need(ds.is_array(),"dependency list");for(const auto& d:ds){auto x=p::natural(d);p::need(x<i&&pair.second->insert(base+int(x)).second,"backward unique local dependency");}}
                for(int d:n.issue)p::need(!n.completion.count(d),"duplicate completion/issue edge");
                if(i)p::need(n.completion.count(n.flat-1)||n.issue.count(n.flat-1),"native warp issue sequence must be preserved");
                if(n.kind=="global"){
                    p::need(p::natural(v.at("memory_record_ordinal"))==U(next_memory)&&next_memory<int(memory_indices[w].size()),"ordered complete global memory binding");
                    n.memory=memory_indices[w][next_memory++];const auto& r=mem.bodies[0].records[n.memory];
                    p::need(r.pc==n.pc&&r.function==n.function&&r.mask==n.mask&&mem.opcodes[r.opcode]==n.opcode,"register/global source mismatch");n.op=r.op;n.width=r.width;++global_nodes;
                }else if(n.kind=="shared"){
                    p::need(shared_bytes>=4,"shared node requires allocated native shared bytes");
                    n.width=fused?native_family::fused_shared_width(v):4;
                    p::need(n.mask&&((fused&&v.at("width")==n.width)||(!fused&&v.at("width")==4&&(n.opcode=="LDS"||n.opcode=="STS"))),"shared exact form");auto op=v.at("op").get<std::string>();p::need(op==((n.opcode=="LDS"||n.opcode=="LDS.128")?"R":"W"),"shared direction");n.op=op[0];
                    std::uint32_t mask=0;for(const auto& lane:v.at("lanes")){p::need(lane.is_array()&&lane.size()==2,"shared lane shape");int id=int(p::natural(lane[0],31));U address=p::natural(lane[1],shared_bytes-U(n.width));p::need(!(mask&(1u<<id))&&address%U(n.width)==0,"shared lane identity/alignment");mask|=1u<<id;n.lanes.push_back({id,address});}
                    p::need(mask==n.mask,"shared actual active lane coverage");++shared_nodes;
                }else if(n.kind=="barrier"){
                    p::need((fused?native_family::fused_barrier_form(v):norm?native_family::norm_barrier_form(v):n.opcode=="BAR.SYNC")&&n.active==UINT32_MAX&&n.mask==UINT32_MAX,"full CTA barrier participant");n.round=int(p::natural(v.at("barrier_round"),1024));p::need(U(n.round)==barriers[w].size(),"consecutive barrier rounds");barriers[w].push_back(n.flat);++barrier_nodes;
                }else{
                    p::need(n.kind=="compute"||n.kind=="control","unsupported register node kind");n.pipeline=v.at("pipeline").get<std::string>();p::need(n.pipeline=="SIMD"||n.pipeline=="SFU"||n.pipeline=="SHFL","supported compute pipeline");++compute_nodes;
                }
                warp_nodes[w].push_back(n.flat);nodes.push_back(std::move(n));
            }
            p::need(next_memory==int(memory_indices[w].size()),"unbound global memory records");
        }
        const U rounds=barriers[0].size();for(int w=0;w<mem.warps;++w)p::need(barriers[w].size()==rounds,"CTA barrier participation differs");
        // Every warp may arrive independently. Its post-barrier instruction
        // waits until every participating warp has completed that same round.
        for(int w=0;w<mem.warps;++w){int previous_barrier=-1;std::set<int> memory_since;
            for(int flat:warp_nodes[w]){auto& n=nodes[flat];
                if(previous_barrier>=0){int round=nodes[previous_barrier].round;for(int other=0;other<mem.warps;++other){int d=barriers[other][round];n.issue.erase(d);n.completion.insert(d);}previous_barrier=-1;}
                if(n.kind=="global"||n.kind=="shared")memory_since.insert(flat);
                if(n.kind=="barrier"){for(int d:memory_since)p::need(n.completion.count(d),"barrier must wait preceding memory completion");memory_since.clear();previous_barrier=flat;}
            }
        }
        std::vector<int> indegree(nodes.size());std::vector<std::vector<int>> children(nodes.size());std::priority_queue<int,std::vector<int>,std::greater<int>> ready;
        for(const auto& n:nodes){indegree[n.flat]=int(n.completion.size()+n.issue.size());for(const auto* edges:{&n.completion,&n.issue})for(int d:*edges){p::need(d>=0&&U(d)<nodes.size()&&d!=n.flat,"dependency target");children[d].push_back(n.flat);}if(!indegree[n.flat])ready.push(n.flat);}
        while(!ready.empty()){int i=ready.top();ready.pop();nodes[i].output=int(order.size());order.push_back(i);for(int c:children[i])if(--indegree[c]==0)ready.push(c);}
        p::need(order.size()==nodes.size(),"CTA dependency cycle");p::need(p::multiply(nodes.size(),mem.ctas)<=INT32_MAX,"per-kernel dense id budget");
        for(auto& n:nodes){std::set<int>a,b;for(int d:n.completion){p::need(nodes[d].output<n.output,"completion topo ordering");a.insert(nodes[d].output);}for(int d:n.issue){p::need(nodes[d].output<n.output,"issue topo ordering");b.insert(nodes[d].output);}n.completion=std::move(a);n.issue=std::move(b);}
    }
    J census()const{return {{"nodes_per_cta",nodes.size()},{"full_grid_nodes",p::multiply(nodes.size(),memory.ctas)},{"global_nodes_per_cta",global_nodes},{"shared_nodes_per_cta",shared_nodes},{"compute_control_nodes_per_cta",compute_nodes},{"barrier_nodes_per_cta",barrier_nodes},{"barrier_rounds",barriers[0].size()},{"node_object_bytes",sizeof(GTSim::DAGNode)}};}
};
}
