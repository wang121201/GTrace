#pragma once
// The original binding owns all graph/control/shared behavior and remains unchanged.
namespace current_elementwise {
using namespace native_sequence;
class Binding final:public tiny_full::KernelBinding {
 J current_,old_;
 std::unique_ptr<tiny_full::KernelBinding> graph_;
 const coupling::ServiceMapper& mapper_;
 static U raw(const J&r){
  const std::string text=r.at("raw_hex"),sha=r.at("sha256");U size=p::natural(r.at("bytes"));
  p::need((size==4||size==8)&&text.size()==size*2,"elementwise exact raw parameter width");
  std::string bytes;U out=0;auto nibble=[](char c)->U{if(c>='0'&&c<='9')return U(c-'0');if(c>='a'&&c<='f')return U(c-'a'+10);throw std::runtime_error("noncanonical raw ABI hex");};
  for(U i=0;i<size;++i){U b=(nibble(text[2*i])<<4)|nibble(text[2*i+1]);bytes.push_back(char(b));out|=b<<(8*i);}
  p::need(tiny_sha::sha256(bytes)==sha,"elementwise parameter SHA");return out;
 }
public:
 static void admit(const J&c,const J&old){
  p::need(c.at("schema")=="CURRENT_ELEMENTWISE_TINY_CALL_V1"&&c.at("process")==J({{"pid",3011726},{"start_ticks",897090720}}),"current elementwise capture process");
  p::need(c.at("native_launch_binding").at("process")==c.at("process")&&c.at("legacy_source_key")==old.at("source_launch_key"),"separate current launch/source identity");
  const std::string phase=c.at("phase"),oldphase=old.at("phase"),family=c.at("family");
  p::need((phase=="Warmup/"+oldphase||phase=="Measured/"+oldphase)&&c.at("layer")==old.at("layer")&&c.at("module_scope")==old.at("module_scope"),"exact phase/module specialization");
  for(auto k:{"code_sha256","parameter_layout_sha256","grid","block","context_id","stream_u64"})p::need(c.at(k)==old.at(k),"same native source specialization");
  p::need(c.at("current_occupancy_observed")==false&&c.at("current_dynamic_compute_control_qualified")==false&&c.at("full_simulation_admitted")==false,"retain current transfer boundary");
  p::need(c.at("reused_source_occupancy_limit")==old.at("native_resources").at("max_active_blocks_per_sm"),"declared old source occupancy assumption");
  for(auto k:{"registers","dynamic_shared_bytes","static_shared_bytes","local_bytes_per_thread","binary_version","ptx_version","attribute_query_results"})p::need(c.at("observed_static_resources").at(k)==old.at("native_resources").at(k),"actual static resource equality");
  std::vector<std::string>names;U pointers=0;
  if(family=="rmsnorm"){names={"input","weight","output","hidden_size","input_stride_elements","output_stride_elements","weight_bias_bits","eps_bits"};pointers=3;}
  else if(family=="fused_norm"){names={"input","residual","weight","d","input_stride","residual_stride","weight_bias_bits","eps_bits"};pointers=3;}
  else{p::need(family=="silu","supported elementwise family");names={"out","input","d"};pointers=2;}
  p::need(c.at("raw_parameters").size()==names.size()&&c.at("arguments").size()==names.size()&&c.at("objects").size()==pointers,"closed typed ABI fields");
  for(U i=0;i<names.size();++i){const auto&r=c.at("raw_parameters")[i];p::need(r.at("name")==names[i]&&r.at("bytes")==J(i<pointers?8:4),"actual parameter order/type");U v=raw(r);p::need(v==p::natural(c.at("arguments").at(names[i])),"raw parameter/decoded value");if(i>=pointers)p::need(c.at("arguments").at(names[i])==old.at("arguments").at(names[i]),"all active scalar bits exact");}
  for(const auto&item:c.at("objects").items()){
   const auto&x=item.value();const auto&before=old.at("objects").at(item.key());U base=p::natural(x.at("pointer")),bytes=p::natural(x.at("bytes"));
   p::need(base&&base==p::natural(c.at("arguments").at(item.key()))&&bytes==p::natural(before.at("bytes"))&&base%128==p::natural(before.at("pointer"))%128,"current pointer/extent/alignment");
   const J& shape=before.contains("shape")?before.at("shape"):before.at("descriptor").at("shape");const J& stride=before.contains("stride_bytes")?before.at("stride_bytes"):before.at("descriptor").at("stride_bytes");
   p::need(x.at("shape")==shape&&x.at("stride_bytes")==stride,"current complete typed shape/stride");
   p::need(p::add(base,bytes)==p::natural(x.at("end_exclusive"))&&p::add(p::natural(x.at("root").at("base_address")),p::natural(x.at("storage_offset_bytes")))==base&&p::add(base,bytes)<=p::add(p::natural(x.at("root").at("base_address")),p::natural(x.at("root").at("storage_nbytes"))),"current typed root bound");
   p::need(x.at("logical_identity")=="3011726:897090720:"+x.at("root").at("id").get<std::string>(),"current object process identity");
  }
  for(const auto&x:c.at("objects").items())for(const auto&y:c.at("objects").items())if(x.key()!=y.key()){
   const auto&a=x.value();const auto&b=y.value();const auto&oa=old.at("objects").at(x.key());const auto&ob=old.at("objects").at(y.key());
   bool overlap=p::natural(a.at("pointer"))<p::natural(b.at("end_exclusive"))&&p::natural(b.at("pointer"))<p::natural(a.at("end_exclusive"));
   bool old_overlap=p::natural(oa.at("pointer"))<p::natural(ob.at("end_exclusive"))&&p::natural(ob.at("pointer"))<p::natural(oa.at("end_exclusive"));
   p::need(!overlap&&!old_overlap,"this fixed family requires disjoint role extents; in-place writes reuse the same role");
  }
 }
 Binding(std::unique_ptr<tiny_full::KernelBinding>graph,const J&old,const J&current,const coupling::ServiceMapper&map):current_(current),old_(old),graph_(std::move(graph)),mapper_(map){
  admit(current_,old_);p::need(graph_&&graph_->ctas()==p::natural(current_.at("grid")[0])&&graph_->resident_limit()==p::natural(current_.at("reused_source_occupancy_limit")),"original full-grid source binding");
 }
 U ctas()const override{return graph_->ctas();}unsigned warps(U c)const override{return graph_->warps(c);}unsigned resident_limit()const override{return graph_->resident_limit();}U first_node(U c)const override{return graph_->first_node(c);}std::span<const tiny_full::SourceNode>nodes(U c)const override{return graph_->nodes(c);}U template_class(U c)const override{return graph_->template_class(c);}
 tiny_full::MemoryDescriptor memory(U c,unsigned member)const override{
  auto out=graph_->memory(c,member);if(out.path==tiny_full::Path::Shared)return out;
  p::need(out.path==tiny_full::Path::DirectGlobal&&out.shared_subops.empty(),"existing elementwise memory path");std::set<U>lines;
  for(auto&sub:out.global_subops)for(auto&range:sub.ranges){
   const U oldva=range.offset_bytes,end=p::add(oldva,range.byte_count);const J*target=nullptr;U offset=0;
   for(const auto&item:old_.at("objects").items()){const auto&o=item.value();if(oldva>=p::natural(o.at("pointer"))&&end<=p::natural(o.at("end_exclusive"))){p::need(!target,"one original typed role owns this complete range");target=&current_.at("objects").at(item.key());offset=oldva-p::natural(o.at("pointer"));}}
   p::need(target&&range.byte_count,"original global range must have typed owner");const U va=p::add(p::natural(target->at("pointer")),offset);p::need(p::add(va,range.byte_count)<=p::natural(target->at("end_exclusive")),"rebound complete width extent");range.offset_bytes=va;
   const U last=(p::add(va,range.byte_count)-1)/128*128;for(U line=va/128*128;;line+=128){(void)mapper_.map({1,line});lines.insert(line);if(line==last)break;}
  }
  out.lines.clear();for(U line:lines)out.lines.push_back({1,line});return out;
 }
 J evidence()const override{return {{"schema","CURRENT_ELEMENTWISE_TINY_BRIDGE_EVIDENCE_V1"},{"current_call",current_},{"original_source_DAG",graph_->evidence()},{"legacy_Model_admitted_current_PID",false},{"global_translation","original complete typed-role relative offsets"},{"shared_control_barrier_graph_changed",false},{"current_occupancy_observed",false},{"full_simulation_admitted",false}};}
};
} // namespace current_elementwise
