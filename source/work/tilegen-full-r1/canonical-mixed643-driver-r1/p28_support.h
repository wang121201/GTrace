#pragma once
namespace canonical_mixed643 {
using namespace native_sequence;
struct P28Catalog {
 p28::Model model;U encoded=0,edges=0,records=0,lanes=0;
 P28Catalog(const J&j,const J&seal):model(j){
  auto raw=j.at("nodes").dump();p::need(raw.size()==p::natural(seal.at("bytes"))&&tiny_sha::sha256(raw)==seal.at("sha256").get<std::string>(),"sealed RAM template nodes");encoded=raw.size();p::need(model.ctas==48,"full48 P28 structure");
  for(const auto&n:model.nodes){edges+=n.deps.size()+n.issues.size();if(n.kind=="global"||n.kind=="async_copy"){++records;lanes+=n.global.size();}}
  p::need(edges==245940&&records==2608,"P28 pool census");
 }
 void select(const J&c){
  p::need(c.at("schema")=="P28_QKV_ADDRESS_CALL_V1"&&c.at("kind")=="canonical_target"&&c.at("role")=="self_attn.qkv_proj"&&c.at("phase")=="Prefill"&&c.at("process")==J({{"pid",1925216},{"start_ticks",857625100}}),"sealed P28 target domain");
  p::need(c.at("grid")==J::array({48,1,1})&&c.at("block")==J::array({128,1,1})&&c.at("native_resources").at("static_shared_bytes")==49152&&c.at("native_resources").at("dynamic_shared_bytes")==24576&&c.at("native_resources").at("max_active_blocks_per_sm")==1,"P28 resources unchanged");
  const std::array<std::string,3>roles={"weight","input","output"};const std::array<U,3>sizes={50331648,262144,393216},strides={1048576,0,256};std::vector<p28::Object> next;
  for(int i=0;i<3;++i){const auto&o=c.at("objects").at(roles[i]);U lo=p::natural(o.at("pointer"));p::need(o.at("bytes")==sizes[i]&&o.at("end_exclusive")==p::add(lo,sizes[i])&&lo%128==0,"canonical P28 role span");next.push_back({roles[i],o.at("logical_identity"),lo,sizes[i],strides[i]});}
  for(int i=0;i<3;++i)for(int k=i+1;k<3;++k)p::need(p::add(next[i].pointer,next[i].bytes)<=next[k].pointer||p::add(next[k].pointer,next[k].bytes)<=next[i].pointer,"canonical disjoint roles");
  model.objects=std::move(next); // Only called at the quiescent kernel boundary; nodes/edges never mutate.
 }
 J receipt()const{return {{"sources",1},{"encoded_source_bytes",encoded},{"register_template_nodes",model.nodes.size()},{"typed_register_dependency_edges",edges},{"memory_records",records},{"memory_lane_tuples",lanes},{"oracle_validations",0},{"inherited_sampled_CTA_oracle_checks",36},{"source_bytes_are_RSS",false},{"shared_template_instances",1},{"same_source_dependency_template",true},{"known_BYPASS_L1",true},{"CONSTANT_policy_recovered",false}};}
};
struct P28Builder: p28::Builder {
 bool fast_hash=false,merge_ranges=false;int kernel_index=0;const std::vector<g::CtaGraphStore::Span>&spans;U&built_nodes;U&retired_nodes;std::vector<std::string>&retired_semantics;std::vector<std::string>retired_kernel_semantics;
 explicit P28Builder(const p28::Model&m):p28::Builder(m),spans(m.spans),built_nodes(built),retired_nodes(retired),retired_semantics(digests),retired_kernel_semantics(m.ctas){}
 void retire(int c,const std::vector<g::DAGNode*>&ns,g::Cycle cy){p28::Builder::retire(c,ns,cy);tiny_sha::Sha256 h;h.add("kernel="+std::to_string(kernel_index)+":"+digests[c]+"\n");retired_kernel_semantics[c]=h.hex();}
};
}
