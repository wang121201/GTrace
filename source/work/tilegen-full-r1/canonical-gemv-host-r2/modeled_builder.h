#pragma once
#include "prepared_memory.h"
namespace canonical_gemv {
using namespace native_sequence;
struct ModeledBuilder:Builder {
 const Model& model;const J& call;PreparedMemory prepared_memory;tiny_sha::Sha256 address_digest;U address_group_calls=0;
 ModeledBuilder(const Model& m,const J& c,const SourceBundle& s,const coupling::ServiceMapper& map,U count):Builder(s.program,s.registers,map,count,s.range_plan),model(m),call(c),prepared_memory(m,c,map){}
    g::CtaGraphStore::Owned build(int c){
        HostTimer timer(build_seconds);
        const auto& body=program.body(0);g::CtaGraphStore::Owned nodes;nodes.reserve(registers.nodes.size());
        for(int flat:registers.order){const auto& r=registers.nodes[flat];int id=spans[c].first_node+r.output;
            std::string pipeline=r.pipeline,op="compute";int elements=r.elements;
            if(r.kind=="global"){pipeline=r.op=='R'?"LD":"ST";op=r.op=='R'?"ld.dram2reg":"st.reg2dram";elements=int(body.records[r.memory].lanes.size())*r.width;}
            if(r.kind=="global"&&elements==0){pipeline="SIMD";op="compute";elements=r.elements;}
            if(r.kind=="shared"){pipeline=r.op=='R'?"LD":"ST";op=r.op=='R'?"ld.sram2reg":"st.reg2sram";elements=int(r.lanes.size())*r.width;}
            if(r.kind=="barrier"){pipeline="BARRIER";op="barrier";elements=1;}
            g::Tile tile(0,0,1,elements);
            auto n=std::make_unique<g::DAGNode>(id,"n"+std::to_string(id),pipeline,op,g::cta_placement::token(c,r.warp,program.warps,48,4,true),std::vector<int>{},0,tile,g::DataType::INT8);
            // Materialize the same edges directly in their owner, avoiding a
            // temporary dependency vector and its second allocation/copy.
            n->depends_on.reserve(r.completion.size());
            for(int d:r.completion)n->depends_on.push_back(spans[c].first_node+d);
            n->remaining_deps=int(n->depends_on.size());
            n->sm_id=c%48;n->thread_block_id=c;n->compiler_cta_id=c;n->source_ordinal=r.local;
            n->issue_depends_on.reserve(r.issue.size());
            for(int d:r.issue)n->issue_depends_on.push_back(spans[c].first_node+d);
            if(r.kind=="barrier")n->setup_latency=0;
            if(r.kind=="global"&&!body.records[r.memory].lanes.empty()){
                const auto& mem=body.records[r.memory];n->matrix_id=1;n->memory_access_granularity_bytes=1;n->memory_coalesce_bytes=128;
                auto sub=prepared_memory.materialize(r.memory,U(c));
                address_group_calls+=sub.ranges.size();
                if(host_address::materialization_hash)for(const auto& range:sub.ranges)
                    address_digest.add(std::to_string(c)+":"+std::to_string(r.memory)+":"+std::to_string(range.offset_bytes)+":"+std::to_string(range.byte_count)+"\n");
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
