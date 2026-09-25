#pragma once
namespace current_rotary {
using namespace native_sequence;
class Binding final:public tiny_full::KernelBinding {
 J old_,current_;std::unique_ptr<tiny_full::RotaryBinding>graph_;const coupling::ServiceMapper&map_;
public:
 static void admit(const J&c,const J&old){
  p::need(c.at("schema")=="CURRENT_ROTARY_TINY_BINDING_V1"&&c.at("process")==J({{"pid",3011726},{"start_ticks",897090720}}),"current rotary process/schema");
  p::need(c.at("legacy_source_key")==old.at("source_launch_key")&&(c.at("phase")=="Warmup/"+old.at("phase").get<std::string>()||c.at("phase")=="Measured/"+old.at("phase").get<std::string>()),"original phase source");
  for(auto k:{"code_sha256","parameter_layout_sha256","grid","block","positions","model_counts"})p::need(c.at(k)==old.at(k),"same rotary source specialization");
  p::need(c.at("current_occupancy_observed")==false&&c.at("current_dynamic_compute_control_qualified")==false&&c.at("full_simulation_admitted")==false&&c.at("reused_source_occupancy_limit")==12,"current qualification retained");
  for(const auto&x:c.at("observed_static_resources").items())p::need(x.value()==old.at("native_resources").at(x.key()),"source static resource");
  p::need(c.at("arguments").size()==old.at("arguments").size()&&c.at("objects").size()==6,"complete current roles and fields");
  for(const auto&a:old.at("arguments").items()){
   if(!c.at("objects").contains(a.key()))p::need(c.at("arguments").at(a.key())==a.value(),"source scalar unchanged");
   else{const auto&o=c.at("objects").at(a.key());const auto&prior=old.at("objects").at(a.key());U va=p::natural(c.at("arguments").at(a.key())),sz=p::natural(o.at("span_bytes"));
    p::need(va&&va==p::natural(o.at("pointer"))&&p::add(va,sz)==p::natural(o.at("end_exclusive"))&&sz==p::natural(prior.at("span_bytes"))&&o.at("shape")==prior.at("shape"),"current complete role span");
    p::need(va>=p::natural(o.at("root").at("base_address"))&&p::add(va,sz)<=p::add(p::natural(o.at("root").at("base_address")),p::natural(o.at("root").at("storage_nbytes"))),"current role root capacity");
   }
  }
  p::need(c.at("arguments").at("q")==c.at("arguments").at("q_rope")&&c.at("arguments").at("k")==c.at("arguments").at("k_rope"),"exact current inplace aliases");
  const auto&w=c.at("position_witness");p::need(w.at("process")==c.at("process")&&w.at("native_launch_id")==c.at("native_launch_id")&&w.at("position_pointer")==c.at("arguments").at("pos_ids")&&w.at("values")==c.at("positions"),"current retained position witness");
 }
 Binding(const canonical_rotary::Model&m,const J&old,const J&current,const coupling::ServiceMapper&oldmap,const coupling::ServiceMapper&map):old_(old),current_(current),map_(map){admit(current_,old_);graph_=std::make_unique<tiny_full::RotaryBinding>(m,old_,p::product3(current_.at("grid"),10000000),oldmap);}
 U ctas()const override{return graph_->ctas();}unsigned warps(U c)const override{return graph_->warps(c);}unsigned resident_limit()const override{return graph_->resident_limit();}U first_node(U c)const override{return graph_->first_node(c);}U template_class(U c)const override{return graph_->template_class(c);}std::span<const tiny_full::SourceNode>nodes(U c)const override{return graph_->nodes(c);}
 tiny_full::MemoryDescriptor memory(U c,unsigned member)const override{
  auto out=graph_->memory(c,member);if(out.path==tiny_full::Path::Shared)return out;
  p::need(out.path==tiny_full::Path::DirectGlobal,"original Rotary global path");
  // Exact role selection from the original canonical_rotary::Model::address;
  // never infer ownership from overlapping q/k tensor bounding boxes.
  U pc=nodes(c)[member].pc;std::string role;
  if(pc==0x120)role="pos_ids";
  else if(pc==0x310||pc==0x320||pc==0x330||pc==0x340)role="cos_sin_cache";
  else{p::need(pc==0x400||pc==0x5a0||pc==0x810||pc==0x8c0||pc==0xa60||pc==0xcd0,"original Rotary tensor PC");role=c/p::natural(old_.at("grid")[0])<32?"q":"k";if(pc==0x810||pc==0xcd0)role+="_rope";}
  const auto&prior=old_.at("objects").at(role);const auto&now=current_.at("objects").at(role);out.lines.clear();std::unordered_set<U>seen;
  for(auto&sub:out.global_subops)for(auto&r:sub.ranges){U before=r.offset_bytes;
   p::need(before>=p::natural(prior.at("pointer"))&&p::add(before,r.byte_count)<=p::natural(prior.at("end_exclusive")),"original role bounds");
   U va=p::add(p::natural(now.at("pointer")),before-p::natural(prior.at("pointer")));p::need(p::add(va,r.byte_count)<=p::natural(now.at("end_exclusive")),"current complete source width");r.offset_bytes=va;
   U last=(va+r.byte_count-1)/128*128;for(U line=va/128*128;;line+=128){g::CacheLineKey key{1,line};(void)map_.map(key);if(seen.insert(line).second)out.lines.push_back(key);if(line==last)break;}
  }return out;
 }
 J evidence()const override{return {{"schema","CURRENT_ROTARY_TINY_EVIDENCE_V1"},{"current_call",current_},{"original_source_DAG",graph_->evidence()},{"source_class_averaged",false},{"current_hardware_timing_qualified",false},{"legacy_PID_guard_changed",false},{"full_simulation_admitted",false}};}
};
}
