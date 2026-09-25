#pragma once
// Current call admission is separate from the immutable, old-PID source Model.
namespace current_gemv {
using namespace native_sequence;
struct Plan {
 J envelope;
 canonical_gemv::Model source_model;
 std::map<std::string,const J*> sources;
 std::map<U,const J*> current;
 static void check_call(const canonical_gemv::Model&m,const J&c,const J&old,const J&process){
  p::need(c.at("schema")=="CURRENT_GEMV_TINY_CHECKED_CALL_V1"&&c.at("process")==process,"current checked call/process");
  const auto&nb=c.at("native_launch_binding");p::need(nb.at("process")==process&&nb.at("native_launch_id")==c.at("native_launch_id")&&nb.at("source_launch_key")==c.at("source_launch_key"),"current native identity");
  p::need(c.at("source_template_target_key")==old.at("source_launch_key")&&c.at("module_scope")==old.at("module_scope")&&c.at("layer")==old.at("layer"),"old template target role/layer");
  auto phase=c.at("phase").get<std::string>();auto slash=phase.find('/');p::need(slash!=std::string::npos&&(phase.substr(0,slash)=="Warmup"||phase.substr(0,slash)=="Measured")&&phase.substr(slash+1)==old.at("phase").get<std::string>().substr(old.at("phase").get<std::string>().find('/')+1),"current phase suffix source");
  for(auto k:{"code_sha256","parameter_layout_sha256","grid","block","template_key","model_counts"})p::need(c.at(k)==old.at(k),"current exact source specialization/work");
  p::need(c.at("current_occupancy_observed")==false&&c.at("reused_source_occupancy_limit")==old.at("native_resources").at("max_active_blocks_per_sm")&&c.at("current_dynamic_compute_or_control_observed")==false&&c.at("native_target_qualified")==false&&c.at("driver_admitted")==false,"source resource/compute assumption remains explicit");
  const auto&nr=c.at("observed_static_resources");p::need(nr.size()==7,"all seven queried resource fields");for(auto k:{"dynamic_shared_bytes","static_shared_bytes","registers","local_bytes_per_thread","binary_version","ptx_version","attribute_query_results"})p::need(nr.at(k)==old.at("native_resources").at(k),"actual current static resource matches source");
  const auto&w=c.at("argument_words");p::need(w.size()==38,"actual152B ABI");std::set<int>ptr{0,1,4,5,8,9,12,13};for(int i=0;i<38;++i){p::natural(w[i],UINT32_MAX);if(!ptr.count(i))p::need(w[i]==old.at("argument_words")[i],"all120 nonpointer bytes");}
  p::need(w[8]==w[12]&&w[9]==w[13],"actual C/Y alias");p::need(c.at("objects").size()==3,"complete three-role object domain");int wi=0;
  for(auto role:{"weight","input","output"}){
   const auto&o=c.at("objects").at(role);const auto&before=old.at("objects").at(role);U base=p::natural(w[wi])+(p::natural(w[wi+1])<<32);wi+=4;
   p::need(base>0&&base==p::natural(o.at("pointer"))&&o.at("bytes")==before.at("bytes")&&o.at("shape")==before.at("shape")&&o.at("stride_bytes")==before.at("stride_bytes"),"current VA/shape/stride/complete extent");
   p::need(p::add(base,p::natural(o.at("bytes")))==p::natural(o.at("end_exclusive"))&&base%128==p::natural(m.tpl(old).at("pointer_alignment_mod128").at(role)),"current aligned full extent");
   if(o.at("tensor_hook_observed")==true){p::need(o.at("evidence_kind")=="ACTUAL_CURRENT_TYPED_VIEW"&&p::add(p::natural(o.at("root").at("base_address")),p::natural(o.at("storage_offset_bytes")))==base&&p::natural(o.at("end_exclusive"))<=p::add(p::natural(o.at("root").at("base_address")),p::natural(o.at("root").at("storage_nbytes"))),"current typed root bound");}
   else{
    p::need(c.at("module_scope")=="logits_processor"&&o.at("evidence_kind")=="ABI_AND_ADJACENT_CURRENT_CONSUMER_PRODUCER","only explicit current head scratch derivation");
    const auto&d=o.at("derivation");const auto&b=d.at("binding");p::need(b.at("process")==process&&b.at("module_call_id")==c.at("module_call_id")&&b.at("stream_u64")==c.at("stream_u64"),"adjacent scratch same process/call/stream");
    if(std::string(role)=="output")p::need(d.at("kind")=="CURRENT_BF16_CY_TO_FOLLOWING_FP32_CAST"&&p::natural(d.at("related_native_launch_id"))==p::natural(c.at("native_launch_id"))+1&&b.at("schema")=="CURRENT_HEAD_CAST_NATIVE_BINDING_V1"&&b.at("input")==o.at("pointer")&&b.at("N")==128256&&o.at("bytes")==256512,"current BF16 output-to-cast extent");
    else p::need(std::string(role)=="input"&&phase.substr(slash+1)=="Prefill"&&d.at("kind")=="CURRENT_LAST_TOKEN_GATHER_TO_GEMV"&&p::natural(d.at("related_native_launch_id"))+1==p::natural(c.at("native_launch_id"))&&b.at("schema")=="CURRENT_LAST_TOKEN_GATHER_BINDING_V1"&&b.at("output")==o.at("pointer")&&b.at("N")==4096&&b.at("index_value")==31&&o.at("bytes")==8192,"current Prefill last-token scratch extent");
   }
  }
 }
 explicit Plan(const J&in):envelope(in),source_model(in.at("source_model_envelope")){
  p::need(in.at("schema")=="CURRENT_GEMV_TINY_BRIDGE_PLAN_V1"&&in.at("status")=="SOURCE_READY_ALL518_CURRENT_GEMV"&&in.at("process")==J({{"pid",3011726},{"start_ticks",897090720}}),"specific closed current metadata cohort");
  p::need(in.at("full_model_admitted")==false&&in.at("current_occupancy_observed")==false&&source_model.calls.size()==259&&in.at("calls").size()==518,"full two-stage call domain; no timing admission");
  for(auto*c:source_model.calls)p::need(sources.emplace(c->at("source_launch_key"),c).second,"unique original source call");
  std::map<std::string,U>uses,phases;std::set<std::string>templates;
  for(const auto&c:envelope.at("calls")){const auto&old=*sources.at(c.at("source_template_target_key"));check_call(source_model,c,old,in.at("process"));p::need(current.emplace(p::natural(c.at("native_launch_id")),&c).second,"unique current call");++uses[c.at("source_template_target_key")];++phases[c.at("phase")];templates.insert(c.at("template_key"));}
  p::need(uses.size()==259&&templates.size()==11&&phases.size()==6,"all source templates and six phases");for(auto[k,n]:uses)p::need(n==2,"each old source target used twice");for(auto stage:{"Warmup","Measured"})for(auto suffix:{"Prefill","Decode1","Decode2"})p::need(phases.at(std::string(stage)+"/"+suffix)==(std::string(suffix)=="Prefill"?1:129),"complete current phase census");
 }
 const J&call(U id)const{return *current.at(id);}
 const J&old(U id)const{return *sources.at(call(id).at("source_template_target_key"));}
};
class CurrentGemvBinding final:public tiny_full::KernelBinding {
 const J&current_;tiny_full::GemvBinding graph_;packet_binding::CallBinding request_;
public:
 CurrentGemvBinding(const Plan&p,U id,const coupling::ServiceMapper&map):current_(p.call(id)),graph_(p.source_model,p.old(id),p::natural(current_.at("grid")[0]),map),request_(p.source_model,current_,map){}
 U ctas()const override{return graph_.ctas();}unsigned warps(U c)const override{return graph_.warps(c);}unsigned resident_limit()const override{return graph_.resident_limit();}U first_node(U c)const override{return graph_.first_node(c);}std::span<const tiny_full::SourceNode>nodes(U c)const override{return graph_.nodes(c);}U template_class(U c)const override{return graph_.template_class(c);}
 tiny_full::MemoryDescriptor memory(U c,unsigned member)const override{
  p::need(c<ctas()&&member<nodes(c).size(),"current bridge memory domain");auto old=request_.memory(c,member);p::need(!old.zero_lane_compute,"zero lanes remain source SIMD node");tiny_full::MemoryDescriptor out;out.write=old.write;out.path=old.global?tiny_full::Path::DirectGlobal:tiny_full::Path::Shared;
  if(old.global){out.global_bytes=old.logical_bytes;out.global_subops=std::move(old.subops);out.lines=std::move(old.lines);}else{out.shared_bytes=old.logical_bytes;out.shared_subops=std::move(old.subops);out.shared_service=old.shared_service;}return out;
 }
 J normalized_global(U c,unsigned member)const{
  const auto&n=request_.node(member);p::need(n.kind=="global","actual global node");const auto&r=request_.source.program.body(0).records.at(n.memory);const auto&groups=request_.source.range_plan.records.at(n.memory);auto md=memory(c,member);const auto&sub=md.global_subops.at(0);const auto*s=sub.source_semantics;
  p::need(s&&s->pc==n.pc&&s->warp==unsigned(n.warp)&&s->effective_mask==n.mask&&groups.size()==sub.ranges.size(),"actual source semantics/range cardinality");J lanes=J::array();std::vector<std::pair<int,U>>ordered;U bytes=0;
  for(U gi=0;gi<groups.size();++gi){const auto&g=groups[gi];const auto&range=sub.ranges[gi];p::need(range.byte_count==g.count*U(r.width),"coalesced lane multiplicity");for(U k=0;k<g.count;++k){const auto&lane=r.lanes.at(g.first+k);U addr=p::add(range.offset_bytes,k*U(r.width));p::need(addr==request_.model.address(current_,lane,r.width,c),"actual PreparedMemory equals source JSON oracle");ordered.emplace_back(lane.lane,addr);bytes+=r.width;}}
  std::sort(ordered.begin(),ordered.end());for(auto[lane,addr]:ordered)lanes.push_back({lane,addr});p::need(ordered.size()==r.lanes.size()&&bytes==md.global_bytes&&bytes==sub.requested_bytes&&sub.source_member_ordinals.size()==ordered.size(),"all original lanes/bytes preserved");
  for(U i=0;i<ordered.size();++i)p::need(sub.source_member_ordinals[i]==ordered[i].first,"source lane order preserved");
  return J::array({n.warp,n.pc,*s->raw_opcode,md.write?"WRITE":"READ",r.width,s->effective_mask,lanes});
 }
 J evidence()const override{return {{"schema","CURRENT_GEMV_TINY_BRIDGE_EVIDENCE_V1"},{"native_launch_id",current_.at("native_launch_id")},{"source_template_target_key",current_.at("source_template_target_key")},{"source_DAG",graph_.evidence()},{"legacy_model_admitted_current_PID",false},{"current_occupancy_observed",false},{"source_occupancy_limit_reused",resident_limit()},{"current_dynamic_compute_control_qualified",false}};}
};
}
