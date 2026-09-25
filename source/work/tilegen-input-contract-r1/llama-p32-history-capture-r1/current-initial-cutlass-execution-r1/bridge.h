#pragma once
// Exactly native144: source-derived full fixed-control CUTLASS path, not a dynamic witness.
namespace current_initial_cutlass {
using namespace native_sequence;
class Binding final:public tiny_full::KernelBinding {
 const J&t_;J call_;const coupling::ServiceMapper&mapper_;std::vector<tiny_full::SourceNode>nodes_;
public:
 static void admit(const J&c,const J&t){
  p::need(c.at("schema")=="CURRENT_INITIAL_CUTLASS_EXECUTION_CALL_V1"&&c.at("native_launch_id")==144&&c.at("process")==J({{"pid",3011726},{"start_ticks",897090720}}),"exact current144 process");
  p::need(tiny_sha::sha256(c.dump())==t.at("admitted_call_sha256").get<std::string>(),"source-prepared entire current call unchanged");
  const auto&a=c.at("raw_argument_record");const auto&b=c.at("binding");const auto&l=c.at("launch_before");const auto&r=c.at("launch_return");
  p::need(tiny_sha::sha256(a.dump())==c.at("raw_argument_sha256").get<std::string>()&&a.at("native_launch_binding").at("native_launch_id")==144&&a.at("native_launch_binding").at("process")==c.at("process"),"full actual raw Params record");
  p::need(c.at("grid")==J::array({1,1,1})&&c.at("block")==J::array({128,1,1})&&a.at("arguments").size()==1&&a.at("arguments")[0].at("size_bytes")==360,"exact fixed geometry and Params ABI");
  for(auto k:{"grid","block"})p::need(c.at(k)==b.at(k)&&c.at(k)==l.at(k)&&c.at(k)==r.at(k),"current geometry join");
  for(auto k:{"code_sha256","function_id","parameter_layout_sha256","stream_u64","context_id"})p::need(l.at(k)==r.at(k)&&l.at(k)==a.at(k),"current identity/layout/context");
  p::need(l.at("registers")==48&&l.at("dynamic_shared_bytes")==5120&&l.at("static_shared_bytes")==0&&r.at("cuda_status")==0&&l.at("event_ordinal")<r.at("event_ordinal"),"actual resource and successful callbacks");
  p::need(c.at("resident_limit_assumption")==1&&c.at("nodes")==1280&&t.at("nodes").size()==1280,"strict source instruction scope");
  for(auto k:{"current_occupancy_observed","compute_transfer_qualified","implicit_register_dependencies_complete","full_simulation_admitted"})p::need(c.at(k)==false,"source-derived lower qualification retained");
 }
 Binding(const J&t,const J&c,const coupling::ServiceMapper&mapper):t_(t),call_(c),mapper_(mapper){admit(c,t);
  for(const auto&n:t.at("nodes")){tiny_full::SourceNode s;s.id=p::natural(n.at("id"));s.warp=p::natural(n.at("warp"));s.ordinal=p::natural(n.at("ordinal"));s.source_ordinal=p::natural(n.at("source_ordinal"));s.pc=p::natural(n.at("pc"));s.mask=p::natural(n.at("effective_mask"));s.compute_elements=p::natural(n.at("elements"));s.tensor_fma=p::natural(n.at("tensor_fma"));s.pipeline=n.at("pipeline");s.write=n.at("write");auto k=n.at("kind");s.kind=k=="global"?tiny_full::Kind::Global:k=="shared"?tiny_full::Kind::Shared:k=="barrier"?tiny_full::Kind::Barrier:k=="tensor"?tiny_full::Kind::Tensor:k=="control"?tiny_full::Kind::Control:tiny_full::Kind::Compute;s.completion_dependencies=n.at("completion_dependencies").get<std::vector<unsigned>>();s.issue_dependencies=n.at("issue_dependencies").get<std::vector<unsigned>>();p::need(s.id==nodes_.size()&&s.warp<4,"dense fixed DAG");for(auto d:s.completion_dependencies)p::need(d<s.id,"backward RAW/WAW/shared/barrier edge");for(auto d:s.issue_dependencies)p::need(d<s.id,"backward source issue edge");nodes_.push_back(std::move(s));}
 }
 U ctas()const override{return 1;}unsigned warps(U c)const override{p::need(c==0,"one original CTA");return 4;}unsigned resident_limit()const override{return 1;}U first_node(U c)const override{p::need(c==0,"one original CTA");return 0;}U template_class(U c)const override{p::need(c==0,"one original CTA");return 0;}std::span<const tiny_full::SourceNode>nodes(U c)const override{p::need(c==0,"one original CTA");return nodes_;}
 tiny_full::MemoryDescriptor memory(U c,unsigned member)const override{
  p::need(c==0,"original CTA");const auto&n=t_.at("nodes").at(member);bool global=n.at("kind")=="global";p::need(global||n.at("kind")=="shared","real source memory node");tiny_full::MemoryDescriptor d;d.path=global?tiny_full::Path::DirectGlobal:tiny_full::Path::Shared;d.write=n.at("write");g::ExplicitMemorySubop sub;std::unordered_set<U>seen;U mask=0,width=p::natural(n.at("width"));
  for(const auto&x:n.at("lanes")){U lane=p::natural(x[0]),address=p::natural(x[1]);p::need(lane<32&&!(mask&(U(1)<<lane)),"unique actual source lane");mask|=U(1)<<lane;sub.ranges.push_back({int(lane),address,width});sub.source_member_ordinals.push_back(int(lane));sub.requested_bytes+=width;
   if(global){const auto&b=call_.at("binding");std::string role=d.write?"output":p::natural(n.at("pc"))==0x600?"operand_a":"operand_b";U base=p::natural(b.at(role));p::need(base<=address&&p::add(address,width)<=p::add(base,512),"fixed source ABI operand extent");for(U line=address/128*128,last=(address+width-1)/128*128;;line+=128){g::CacheLineKey key{1,line};(void)mapper_.map(key);if(seen.insert(line).second)d.lines.push_back(key);if(line==last)break;}}
   else p::need(p::add(address,width)<=5120,"actual shared extent");
  }
  p::need(mask==p::natural(n.at("effective_mask"))&&mask,"exact active native memory lanes");
  if(global){d.global_bytes=sub.requested_bytes;d.global_subops.push_back(std::move(sub));}else{d.shared_bytes=sub.requested_bytes;d.shared_service=g::describe_explicit_sram_ranges(sub);d.shared_subops.push_back(std::move(sub));}return d;
 }
 J evidence()const override{return {{"schema","CURRENT_INITIAL_CUTLASS_SOURCE_DAG_V1"},{"native_launch_id",144},{"all_selected_source_PCs_preserved",true},{"global_records",8},{"shared_allocation_bytes",5120},{"barrier_rounds",5},{"HMMA_instructions",16},{"modeled_padded_tensor_FMA",32768},{"source_derived_control",true},{"dynamic_instruction_path_observed",false},{"numerical_tensor_values_evaluated",false},{"constant_cache_timing_modeled",false},{"implicit_register_dependencies_complete",false},{"current_occupancy_observed",false},{"resident_limit_assumption",1},{"compute_transfer_qualified",false},{"full_simulation_admitted",false}};}
};
}
