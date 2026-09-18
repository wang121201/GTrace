#pragma once
#include "native_memory_program.h"
#include "dag_node.h"
#include "register_program.h"
#include "cta_graph_store.h"
#include "integration.h"
#include "range_plan.h"
#include "semantic_hash.h"
#include <chrono>
namespace native_sequence {
namespace g=GTSim;namespace p=native_program;using J=p::J;using U=p::U;using Clock=std::chrono::steady_clock;
struct HostTimer{ double& seconds;Clock::time_point start=Clock::now();explicit HostTimer(double& value):seconds(value){} ~HostTimer(){seconds+=std::chrono::duration<double>(Clock::now()-start).count();} };
struct Builder{
    const p::Program& program;const native_register::Program& registers;const native_range_plan::Plan& range_plan;bool merge_ranges=false;const coupling::ServiceMapper& mapper;
    std::vector<g::CtaGraphStore::Span> spans;std::vector<std::string> retired_semantics,retired_kernel_semantics;
    bool fast_hash=false;double build_seconds=0,retire_seconds=0,retire_invariant_seconds=0,retire_hash_seconds=0;
    int kernel_index=0;U executed_ctas;U built_nodes=0,retired_nodes=0,retired_ctas=0,read=0,write=0;
    Builder(const p::Program& p,const native_register::Program& r,const coupling::ServiceMapper& m,U count,const native_range_plan::Plan& shared_plan):program(p),registers(r),range_plan(shared_plan),mapper(m),executed_ctas(count){
        int next=0;spans.reserve(count);retired_semantics.resize(count);retired_kernel_semantics.resize(count);for(U c=0;c<count;++c){int n=int(r.nodes.size());spans.push_back({next,n});next+=n;}
    }
    g::CtaGraphStore::Owned build(int c){
        HostTimer timer(build_seconds);
        const auto& body=program.body(c);g::CtaGraphStore::Owned nodes;nodes.reserve(registers.nodes.size());
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
                if(merge_ranges){sub=native_range_plan::materialize(program,mem,range_plan.records[r.memory],c,true);
                    for(const auto& range:sub.ranges){const U last=(range.offset_bytes+range.byte_count-1)/128*128;for(U line=range.offset_bytes/128*128;;line+=128){(void)mapper.map(g::CacheLineKey{1,line});if(line==last)break;}}}
                else {sub.requested_bytes=mem.lanes.size()*mem.width;
                    for(const auto& lane:mem.lanes){U va=program.address(lane,mem.width,c);sub.ranges.push_back({lane.lane,va,U(mem.width)});sub.source_member_ordinals.push_back(lane.lane);
                        const U last=(va+mem.width-1)/128*128;for(U line=va/128*128;;line+=128){(void)mapper.map(g::CacheLineKey{1,line});if(line==last)break;}}}
                (mem.op=='R'?read:write)+=sub.requested_bytes;n->explicit_memory_subops.push_back(std::move(sub));
            }else if(r.kind=="shared"){
                n->explicit_sram_bank_service_v1=true;g::ExplicitMemorySubop sub;sub.requested_bytes=r.lanes.size()*r.width;
                for(auto [lane,address]:r.lanes){sub.ranges.push_back({lane,address,U(r.width)});sub.source_member_ordinals.push_back(lane);}n->explicit_memory_subops.push_back(std::move(sub));
            }
            nodes.push_back(std::move(n));
        }
        p::need(nodes.size()==U(spans[c].node_count),"generated CTA node closure");built_nodes+=nodes.size();return nodes;
    }
    void retire(int c,const std::vector<g::DAGNode*>& nodes,g::Cycle){
        HostTimer timer(retire_seconds);
        {HostTimer audit(retire_invariant_seconds);
        p::need(nodes.size()==U(spans.at(c).node_count),"retired CTA census");
        for(auto* n:nodes){p::need(n->finished&&n->issue_done&&n->issue_deps_resolved&&n->remaining_deps==0&&n->pending_transactions==(n->op_type==g::OpType::BARRIER?1:0),"unfinished node at retirement");
            for(int d:n->issue_depends_on){auto* prev=nodes.at(d-spans[c].first_node);p::need(n->start>prev->issue_cycle,"per-warp source issue order violated");}
            for(int d:n->depends_on){auto* prev=nodes.at(d-spans[c].first_node);p::need(n->start>prev->end,"completion dependency violated");}}
        for(const auto* n:nodes){const auto& source=registers.nodes[registers.order[n->id-spans[c].first_node]];if(source.local>0){const auto& prior=registers.nodes[registers.warp_nodes[source.warp][source.local-1]];if(prior.kind=="barrier")for(int w=0;w<program.warps;++w){int flat=registers.barriers[w][prior.round];p::need(n->start>nodes[registers.nodes[flat].output]->end,"native all-warp barrier rendezvous violated");}}}
        }
        {HostTimer hash_timer(retire_hash_seconds);tiny_sha::Sha256 digest,kernel_digest;std::string scratch;scratch.reserve(256);
        const std::string kernel_prefix="kernel="+std::to_string(kernel_index)+":";
        for(const auto* n:nodes){semantic_audit::integer_line(scratch,std::array<std::int64_t,11>{{n->id,n->warp_id,n->source_ordinal,n->start,n->end,n->issue_cycle,n->ready_cycle,n->total_transactions,n->pending_transactions,n->next_transaction_index,n->remaining_deps}});digest.add(scratch);kernel_digest.add(kernel_prefix);kernel_digest.add(scratch);}
        p::need(retired_semantics[c].empty(),"duplicate CTA retire audit");retired_semantics[c]=digest.hex();retired_kernel_semantics[c]=kernel_digest.hex();}
        ++retired_ctas;retired_nodes+=nodes.size();
    }
};

}
