#pragma once
#include "sealed_inputs.h"
namespace canonical_prefillcopy {
using namespace native_sequence;
inline const std::string code="62ee9483d3ce99efea1ac258050f3a20663a0b4d309b247b779c0b8ca60fb8d1";
inline const std::array<U,8> pcs{{0xed0,0xf10,0x1db0,0x1e00,0x2c70,0x2cb0,0x3b40,0x3b70}};
// Dedicated explicit-model adapter. It never calls a legacy native admission
// constructor with falsified schema/status or labels real data synthetic.
struct Registers {
 std::vector<native_register::Node> nodes;std::vector<int>order;
 std::vector<std::vector<int>>warp_nodes,memory_indices,barriers;
 U global_nodes=0,shared_nodes=0,barrier_nodes=0,compute_nodes=0;
 Registers(const J&t,const p::Program&m):warp_nodes(4),memory_indices(4),barriers(4){
  p::need(t.at("schema")=="PREFILLCOPY_EXPLICIT_NEUTRAL_WARP_TEMPLATE_V1"&&t.at("code_sha256")==code&&t.at("warp_nodes").size()==137,"sealed Copy explicit template");
  for(auto k:{"branch_value_semantics_proved","implicit_register_dependencies_complete"})p::need(t.at(k)==false,"no semantic qualification uplift");
  for(std::size_t i=0;i<m.bodies[0].records.size();++i)memory_indices[m.bodies[0].records[i].warp].push_back(int(i));

  for(int w=0;w<4;++w){int ordinal=0,base=int(nodes.size());const auto& ns=t.at("warp_nodes");
   for(std::size_t i=0;i<ns.size();++i){const auto&v=ns[i];native_register::Node n;n.warp=w;n.local=int(i);n.flat=int(nodes.size());n.output=n.flat;
    p::need(p::natural(v.at("id"))==i,"dense explicit local IDs");n.pc=p::natural(v.at("pc"));n.function=p::natural(m.identity.at("begin").at("function_id"));n.kind=v.at("kind").get<std::string>();n.opcode=v.at("opcode").get<std::string>();n.active=p::natural(v.at("active_mask"),UINT32_MAX);n.mask=p::natural(v.at("effective_mask"),UINT32_MAX);n.elements=p::natural(v.at("elements"),32);
    p::need(n.active&&!(n.mask&~n.active)&&n.elements==std::max(1,__builtin_popcount(n.mask)),"observed node masks/elements");
    for(auto pair:{std::pair<const char*,std::set<int>*>{"depends_on",&n.completion},{"issue_depends_on",&n.issue}})for(const auto&d:v.at(pair.first)){U q=p::natural(d);p::need(q<i&&pair.second->insert(base+int(q)).second,"strict backward dependency");}
    for(int d:n.issue)p::need(!n.completion.count(d),"no duplicate dependency types");if(i)p::need(n.completion.count(n.flat-1)||n.issue.count(n.flat-1),"all observed instruction order");
    if(n.kind=="global"){
     p::need(ordinal<8&&p::natural(v.at("memory_record_ordinal"))==U(ordinal),"ordered global ordinal");n.memory=memory_indices[w].at(ordinal);const auto&r=m.bodies[0].records[n.memory];p::need(r.pc==pcs[ordinal]&&r.pc==n.pc&&r.mask==n.mask&&r.function==n.function&&m.opcodes[r.opcode]==n.opcode&&r.lanes.size()==32,"actual source body join");n.op=r.op;n.width=r.width;++ordinal;++global_nodes;
     p::need(v.at("implicit_register_dependencies_complete")==false&&v.at("implicit_descriptor_register_dependency_unresolved")==true,"unknown GLOBAL descriptor retained");
    }else{
     p::need(n.kind=="compute"||n.kind=="control","Copy has no shared/barrier nodes");n.pipeline=v.at("pipeline").get<std::string>();p::need(n.pipeline=="SIMD","closed Copy pipeline candidate");++compute_nodes;
    }
    warp_nodes[w].push_back(n.flat);order.push_back(n.flat);nodes.push_back(std::move(n));
   }p::need(ordinal==8,"all eight source memory records");
  }p::need(nodes.size()==548&&global_nodes==32,"exact source graph census");
 }
};
struct SourceBundle {
 const J input;p::Program program;Registers registers;int resident=12;
 SourceBundle(J value,const J&t):input(std::move(value)),program(input.at("program")),registers(t,program){
  const auto&r=input.at("register_program");const auto&b=program.identity.at("begin");
  p::need(r.at("schema")=="PREFILLCOPY_SAMPLED_REGISTER_DAG_CPU_CANDIDATE_V1"&&r.at("source_begin")==b&&r.at("code_sha256")==code,"actual Copy source candidate identity");
  p::need(r.at("native_qualified")==false&&r.at("driver_admitted")==false&&r.at("constant_values_complete")==false,"candidate scope only");
  p::need(program.warps==4&&program.ctas==256&&program.bodies.size()==256&&program.kind=="exact_anchors"&&b.at("block")==J::array({128,1,1}),"all256 own-source bodies");
  p::need(r.at("ctas").size()==6&&b.at("native_resources").at("max_active_blocks_per_sm")==12&&b.at("native_resources").at("registers")==12,"sample/source resources");
  for(const auto&c:r.at("ctas"))for(const auto&w:c.at("warps")){J ns=w.at("nodes");for(auto&n:ns){n.erase("function_id");n.erase("memory_body_record_ordinal");}p::need(ns==t.at("warp_nodes"),"every sampled graph exact neutral equality");}
 }
};
struct CopyCatalog {
 SealRegistry seals;std::map<std::string,std::unique_ptr<const SourceBundle>>bundles;
 CopyCatalog(const J&s,const J&t){
  for(auto k:{"plan_file","template_file","model_manifest","model_source","compiler_manifest","source_oracle"})seals.evidence(s.at(k));
  for(const auto&ref:s.at("source_refs")){auto m=strict_parse(seals.load_source(ref.at("program_file")));auto r=strict_parse(seals.load_source(ref.at("register_file")));J entry={{"program",std::move(m)},{"register_program",std::move(r)}};bundles.emplace(ref.at("catalog_id").get<std::string>(),std::make_unique<const SourceBundle>(std::move(entry),t));}
  p::need(bundles.size()==1,"one separately sealed observed Copy source");
 }
 const SourceBundle&get(const std::string&id)const{return *bundles.at(id);}
 J receipt()const{return {{"sources",bundles.size()},{"memory_oracle_validations",1},{"sampled_warp_graph_comparisons",24},{"unique_evidence_pins_verified",seals.verified},{"final_unique_pin_rechecks",seals.final_checks},{"legacy_native_parser_bypassed_via_falsified_status",false},{"dedicated_explicit_model_adapter",true},{"implicit_register_dependencies_complete",false}};}
};
}
