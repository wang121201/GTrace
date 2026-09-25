#pragma once
// Include after frozen canonical-full streaming TU. No old-PID validators change.
namespace current_index_copy {
using J=nlohmann::json;using U=std::uint64_t;namespace g=GTSim;
inline U checks=0;inline void need(bool v,const std::string&m){++checks;if(!v)throw std::runtime_error(m);}
inline U num(const J&j){need(j.is_number_integer()&&!j.is_boolean()&&(j.is_number_unsigned()||j.get<std::int64_t>()>=0),"natural value");return j.get<U>();}
inline U add(U a,U b){need(b<=UINT64_MAX-a,"address overflow");return a+b;}
struct Catalog {
 canonical_indexput::Model index;canonical_prefillcopy::Model copy;
 const coupling::ServiceMapper index_map,copy_map;
 Catalog(const J&p,const g::L2DramAddressMapper&base):index(p.at("legacy_envelopes").at("indexput")),copy(p.at("legacy_envelopes").at("copy")),index_map(index.service_map(),base,20ULL<<30),copy_map(copy.service_map(),base,20ULL<<30){}
};
class PreparedMemory {
 const Catalog&owner_;J current_;const J*old_=nullptr;const coupling::ServiceMapper&map_;
 std::unique_ptr<canonical_indexput::ModeledBuilder>ib_;std::unique_ptr<canonical_prefillcopy::ModeledBuilder>cb_;
public:
 PreparedMemory(const Catalog&owner,const J&current,const coupling::ServiceMapper&map):owner_(owner),current_(current),map_(map){
  const auto&b=current_.at("binding");const auto&a=current_.at("raw_argument_record");const bool ix=current_.at("family")=="indexput";need(ix||current_.at("family")=="copy","closed family");
  need(current_.at("schema")=="CURRENT_INDEX_COPY_EXECUTION_CALL_V1"&&current_.at("process")==J({{"pid",3011726},{"start_ticks",897090720}})&&b.at("process")==current_.at("process")&&a.at("native_launch_binding").at("process")==current_.at("process"),"strict current process");
  need(b.at("native_launch_id")==current_.at("native_launch_id")&&a.at("native_launch_binding").at("native_launch_id")==current_.at("native_launch_id")&&b.at("source_launch_key")==current_.at("source_launch_key")&&b.at("phase")==current_.at("phase"),"current launch identity");
  need(tiny_sha::sha256(a.dump())==current_.at("raw_argument_record_sha256").get<std::string>(),"complete current raw ABI evidence");
  const auto&calls=ix?owner.index.calls:owner.copy.calls;for(const J*c:calls)if(c->at("source_launch_key")==current_.at("source_model_key")){need(!old_,"unique immutable source call");old_=c;}need(old_,"known original source key");
  const std::string phase=current_.at("phase"),oldphase=old_->at("phase");need(phase=="Measured/"+oldphase||phase=="Warmup/"+oldphase,"exact source phase");
  for(const auto*k:{"code_sha256","parameter_layout_sha256","grid","block"})need(current_.at(k)==old_->at(k)&&current_.at(k)==b.at(k),std::string("closed original specialization ")+k);
  for(const auto*k:{"registers","dynamic_shared_bytes","static_shared_bytes","local_bytes_per_thread","binary_version","ptx_version","attribute_query_results"})need(current_.at("observed_static_resources").at(k)==old_->at("native_resources").at(k),"same actual resources; occupancy separate assumption");
  for(const auto*k:{"current_occupancy_observed","implicit_register_dependencies_complete","compute_transfer_qualified","full_simulation_admitted"})need(current_.at(k)==false,"no qualification upgrade");
  need(current_.at("source_occupancy_assumption")==12,"unchanged original resident assumption");
  need(current_.at("objects").size()==U(ix?3:2),"exact roles");
  for(const auto&kv:current_.at("objects").items()){
   const auto&o=kv.value();const auto&v=o.at("view");const auto&r=o.at("root");const auto&src=old_->at(kv.key());U ptr=num(o.at("pointer")),bytes=num(o.at("bytes")),base=num(r.at("base_address"));
   U offset=v.contains("storage_offset_bytes")?num(v.at("storage_offset_bytes")):num(v.at("storage_offset_elements"))*num(v.at("element_size"));
   need(ptr==num(v.at("data_address"))&&ptr==add(base,offset)&&add(ptr,bytes)==num(o.at("end_exclusive"))&&add(ptr,bytes)<=add(base,num(r.at("storage_nbytes"))),"complete typed current role/storage");
   U extent=num(v.at("element_size"));need(v.at("shape").size()==v.at("stride_bytes").size(),"typed axes");for(U i=0;i<v.at("shape").size();++i){U n=num(v.at("shape")[i]),stride=num(v.at("stride_bytes")[i]);need(n>0&&(!stride||(n-1)<=UINT64_MAX/stride),"typed extent overflow");extent=add(extent,(n-1)*stride);}need(extent==bytes,"actual full typed extent");
   need(o.at("source_pointer")==src.at("data_address")&&o.at("source_extent")==src.at("addressed_span_bytes")&&bytes>=num(o.at("source_extent"))&&ptr%128==num(o.at("source_pointer"))%128,"exact old role and full translated extent");
   J strides=v.at("stride_bytes");if(!ix&&kv.key()=="destination"){need(v.at("shape")==J::array({32,32,128})&&strides==J::array({8192,256,2}),"only contiguous actual ragged Q inner axes flatten");strides=J::array({8192,2});}need(strides==src.at("stride_bytes"),"same typed address strides");
   U actual=ix?num(b.at("ABI").at(kv.key()=="indices"?"index_pointer":kv.key()=="value_input"?"source":"destination")):num(b.at(kv.key()=="value_input"?"input":"output"));need(ptr==actual,"typed role pointer bound to actual raw ABI decoder");
  }
  if(ix){need(current_.at("source_role")==old_->at("role")&&b.at("actual_indices")==old_->at("recorded_output_cache_slots")&&b.at("ABI").at("index_size")==1281,"same actual indexed values/role, explicit larger capacity");for(const auto&i:b.at("actual_indices"))need(num(i)<257,"both old and current bounds true for every used slot");ib_=std::make_unique<canonical_indexput::ModeledBuilder>(owner.index,*old_,owner.index.source(*old_),owner.index_map,ctas());}
  else{need(b.at("N")==131072,"exact copy count");const auto&i=current_.at("objects").at("value_input");const auto&o=current_.at("objects").at("destination");need(num(i.at("end_exclusive"))<=num(o.at("pointer"))||num(o.at("end_exclusive"))<=num(i.at("pointer")),"actual copy no alias");cb_=std::make_unique<canonical_prefillcopy::ModeledBuilder>(owner.copy,*old_,owner.copy.source(),owner.copy_map,ctas());}
  visit([&](auto&builder){builder.fast_hash=true;builder.kernel_index=int(num(current_.at("native_launch_id")));});
 }
 template<class F>void visit(F&&f){if(ib_)f(*ib_);else f(*cb_);}
 U ctas()const{return num(current_.at("grid")[0]);}
 const J&current()const{return current_;}const J&original()const{return *old_;}
 int first_node(int c){int v=0;visit([&](auto&b){v=b.spans.at(c).first_node;});return v;}
 g::CtaGraphStore::Owned build_cta(int c){
  need(c>=0&&U(c)<ctas(),"strict full current CTA domain");g::CtaGraphStore::Owned ns;
  visit([&](auto&b){ns=b.build(c);need(ns.size()==b.registers.nodes.size(),"complete original modeled DAG");for(U i=0;i<ns.size();++i){const auto&r=b.registers.nodes.at(b.registers.order.at(i));auto&n=*ns[i];if(r.kind!="global")continue;
   const std::string role=ib_?owner_.index.records(*old_).at(r.memory).role:owner_.copy.records.at(r.memory).role;const auto&o=current_.at("objects").at(role);U oldptr=num(o.at("source_pointer")),newptr=num(o.at("pointer"));need(n.explicit_memory_subops.size()==1,"one original explicit subop");
   for(auto&range:n.explicit_memory_subops[0].ranges){need(range.byte_count>0&&range.offset_bytes>=oldptr&&add(range.offset_bytes,range.byte_count)<=add(oldptr,num(o.at("source_extent"))),"original Formula.role owns full source range");range.offset_bytes=add(newptr,range.offset_bytes-oldptr);need(add(range.offset_bytes,range.byte_count)<=num(o.at("end_exclusive")),"current whole range");for(U line=range.offset_bytes/128*128,last=(add(range.offset_bytes,range.byte_count)-1)/128*128;;line=add(line,128)){(void)map_.map(g::CacheLineKey{1,line});if(line==last)break;}}
  }});return ns;
 }
 void retire(int c,const std::vector<g::DAGNode*>&ns,g::Cycle cycle){visit([&](auto&b){b.retire(c,ns,cycle);});}
 // Catalog, base/old/current mappers and this owner outlive Fine/store/callbacks.
 // All node/control/shared/barrier/dual dependencies are created by old builders;
 // only GLOBAL ranges change by the original memory Formula.role's typed offset.
};
}
