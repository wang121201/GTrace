#pragma once
// Include after the frozen canonical streaming TU. No cache or scheduler changes.
namespace current_attention {
using namespace native_sequence;
struct Plan {
 J envelope;
 std::map<std::string,std::unique_ptr<attention_bridge::Model>> models;
 std::map<std::string,std::map<std::string,U>> max_ends;
 explicit Plan(const J&j):envelope(j){
  p::need(j.at("schema")=="CURRENT_ATTENTION_192_FINE_EXECUTION_PLAN_V1"&&j.at("call_count")==192&&j.at("calls").size()==192,"all current Attention192");
  p::need(j.at("process")==J({{"pid",3011726},{"start_ticks",897090720}})&&j.at("current_dynamic_compute_control_qualified")==false&&j.at("full_simulation_admitted")==false,"current source-transfer authority");
  for(const auto&item:j.at("templates").items())models.emplace(item.key(),std::make_unique<attention_bridge::Model>(attention_bridge::read_pin(item.value(),16ULL<<20)));
  p::need(models.size()==3,"three exact Attention source classes");
  for(const auto&[phase,owner]:models){const auto&m=*owner;for(int c=0;c<8;++c)for(const auto&n:m.nodes)for(const auto&r:m.global(c,n)){auto&end=max_ends[phase][m.objects[r.role].role];end=std::max(end,p::add(r.offset,r.width));}}
  for(const auto&item:j.at("calls").items())admit(item.value(),std::stoull(item.key()));
 }
 const J&call(U id)const{return envelope.at("calls").at(std::to_string(id));}
 const attention_bridge::Model&model(U id)const{return *models.at(call(id).at("template").get<std::string>());}
 void admit(const J&c,U id)const{
  const auto&b=c.at("binding");const std::string phase=c.at("template");const auto&m=*models.at(phase);const auto&control=envelope.at("control_contracts").at(phase);
  p::need(b.at("schema")=="P32_ATTENTION_CURRENT_SOURCE_BINDING_V1"&&b.at("native_launch_id")==id&&b.at("native_launch_binding").at("native_launch_id")==id&&b.at("process")==envelope.at("process")&&b.at("native_launch_binding").at("process")==b.at("process"),"current launch identity");
  p::need(tiny_sha::sha256(b.dump())==c.at("binding_sha256").get<std::string>(),"exact qualified current binding");
  p::need(b.at("phase")=="Warmup/"+phase||b.at("phase")=="Measured/"+phase,"exact source phase suffix");
  p::need(b.at("source_model_binding_qualified")==true&&b.at("current_native_memory_trace_qualified")==false&&b.at("current_occupancy_observed")==false&&b.at("full_model_admitted")==false,"source qualification not dynamic current execution proof");
  for(auto k:{"code_sha256","parameter_layout_sha256","grid","block","named_scalars","array_values","plan_info"})p::need(b.at(k)==control.at(k),"fixed active scalar/array control");
  for(auto k:{"code_sha256","grid","block"})p::need(b.at(k)==m.profile.at(k),"same frozen graph specialization");
  p::need(b.at("pointers").size()==control.at("pointer_nonnull").size(),"closed current pointer domain");for(const auto&x:control.at("pointer_nonnull").items())p::need((p::natural(b.at("pointers").at(x.key()))!=0)==x.value().get<bool>(),"null pointer control branches");
  p::need(c.at("source_resident_limit")==m.p.resident&&c.at("current_occupancy_observed")==false&&c.at("execution_route")=="Fine","source resident-cap reuse");
  for(auto k:{"registers","dynamic_shared_bytes","static_shared_bytes","local_bytes_per_thread","binary_version","ptx_version","attribute_query_results"})p::need(c.at("observed_static_resources").at(k)==m.profile.at("native_resources").at(k),"current observed static resources");
  for(const auto&o:m.objects){const auto&role=b.at("roles").at(o.role);U ptr=p::natural(role.at("pointer")),extent=p::natural(role.at("addressed_span_bytes"));p::need(ptr&&extent&&ptr==p::natural(b.at("pointers").at(o.role))&&ptr%128==o.pointer%128,"current role raw pointer and line alignment");p::need(extent<=UINT64_MAX-ptr,"current complete extent");
   p::need(max_ends.at(phase).at(o.role)<=extent,"all eight source CTA ranges within current role extent");
  }
 }
};
// Owns original Builder, borrows Plan/model and current mapper; both must outlive it.
// Original full compute/shared/barrier/copy graph is created first, then only GLOBAL VA rebased.
struct Binding:canonical_full::AttentionBuilder {
 const Plan&plan;const J&current;const coupling::ServiceMapper&mapper;
 tiny_sha::Sha256 current_addresses;U rebased_ranges=0,rebased_bytes=0;
 Binding(const Plan&p,U id,const coupling::ServiceMapper&map):canonical_full::AttentionBuilder(p.model(id)),plan(p),current(p.call(id).at("binding")),mapper(map){plan.admit(p.call(id),id);}
 void current_word(U v){std::array<char,8>b{};for(int i=0;i<8;++i)b[i]=char(v>>(8*i));current_addresses.add(b.data(),b.size());}
 g::CtaGraphStore::Owned build(int c){
  auto out=canonical_full::AttentionBuilder::build(c);p::need(c>=0&&c<8&&out.size()==m.nodes.size(),"unchanged full original CTA graph");
  for(U i=0;i<out.size();++i){auto&node=*out[i];const auto&t=m.nodes[i];if(t.kind!="global"&&t.kind!="async_copy")continue;
   p::need(node.explicit_memory_subops.size()==1,"one original exact global operand");auto&sub=node.explicit_memory_subops[0];const auto&ranges=m.global(c,t);p::need(sub.ranges.size()==ranges.size(),"same actual lane multiplicity");
   for(U k=0;k<ranges.size();++k){const auto&r=ranges[k];const auto&o=m.objects[r.role];auto&range=sub.ranges[k];const auto&co=current.at("roles").at(o.role);U ptr=p::natural(co.at("pointer")),extent=p::natural(co.at("addressed_span_bytes"));
    p::need(range.offset_bytes==p::add(o.pointer,r.offset)&&range.byte_count==r.width&&range.source_member_ordinal==r.lane,"original typed role/lane identity");p::need(r.offset<=extent&&r.width<=extent-r.offset,"current role whole width");U va=p::add(ptr,r.offset);range.offset_bytes=va;
    for(U line=va/128*128;;line+=128){(void)mapper.map({1,line});if(line==(va+r.width-1)/128*128)break;}
    for(U value:{U(c),i,U(r.lane),va,r.width})current_word(value);++rebased_ranges;rebased_bytes+=r.width;
   }
  }return out;
 }
 J evidence(){return {{"schema","CURRENT_ATTENTION_FINE_BRIDGE_EVIDENCE_V1"},{"native_launch_id",current.at("native_launch_id")},{"source_graph",receipt()},{"current_address_sha256",current_addresses.hex()},{"rebased_ranges",rebased_ranges},{"rebased_bytes",rebased_bytes},{"source_graph_or_dependencies_changed",false},{"source_shared_or_async_destination_changed",false},{"current_occupancy_observed",false},{"source_resident_limit",m.p.resident},{"current_dynamic_compute_control_qualified",false},{"full_simulation_admitted",false}};}
};
} // namespace current_attention
