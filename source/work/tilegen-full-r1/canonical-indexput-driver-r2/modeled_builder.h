#pragma once
#include "model_plan.h"
namespace canonical_indexput {
struct ModeledBuilder {
 const p::Program& program;const Registers& registers;const coupling::ServiceMapper& mapper;
 const Model&model;const J&call;std::vector<g::CtaGraphStore::Span>spans;
 std::vector<std::string>retired_semantics,retired_kernel_semantics;
 bool fast_hash=false,merge_ranges=false;double build_seconds=0,retire_seconds=0,retire_invariant_seconds=0,retire_hash_seconds=0;
 int kernel_index=0;U executed_ctas,built_nodes=0,retired_nodes=0,retired_ctas=0,read=0,write=0;tiny_sha::Sha256 address_digest;
 ModeledBuilder(const Model&m,const J&c,const SourceBundle&s,const coupling::ServiceMapper&map,U count):program(s.program),registers(s.registers),mapper(map),model(m),call(c),executed_ctas(count){
  int next=0;retired_semantics.resize(count);retired_kernel_semantics.resize(count);for(U cta=0;cta<count;++cta){spans.push_back({next,int(registers.nodes.size())});next+=int(registers.nodes.size());}
 }
 g::CtaGraphStore::Owned build(int c){HostTimer timer(build_seconds);g::CtaGraphStore::Owned nodes;nodes.reserve(registers.nodes.size());
  for(int flat:registers.order){const auto&r=registers.nodes[flat];int id=spans[c].first_node+r.output;std::string pipeline=r.pipeline,op="compute";int elements=r.elements;
   if(r.kind=="global"){pipeline=r.op=='R'?"LD":"ST";op=r.op=='R'?"ld.dram2reg":"st.reg2dram";elements=32*r.width;}
   g::Tile tile(0,0,1,elements);std::vector<int>deps;for(int d:r.completion)deps.push_back(spans[c].first_node+d);
   auto n=std::make_unique<g::DAGNode>(id,"n"+std::to_string(id),pipeline,op,g::cta_placement::token(c,r.warp,4,48,4,true),deps,0,tile,g::DataType::INT8);
   n->sm_id=c%48;n->thread_block_id=c;n->compiler_cta_id=c;n->source_ordinal=r.local;for(int d:r.issue)n->issue_depends_on.push_back(spans[c].first_node+d);
   if(r.kind=="global"){
    const auto&f=model.records(call).at(r.memory);n->matrix_id=1;n->memory_access_granularity_bytes=1;n->memory_coalesce_bytes=128;g::ExplicitMemorySubop sub;sub.requested_bytes=32*U(r.width);
    for(int l=0;l<32;++l)sub.source_member_ordinals.push_back(l);
    if(f.role=="indices"){
     // A broadcast has32 requested8B lanes. Do not pretend it is one256B span.
     for(int l=0;l<32;++l)sub.ranges.push_back({l,model.address(call,U(c),f,l),U(r.width)});
    }else{
     U va=model.address(call,U(c),f,0);for(int l=0;l<32;++l)p::need(model.address(call,U(c),f,l)==va+U(l*r.width),"actual contiguous value/store formula");sub.ranges.push_back({-1,va,sub.requested_bytes});
    }
    for(const auto&range:sub.ranges){const U last=(range.offset_bytes+range.byte_count-1)/128*128;for(U line=range.offset_bytes/128*128;;line+=128){(void)mapper.map(g::CacheLineKey{1,line});if(line==last)break;}address_digest.add(std::to_string(c)+":"+std::to_string(r.memory)+":"+std::to_string(range.offset_bytes)+":"+std::to_string(range.byte_count)+"\n");}
    (r.op=='R'?read:write)+=sub.requested_bytes;n->explicit_memory_subops.push_back(std::move(sub));
   }nodes.push_back(std::move(n));
  }p::need(nodes.size()==U(spans[c].node_count),"CTA generated count");built_nodes+=nodes.size();return nodes;
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
