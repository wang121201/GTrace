#pragma once
#include "../tilegen-tiny-full-r1/source.h"
#include "../tilegen-full-r1/canonical-mixed643-driver-r1/rotary_class_support.h"
#include "../tilegen-full-r1/canonical-rotary-driver-r1/model_plan.h"
#include <unordered_set>

namespace tiny_full {
// Model owns the original sealed class/body catalog. A call owns only one
// SourceNode array per used register class, never a per-CTA DAG or average class.
class RotaryBinding final:public KernelBinding {
 const canonical_rotary::Model& model_;J call_;
 const native_cta_sequence::SourceBundle& source_;
 const coupling::ServiceMapper& mapper_;U count_;
 std::vector<std::vector<SourceNode>> classes_;
 std::vector<J> class_census_;J evidence_;
 const native_register::Node& original(U cta,unsigned member)const{
  const auto& reg=source_.catalog.program(cta);p::need(member<reg.order.size(),"tiny Rotary source member");
  const auto& n=reg.nodes.at(reg.order.at(member));p::need(n.output==int(member),"tiny Rotary dense class topology");return n;
 }
 J prepare_class(U representative,std::vector<SourceNode>& dest){
  const auto& reg=source_.catalog.program(representative);const auto& body=source_.program.body(representative);
  U completion=0,issue=0,compute=0,read=0,write=0,sread=0,swrite=0,ranges=0,zero=0;J kinds=J::object();dest.reserve(reg.order.size());
  for(U member=0;member<reg.order.size();++member){const auto&n=original(representative,unsigned(member));SourceNode s;
   s.id=unsigned(member);s.warp=unsigned(n.warp);s.ordinal=unsigned(n.local);s.source_ordinal=U(n.local);s.pc=n.pc;s.mask=n.mask;s.write=n.op=='W';
   for(int d:n.completion)s.completion_dependencies.push_back(unsigned(d));
   for(int d:n.issue)s.issue_dependencies.push_back(unsigned(d));completion+=n.completion.size();issue+=n.issue.size();
   if(n.kind=="global"){
    const auto& mem=body.records.at(source_.catalog.record(representative,n));
    p::need(mem.op==n.op && mem.mask==n.mask,"tiny Rotary original class/body identity");
    if(mem.lanes.empty()){s.kind=Kind::Compute;s.pipeline="SIMD";s.compute_elements=U(n.elements);++zero;}
    else{s.kind=Kind::Global;s.pipeline=s.write?"ST":"LD";(s.write?write:read)=p::add(s.write?write:read,p::multiply(mem.lanes.size(),U(mem.width)));ranges+=mem.lanes.size();}
   }else if(n.kind=="shared"){
    s.kind=Kind::Shared;s.pipeline=s.write?"ST":"LD";(s.write?swrite:sread)=p::add(s.write?swrite:sread,p::multiply(n.lanes.size(),U(n.width)));ranges+=n.lanes.size();
   }else if(n.kind=="barrier"){s.kind=Kind::Barrier;s.pipeline="BARRIER";}
   else{p::need(n.kind=="compute"||n.kind=="control","tiny Rotary source kind");s.kind=n.kind=="compute"?Kind::Compute:Kind::Control;s.pipeline=n.pipeline;s.compute_elements=U(n.elements);}
   compute=p::add(compute,s.compute_elements);std::string k=s.kind==Kind::Global?"global":s.kind==Kind::Shared?"shared":s.kind==Kind::Barrier?"barrier":s.kind==Kind::Control?"control":"compute";
   if(!kinds.contains(k))kinds[k]=0;kinds[k]=kinds[k].get<U>()+1;dest.push_back(std::move(s));
  }
  p::need(read==body.read&&write==body.write,"tiny Rotary original representative byte census");
  return {{"nodes",dest.size()},{"completion_edges",completion},{"issue_edges",issue},{"scalar_compute_elements",compute},{"global_read_bytes",read},{"global_write_bytes",write},{"shared_read_bytes",sread},{"shared_write_bytes",swrite},{"explicit_ranges",ranges},{"zero_lane_global_converted_to_SIMD",zero},{"declared_tensor_FMA",0},{"async_copies",0},{"zero_source_async_copies",0},{"kinds",kinds}};
 }
public:
 RotaryBinding(const canonical_rotary::Model& model,const J& call,U count,const coupling::ServiceMapper& mapper)
  :model_(model),call_(call),source_(model.source(call_)),mapper_(mapper),count_(count){
  bool found=false;for(const auto*c:model_.calls)if(c->at("source_launch_key")==call_.at("source_launch_key")){p::need(!found&&*c==call_,"tiny Rotary selected canonical identity");found=true;}
  p::need(found&&count_>0&&count_<=source_.program.ctas&&source_.program.ctas==p::product3(call_.at("grid"),10000000),"tiny Rotary flattened selected CTA count");
  p::need(source_.program.warps==4&&source_.resident==12,"tiny Rotary original resources");
  classes_.resize(source_.catalog.classes.size());class_census_.resize(classes_.size());
  std::vector<U> population(classes_.size(),0),representatives(classes_.size(),std::numeric_limits<U>::max());
  for(U c=0;c<count_;++c){const U id=U(source_.catalog.bindings.at(c).class_id);if(population.at(id)++==0)representatives[id]=c;}
  for(U id=0;id<classes_.size();++id)if(population[id])class_census_[id]=prepare_class(representatives[id],classes_[id]);
  J totals=J::object();for(auto key:{"nodes","completion_edges","issue_edges","scalar_compute_elements","global_read_bytes","global_write_bytes","shared_read_bytes","shared_write_bytes","explicit_ranges","zero_lane_global_converted_to_SIMD","declared_tensor_FMA","async_copies","zero_source_async_copies"})totals[key]=0;
  J bindings=J::array(),class_rows=J::array();
  for(U c=0;c<count_;++c){const auto& b=source_.catalog.bindings.at(c);const auto& census=class_census_.at(b.class_id);const auto& span=source_.catalog.spans.at(c);
   // Catalog has already checked every class/body PC, function, mask, width,
   // lane-mask and direction. Keep each original body/span and sum exact work.
   p::need(census.at("nodes")==b.nodes&&census.at("global_read_bytes")==b.read&&census.at("global_write_bytes")==b.write,"tiny Rotary class/body census invariant");
   p::need(census.at("completion_edges").get<U>()+census.at("issue_edges").get<U>()==b.edges,"tiny Rotary original typed census");
   for(auto it=totals.begin();it!=totals.end();++it)it.value()=p::add(it.value().get<U>(),census.at(it.key()).get<U>());
   bindings.push_back({{"CTA",c},{"class_id",b.class_id},{"body_index",b.body_index},{"first_node",span.first_node},{"nodes",b.nodes},{"global_read_bytes",b.read},{"global_write_bytes",b.write}});
  }
  p::need(totals.at("nodes")==source_.catalog.sum(count_,&native_cta_class::Binding::nodes)&&totals.at("global_read_bytes")==source_.catalog.sum(count_,&native_cta_class::Binding::read)&&totals.at("global_write_bytes")==source_.catalog.sum(count_,&native_cta_class::Binding::write),"tiny Rotary selected original totals");
  for(U id=0;id<classes_.size();++id)if(population[id])class_rows.push_back({{"class_id",id},{"representative_CTA",representatives[id]},{"selected_CTAs",population[id]},{"input_warp_program_sha256",source_.catalog.classes[id]->input_warp_program_sha256},{"per_CTA",class_census_[id]}});
  evidence_={{"schema","TINY_ROTARY_CLASS_SOURCE_EVIDENCE_V1"},{"family","Rotary"},{"source_launch_key",call_.at("source_launch_key")},{"canonical_call",call_},{"selected_CTAs",count_},{"full_source_CTAs",source_.program.ctas},{"warps_per_CTA",source_.program.warps},{"native_resident_limit",source_.resident},{"nodes_per_CTA",nullptr},{"classes",class_rows},{"CTA_bindings",bindings},{"selected_totals",totals},{"register_source_pin",source_.input.at("register_file")},{"memory_source_pin",source_.input.at("program_file")},{"source_phase",call_.at("phase")},{"original_model_validation_preserved",true},{"per_CTA_DAGNode_allocation",false},{"class_average_used",false},{"estimated_compute",true},{"estimated_address",true},{"native_hardware_timing_qualified",false},{"implicit_register_dependencies_complete",false},{"implicit_descriptor_base",call_.at("implicit_descriptor_base")},{"implicit_descriptor_width",call_.at("implicit_descriptor_width")},{"full_trace_saved",false},{"addresses","original Rotary Model::address with actual CTA body/record and every lane range; first-touch128B order"}};
 }
 RotaryBinding(const RotaryBinding&)=delete;RotaryBinding&operator=(const RotaryBinding&)=delete;
 U ctas()const override{return count_;}
 unsigned warps(U c)const override{p::need(c<count_,"tiny Rotary warp CTA");return unsigned(source_.program.warps);}
 unsigned resident_limit()const override{return unsigned(source_.resident);}
 U first_node(U c)const override{p::need(c<count_,"tiny Rotary span CTA");return U(source_.catalog.spans.at(c).first_node);}
 U template_class(U c)const override{p::need(c<count_,"tiny Rotary class CTA");return U(source_.catalog.bindings.at(c).class_id);}
 std::span<const SourceNode>nodes(U c)const override{return classes_.at(template_class(c));}
 MemoryDescriptor memory(U c,unsigned member)const override{
  p::need(c<count_&&member<nodes(c).size(),"tiny Rotary memory bounds");const auto& n=original(c,member);const auto kind=nodes(c)[member].kind;p::need(kind==Kind::Global||kind==Kind::Shared,"tiny Rotary nonmemory member");
  MemoryDescriptor out;out.write=n.op=='W';out.path=kind==Kind::Global?Path::DirectGlobal:Path::Shared;g::ExplicitMemorySubop sub;
  if(kind==Kind::Global){const auto& mem=source_.program.body(c).records.at(source_.catalog.record(c,n));sub.requested_bytes=p::multiply(mem.lanes.size(),U(mem.width));std::unordered_set<U> seen;
   for(const auto&lane:mem.lanes){const U va=model_.address(call_,c,U(n.warp),U(lane.lane),mem.pc,U(mem.width));sub.ranges.push_back({lane.lane,va,U(mem.width)});sub.source_member_ordinals.push_back(lane.lane);
    const U last=(va+mem.width-1)/128*128;for(U line=va/128*128;;line+=128){const g::CacheLineKey key{1,line};(void)mapper_.map(key);if(seen.insert(line).second)out.lines.push_back(key);if(line==last)break;}
   }
   out.global_bytes=sub.requested_bytes;out.global_subops.push_back(std::move(sub));
  }else{sub.requested_bytes=p::multiply(n.lanes.size(),U(n.width));for(auto[lane,address]:n.lanes){sub.ranges.push_back({lane,address,U(n.width)});sub.source_member_ordinals.push_back(lane);}out.shared_bytes=sub.requested_bytes;out.shared_service=g::describe_explicit_sram_ranges(sub);out.shared_subops.push_back(std::move(sub));}
  return out;
 }
 J evidence()const override{return evidence_;}
};
} // namespace tiny_full
