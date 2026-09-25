#pragma once
// Include after the immutable production canonical full streaming translation unit.
#include "../current-gemm-bridge-r1/bridge-r2.h"
namespace current_gemm_exec {
using namespace current_gemm;
class PreparedMemory {
 J entry_, envelope_;
 std::unique_ptr<p28::Model> p28_;
 std::unique_ptr<prefill_gemm::Model> next_;
 std::unique_ptr<canonical_mixed643::P28Builder> pb_;
 std::unique_ptr<canonical_full::NextBuilder> nb_;
public:
 PreparedMemory(const J& entry,const J& frames,const J& original):entry_(entry),envelope_(original){
  validate_call(entry,frames);const std::string key=entry.at("frame_key");const auto&proof=frames.at(key);
  need(tiny_sha::sha256(original.at("nodes").dump())==proof.at("nodes_sha256").get<std::string>()&&original.at("nodes").size()==num(proof.at("node_count")),"unchanged complete original frame DAG");
  envelope_["ctas"]=proof.at("full_grid");
  for(auto&o:envelope_["objects"]){const auto&cur=entry.at("binding").at("objects").at(o.at("role").get<std::string>());o["pointer"]=cur.at("pointer");o["logical_identity"]=cur.at("logical_identity");}
  J inv=envelope_;inv["ctas"]=original.at("ctas");inv["objects"]=original.at("objects");need(inv==original,"only actual current objects and full declared CTA domain rebound");
  if(key=="P28QKV"){p28_=std::make_unique<p28::Model>(envelope_);pb_=std::make_unique<canonical_mixed643::P28Builder>(*p28_);}
  else{next_=std::make_unique<prefill_gemm::Model>(envelope_);nb_=std::make_unique<canonical_full::NextBuilder>(*next_);}
  visit([&](const auto&m,auto&b){need(m.ctas==int(num(proof.at("full_grid")))&&m.nodes.size()==num(proof.at("node_count")),"full current CTA domain");b.fast_hash=true;b.merge_ranges=true;b.kernel_index=int(num(entry.at("binding").at("native_launch_binding").at("native_launch_id")));});
 }
 template<class F>void visit(F&& fn){if(p28_)fn(*p28_,*pb_);else fn(*next_,*nb_);}
 int ctas()const{return p28_?p28_->ctas:next_->ctas;}
 const J& source_node(std::size_t i)const{return envelope_.at("nodes").at(i);}
 const J& target()const{return entry_.at("binding");}
 J service_map()const{return p28_?p28_->service_map():next_->service_map();}
 J memory_envelope()const{J r=envelope_;r.erase("nodes");r["service_address_map"]=service_map();return r;}
 g::CtaGraphStore::Owned build_cta(int c){need(c>=0&&c<ctas(),"strict full current CTA index");return pb_?pb_->build(c):nb_->build(c);}
 void retire(int c,const std::vector<g::DAGNode*>&ns,g::Cycle cycle){need(c>=0&&c<ctas(),"strict retirement CTA index");if(pb_)pb_->retire(c,ns,cycle);else nb_->retire(c,ns,cycle);}
 // Keep this owner, its immutable models and the caller's current mapper/backend
 // alive until the Fine session is quiescent and CtaGraphStore is destroyed.
};
inline J subop(const g::ExplicitMemorySubop&s){J rs=J::array();for(const auto&r:s.ranges)rs.push_back({r.source_member_ordinal,r.offset_bytes,r.byte_count});return {{"requested_bytes",s.requested_bytes},{"ranges",rs},{"source_member_ordinals",s.source_member_ordinals}};}
template<class Model> inline J compare_event(const Model& model,const J& source,int c,std::size_t i,const g::DAGNode& n,const J& expected){
 const auto&t=model.nodes.at(i);need(t.kind=="global"||t.kind=="async_copy","memory source kind");need(expected.at("cta_linear_id")==c&&expected.at("cta_warp_id")==t.warp&&expected.at("memory_ordinal")==source.at("memory_record_ordinal"),"original memory occurrence identity");
 need(expected.at("pc")==t.pc&&expected.at("opcode")==t.opcode&&expected.at("width")==t.width&&expected.at("instruction_effective_mask")==t.effective,"PC/width/instruction mask");
 need(n.explicit_memory_subops.size()==1&&n.memory_access_granularity_bytes==int(t.width)&&n.memory_coalesce_bytes==128,"actual Fine explicit source contract");J actual=J::object();U mask=0,bytes=0;for(const auto&r:n.explicit_memory_subops[0].ranges){actual[std::to_string(r.source_member_ordinal)]=r.offset_bytes;mask|=U(1)<<r.source_member_ordinal;bytes+=r.byte_count;need(r.byte_count==t.width,"full lane width");}
 need(actual==expected.at("lane_addresses")&&mask==num(expected.at("global_effective_mask"))&&bytes==n.explicit_memory_subops[0].requested_bytes,"all current source addresses/multiplicity");
 if(t.kind=="async_copy"){
  need(expected.at("operation")=="GLOBAL_TO_SHARED"&&n.op_type==g::OpType::CP_DRAM2SRAM_LDGSTS&&n.explicit_async_shared_service_v1&&n.async_copy_bypass_l1,"actual copy remains G2S, never ordinary load");
  need(mask==t.source_mask&&expected.at("source_read_mask")==mask&&expected.at("program_ordinal")==t.ordinal&&expected.at("transfer_width")==t.width,"independent read mask and program occurrence");
  need(n.async_copy_shared_subops.size()==1&&expected.at("source_zero_copy_semantics_qualified")==false,"shared destination and qualification");const auto&dst=n.async_copy_shared_subops[0];need(dst.ranges.size()==std::size_t(__builtin_popcount(unsigned(t.effective))),"shared instruction lanes include source-unread lanes");
  for(const auto&r:dst.ranges)need(expected.at("shared_addresses").at(r.source_member_ordinal)==r.offset_bytes&&r.byte_count==16,"shared destination/width exact");
 }else need(expected.at("operation")=="WRITE"&&n.op_type==g::OpType::ST_REG2DRAM&&mask==t.effective,"native output store");
 return {{"cta",c},{"warp",t.warp},{"memory_ordinal",source.at("memory_record_ordinal")},{"pc",t.pc},{"opcode",t.opcode},{"width",t.width},{"instruction_mask",t.effective},{"source_mask",mask},{"global",subop(n.explicit_memory_subops[0])},{"shared",t.kind=="async_copy"?subop(n.async_copy_shared_subops[0]):J(nullptr)}};
}
}
