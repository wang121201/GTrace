#pragma once
#include "model_plan.h"
namespace canonical_silu {
using namespace native_sequence;
struct ModeledBuilder:Builder {
 const Model& model;const J& call;tiny_sha::Sha256 address_digest;
 ModeledBuilder(const Model& m,const J& c,const SourceBundle& s,const coupling::ServiceMapper& map,U count):Builder(s.program,s.registers,map,count,s.range_plan),model(m),call(c){}
    g::CtaGraphStore::Owned build(int c){
        HostTimer timer(build_seconds);
        const auto& body=program.body(0);g::CtaGraphStore::Owned nodes;nodes.reserve(registers.nodes.size());
        for(int flat:registers.order){const auto& r=registers.nodes[flat];int id=spans[c].first_node+r.output;
            std::string pipeline=r.pipeline,op="compute";int elements=r.elements;
            if(r.kind=="global"){pipeline=r.op=='R'?"LD":"ST";op=r.op=='R'?"ld.dram2reg":"st.reg2dram";elements=int(body.records[r.memory].lanes.size())*r.width;}
            if(r.kind=="global"&&elements==0){pipeline="SIMD";op="compute";elements=r.elements;}
            if(r.kind=="shared"){pipeline=r.op=='R'?"LD":"ST";op=r.op=='R'?"ld.sram2reg":"st.reg2sram";elements=int(r.lanes.size())*r.width;}
            if(r.kind=="barrier"){pipeline="BARRIER";op="barrier";elements=1;}
            g::Tile tile(0,0,1,elements);std::vector<int>deps;for(int d:r.completion)deps.push_back(spans[c].first_node+d);
            auto n=std::make_unique<g::DAGNode>(id,"n"+std::to_string(id),pipeline,op,g::cta_placement::token(c,r.warp,program.warps,48,4,true),deps,0,tile,g::DataType::INT8);
            n->sm_id=c%48;n->thread_block_id=c;n->compiler_cta_id=c;n->source_ordinal=r.local;
            for(int d:r.issue)n->issue_depends_on.push_back(spans[c].first_node+d);
            if(r.kind=="barrier")n->setup_latency=0;
            if(r.kind=="global"&&!body.records[r.memory].lanes.empty()){
                const auto& mem=body.records[r.memory];n->matrix_id=1;n->memory_access_granularity_bytes=1;n->memory_coalesce_bytes=128;
                g::ExplicitMemorySubop sub;
                const auto& formula=model.records.at(r.memory);sub.requested_bytes=32*U(mem.width);
                const U va=model.address(call,U(c),formula,0);
                for(int lane=0;lane<32;++lane)p::need(model.address(call,U(c),formula,lane)==va+U(lane*mem.width),"exact modeled contiguous lane order");
                sub.ranges.push_back({-1,va,sub.requested_bytes});sub.source_member_ordinals.push_back(-1);
                const U last=(va+sub.requested_bytes-1)/128*128;
                for(U line=va/128*128;;line+=128){(void)mapper.map(g::CacheLineKey{1,line});if(line==last)break;}
                address_digest.add(std::to_string(c)+":"+std::to_string(r.memory)+":"+std::to_string(va)+":"+std::to_string(sub.requested_bytes)+"\n");
                (mem.op=='R'?read:write)+=sub.requested_bytes;n->explicit_memory_subops.push_back(std::move(sub));
            }else if(r.kind=="shared"){
                n->explicit_sram_bank_service_v1=true;g::ExplicitMemorySubop sub;sub.requested_bytes=r.lanes.size()*r.width;
                for(auto [lane,address]:r.lanes){sub.ranges.push_back({lane,address,U(r.width)});sub.source_member_ordinals.push_back(lane);}n->explicit_memory_subops.push_back(std::move(sub));
            }
            nodes.push_back(std::move(n));
        }
        p::need(nodes.size()==U(spans[c].node_count),"generated CTA node closure");built_nodes+=nodes.size();return nodes;
    }
};
}
