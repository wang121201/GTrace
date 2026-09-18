#pragma once
#include "../driver-attention-r1/model.h"
namespace attention_bridge {
struct Builder {
 const Model&m;U built=0,retired=0,retired_ctas=0,completion_edges=0,issue_edges=0,read=0,write=0,copy_done=0,zero_done=0,tensor_work=0;std::vector<std::string>digests;tiny_sha::Sha256 addresses;
 explicit Builder(const Model& model):m(model),digests(model.ctas){}
 void word(U v){std::array<char,8>b{};for(int i=0;i<8;++i)b[i]=char(v>>(8*i));addresses.add(b.data(),b.size());}
 g::CtaGraphStore::Owned build(int c){host_bound();g::CtaGraphStore::Owned out;out.reserve(m.nodes.size());int first=m.spans[c].first_node;
  for(std::size_t i=0;i<m.nodes.size();++i){const auto&t=m.nodes[i];if(i%4096==0)host_bound();std::vector<int>deps;for(int x:t.deps)deps.push_back(first+x);std::string pipe=t.pipe,op="compute";
   if(t.kind=="async_copy"){pipe="LD";op="cp.dram2sram_ldgsts";}
   else if(t.kind=="global"){pipe=t.memop=="R"?"LD":"ST";op=t.memop=="R"?"ld.dram2reg":"st.reg2dram";}
   else if(t.kind=="shared"||t.kind=="shared_matrix"){pipe=t.memop=="R"?"LD":"ST";op=t.memop=="R"?"ld.sram2reg":"st.reg2sram";}
   else if(t.kind=="tensor"){pipe="Tensor";op="mma";}else if(t.kind=="barrier"){pipe="BARRIER";op="barrier";}
   g::Tile tile(0,0,1,t.elements);if(t.kind=="tensor")tile=g::Tile(0,0,16,8);
   auto n=std::make_unique<g::DAGNode>(first+int(i),"attention."+std::to_string(first+i),pipe,op,g::cta_placement::token(c,t.warp,4,48,4,true),deps,0,tile,t.kind=="tensor"?g::DataType::BF16:g::DataType::FP32);
   n->sm_id=c%48;n->thread_block_id=c;n->compiler_cta_id=c;n->source_ordinal=t.old;n->graph_node_id=std::to_string(t.warp)+":"+std::to_string(t.ordinal)+":"+std::to_string(t.pc);n->semantic_role=t.opcode;n->matrix_id=1;
   for(int x:t.issues)n->issue_depends_on.push_back(first+x);
   if(t.kind=="global"||t.kind=="async_copy"){
    g::ExplicitMemorySubop sub;for(auto&x:m.global(c,t)){const auto&o=m.objects[x.role];U addr=add(o.pointer,x.offset);sub.ranges.push_back({x.lane,addr,x.width});sub.source_member_ordinals.push_back(x.lane);sub.requested_bytes+=x.width;word(U(c));word(i);word(U(x.lane));word(addr);word(x.width);}n->explicit_memory_subops.push_back(std::move(sub));n->memory_access_granularity_bytes=int(t.width);n->memory_coalesce_bytes=128;
   }
   if(t.kind=="shared"||t.kind=="shared_matrix"||t.kind=="async_copy"){
    g::ExplicitMemorySubop sub;for(auto&x:t.shared){sub.ranges.push_back({x.lane,x.offset,x.width});sub.source_member_ordinals.push_back(x.lane);sub.requested_bytes+=x.width;}
    if(t.kind=="async_copy"){n->explicit_async_shared_service_v1=true;n->async_copy_bypass_l1=true;n->async_copy_shared_subops.push_back(std::move(sub));}
    else{n->explicit_sram_bank_service_v1=true;n->explicit_memory_subops.push_back(std::move(sub));n->memory_access_granularity_bytes=int(t.width);}
   }
   if(t.kind=="tensor")n->declare_tensor_fma_work(16);
   out.push_back(std::move(n));++built;
  }return out;
 }
 void retire(int c,const std::vector<g::DAGNode*>& ns,g::Cycle cy){need(ns.size()==m.nodes.size(),"CTA retirement count");tiny_sha::Sha256 h;const int first=m.spans[c].first_node;
  for(std::size_t i=0;i<ns.size();++i){const auto*n=ns[i];const auto&t=m.nodes[i];need(n->id==first+int(i)&&n->finished&&n->issue_done&&n->issue_deps_resolved&&n->remaining_deps==0,"node identity/completion closed");need(n->pending_transactions==(n->op_type==g::OpType::BARRIER?1:0)&&n->end<=cy,"transactions/retirement cycle closed");
   for(int d:n->depends_on){need(n->start>=ns.at(d-first)->end+1,"completion edge timing");++completion_edges;}for(int d:n->issue_depends_on){need(n->start>=ns.at(d-first)->issue_cycle+1,"issue edge timing");++issue_edges;}
   if(t.kind=="async_copy"){need(n->async_copy_phase==3&&n->tma_issue_complete_cycle>=n->issue_cycle&&n->end>n->tma_issue_complete_cycle,"copy global acknowledgement then SRAM destination completion");++copy_done;zero_done+=!t.source_mask;}
   if(t.kind=="tensor")tensor_work+=U(n->tensor_fma_work_per_subpartition());
   if(t.kind=="global"||t.kind=="async_copy")for(auto&x:m.global(c,t)){if(t.memop=="R")read+=x.width;else write+=x.width;}
   std::array<g::Cycle,14>values{{n->id,t.old,t.warp,t.ordinal,t.pc,n->start,n->end,n->issue_cycle,n->ready_cycle,n->total_transactions,n->next_transaction_index,n->pending_transactions,n->remaining_deps,n->tma_issue_complete_cycle}};std::array<char,14*8>bytes{};for(std::size_t k=0;k<values.size();++k)for(int b=0;b<8;++b)bytes[k*8+b]=char(U(values[k])>>(8*b));h.add(bytes.data(),bytes.size());++retired;
  }digests[c]=h.hex();++retired_ctas;std::cerr<<J({{"progress_completed_CTAs",retired_ctas},{"total_CTAs",m.ctas},{"role",m.p.name},{"continuous_cycle",cy}}).dump()<<std::endl;
 }
 J receipt(){tiny_sha::Sha256 h;for(auto&s:digests){need(s.size()==64,"all CTA audit hashes");h.add(s+"\n");}return {{"built_nodes",built},{"retired_nodes",retired},{"retired_CTAs",retired_ctas},{"completion_edges_checked",completion_edges},{"issue_edges_checked",issue_edges},{"requested_read_bytes",read},{"requested_write_bytes",write},{"async_copy_nodes_completed",copy_done},{"zero_source_copy_nodes_completed",zero_done},{"declared_tensor_fma_work",tensor_work},{"per_CTA_node_semantics_sha256",digests},{"ordered_CTA_node_semantics_sha256",h.hex()},{"modeled_address_materialization_sha256",addresses.hex()}};}
};
}
