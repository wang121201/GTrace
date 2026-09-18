#pragma once
#include "model_plan.h"
namespace canonical_rotary {
using namespace native_cta_sequence;
struct ModeledBuilder:ClassBuilder {
 const Model& model;const J& call;tiny_sha::Sha256 address_digest;
 ModeledBuilder(const Model&m,const J&c,const SourceBundle&s,const coupling::ServiceMapper&map,U count):ClassBuilder(s.program,s.catalog,map,count),model(m),call(c){}
    g::CtaGraphStore::Owned build(int c){
        HostTimer timer(build_seconds);
        const auto& registers=catalog.program(c);const auto& range_plan=catalog.plan(c);const auto& body=program.body(c);g::CtaGraphStore::Owned nodes;nodes.reserve(registers.nodes.size());
        for(int flat:registers.order){const auto& r=registers.nodes[flat];int id=spans[c].first_node+r.output;
            std::string pipeline=r.pipeline,op="compute";int elements=r.elements;
            if(r.kind=="global"){pipeline=r.op=='R'?"LD":"ST";op=r.op=='R'?"ld.dram2reg":"st.reg2dram";elements=int(body.records[catalog.record(c,r)].lanes.size())*r.width;}
            if(r.kind=="global"&&elements==0){pipeline="SIMD";op="compute";elements=r.elements;}
            if(r.kind=="shared"){pipeline=r.op=='R'?"LD":"ST";op=r.op=='R'?"ld.sram2reg":"st.reg2sram";elements=int(r.lanes.size())*r.width;}
            if(r.kind=="barrier"){pipeline="BARRIER";op="barrier";elements=1;}
            g::Tile tile(0,0,1,elements);std::vector<int>deps;for(int d:r.completion)deps.push_back(spans[c].first_node+d);
            auto n=std::make_unique<g::DAGNode>(id,"n"+std::to_string(id),pipeline,op,g::cta_placement::token(c,r.warp,program.warps,48,4,true),deps,0,tile,g::DataType::INT8);
            n->sm_id=c%48;n->thread_block_id=c;n->compiler_cta_id=c;n->source_ordinal=r.local;
            for(int d:r.issue)n->issue_depends_on.push_back(spans[c].first_node+d);
            if(r.kind=="barrier")n->setup_latency=0;
            if(r.kind=="global"&&!body.records[catalog.record(c,r)].lanes.empty()){
                const auto& mem=body.records[catalog.record(c,r)];n->matrix_id=1;n->memory_access_granularity_bytes=1;n->memory_coalesce_bytes=128;
                g::ExplicitMemorySubop sub;
                sub.requested_bytes=mem.lanes.size()*mem.width;
                for(const auto& lane:mem.lanes){const U va=model.address(call,c,r.warp,lane.lane,mem.pc,mem.width);
                    sub.ranges.push_back({lane.lane,va,U(mem.width)});sub.source_member_ordinals.push_back(lane.lane);
                    const U last=(va+mem.width-1)/128*128;for(U line=va/128*128;;line+=128){(void)mapper.map(g::CacheLineKey{1,line});if(line==last)break;}
                    address_digest.add(std::to_string(c)+":"+std::to_string(r.warp)+":"+std::to_string(mem.pc)+":"+std::to_string(lane.lane)+":"+std::to_string(va)+"\n");
                }
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
