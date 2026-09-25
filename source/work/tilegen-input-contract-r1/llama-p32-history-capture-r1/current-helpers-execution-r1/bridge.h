#pragma once
// Sealed helper Model owns the original full execution DAG; current admission is separate.
namespace current_helpers {
using namespace native_sequence;
class Binding final:public tiny_full::KernelBinding {
 const helper_bridge::Model&m_;J current_;const coupling::ServiceMapper&mapper_;
 std::vector<std::vector<tiny_full::SourceNode>>classes_;
 static std::vector<U> raw(const J&a){const std::string h=a.at("raw_bytes_hex");p::need(h.size()==2*p::natural(a.at("size_bytes")),"raw helper size");std::vector<U>v;std::string bytes;auto nib=[](char c)->U{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("raw canonical hex");};for(U i=0;i<h.size();i+=2){U x=(nib(h[i])<<4)|nib(h[i+1]);v.push_back(x);bytes.push_back(char(x));}p::need(tiny_sha::sha256(bytes)==a.at("sha256").get<std::string>(),"raw helper SHA");return v;}
 static U word(const std::vector<U>&b,U off){p::need(off+8<=b.size(),"raw helper pointer field");U x=0;for(U i=0;i<8;++i)x|=b[off+i]<<(8*i);return x;}
public:
 static void admit(const J&c,const helper_bridge::Model&m){
  p::need(c.at("schema")=="CURRENT_TAIL_HELPER_CALL_V1"&&c.at("process")==J({{"pid",3011726},{"start_ticks",897090720}}),"current helper process");
  p::need(c.at("source_model_key")==m.key&&c.at("code_sha256")==m.call.at("code_sha256")&&c.at("grid")==m.call.at("grid")&&c.at("block")==m.call.at("block"),"same sealed helper code/geometry");
  for(auto k:{"registers","dynamic_shared_bytes","static_shared_bytes","local_bytes_per_thread","binary_version","ptx_version","attribute_query_results"})p::need(c.at("observed_static_resources").at(k)==m.call.at("native_resources").at(k),"exact queried helper resources");
  p::need(c.at("source_occupancy_assumption")==m.resident&&c.at("current_occupancy_observed")==false&&c.at("implicit_register_dependencies_complete")==false&&c.at("compute_transfer_qualified")==false&&c.at("full_simulation_admitted")==false,"retain source transfer limitations");
  const J&a=c.at("raw_argument_record");p::need(tiny_sha::sha256(a.dump())==c.at("raw_argument_record_sha256").get<std::string>()&&a.at("native_launch_binding").at("process")==c.at("process")&&a.at("native_launch_binding").at("native_launch_id")==c.at("native_launch_id")&&a.at("code_sha256")==c.at("code_sha256")&&a.at("parameter_layout_sha256")==c.at("parameter_layout_sha256")&&a.at("phase")==c.at("phase")&&a.at("context_id")==c.at("context_id")&&a.at("stream_u64")==c.at("stream_u64"),"actual argument launch binding");
  std::vector<std::vector<U>>rs;for(const auto&r:a.at("arguments"))rs.push_back(raw(r));U in=0,out=0;
  if(c.at("family")=="head_cast"){
   p::need(m.call.at("family")=="CastLogits"&&rs.size()==7&&c.at("parameter_layout_sha256")==m.call.at("parameter_layout_sha256"),"cast ABI/layout");const U slots[]={0,4,8,24,25,28,36},sizes[]={4,1,16,1,1,8,8};std::map<U,U>active;
   for(U i=0;i<7;++i){p::need(rs[i].size()==sizes[i],"cast parameter size");for(U j=0;j<rs[i].size();++j)active[slots[i]+j]=rs[i][j];}
   for(const auto&v:m.call.at("structural_fields").at("observed_nonpointer_bytes").items())p::need(active.at(std::stoull(v.key()))==p::natural(v.value()),"original cast used ABI byte");out=word(rs[2],0);in=word(rs[2],8);
  }else{
   p::need(c.at("family")=="single_cta_argmax"&&rs.size()==1&&rs[0].size()==1064&&c.at("parameter_layout_sha256")=="768ee2b68e24e9808dd7384e77b766028d2827250592e3362da43352aed86a9a","argmax exact ABI");
   for(const auto&v:m.call.at("used_nonpointer_bytes").items())p::need(rs[0].at(std::stoull(v.key()))==p::natural(v.value()),"original argmax used control byte");in=word(rs[0],1000);out=word(rs[0],1008);for(U off:{1016,1024,1032,1040})p::need(word(rs[0],off)==0,"no global split/partial/accumulate helper state");
  }
  p::need(c.at("objects").size()==2,"exact two helper roles");
  for(auto role:{"input","output"}){const auto&o=c.at("objects").at(role);const auto&v=o.at("view");const auto&r=o.at("root");U base=p::natural(o.at("pointer")),size=p::natural(o.at("bytes")),old=p::natural(m.call.at("roles").at(role).at("argument_pointer"));U expected=std::string(role)=="input"?(c.at("family")=="head_cast"?256512:513024):(c.at("family")=="head_cast"?513024:8);
   p::need(base==(std::string(role)=="input"?in:out)&&size==expected&&o.at("source_pointer")==old&&base%128==old%128,"raw current helper pointer/whole extent/alignment");
   p::need(v.at("data_address")==base&&v.at("logical_nbytes")==size&&v.at("root")==r.at("id")&&p::add(p::natural(r.at("base_address")),p::natural(v.at("storage_offset_bytes")))==base&&p::add(base,size)==p::natural(o.at("end_exclusive"))&&p::add(base,size)<=p::add(p::natural(r.at("base_address")),p::natural(r.at("storage_nbytes"))),"current typed helper bounds");
  }
  const auto&i=c.at("objects").at("input");const auto&o=c.at("objects").at("output");p::need(p::natural(i.at("end_exclusive"))<=out||p::natural(o.at("end_exclusive"))<=in,"helper role alias graph");
 }
 Binding(const helper_bridge::Model&m,const J&c,const coupling::ServiceMapper&map):m_(m),current_(c),mapper_(map){admit(c,m);
  for(const auto&cl:m.classes){std::vector<tiny_full::SourceNode>ns;for(const auto&n:cl->nodes){tiny_full::SourceNode s;s.id=ns.size();s.warp=n.warp;s.ordinal=n.ordinal;s.pc=n.pc;s.source_ordinal=n.old;s.mask=n.effective;s.compute_elements=n.elements;s.pipeline=n.pipe;s.write=n.memop=="W";
    s.kind=n.kind=="global"?tiny_full::Kind::Global:n.kind=="shared"?tiny_full::Kind::Shared:n.kind=="barrier"?tiny_full::Kind::Barrier:n.kind=="control"?tiny_full::Kind::Control:tiny_full::Kind::Compute;
    s.completion_dependencies.assign(n.deps.begin(),n.deps.end());s.issue_dependencies.assign(n.issues.begin(),n.issues.end());ns.push_back(std::move(s));}classes_.push_back(std::move(ns));}
 }
 U ctas()const override{return m_.ctas;}unsigned warps(U c)const override{p::need(c<ctas(),"helper CTA");return m_.warps;}unsigned resident_limit()const override{return m_.resident;}U first_node(U c)const override{return m_.spans.at(c).first_node;}
 std::span<const tiny_full::SourceNode>nodes(U c)const override{return classes_.at(m_.class_for_cta.at(c));}U template_class(U c)const override{return m_.class_for_cta.at(c);}
 tiny_full::MemoryDescriptor memory(U c,unsigned member)const override{
  const auto&n=m_.at(c).nodes.at(member);p::need(n.kind=="global"||n.kind=="shared","helper memory member");tiny_full::MemoryDescriptor d;d.write=n.memop=="W";d.path=n.kind=="global"?tiny_full::Path::DirectGlobal:tiny_full::Path::Shared;g::ExplicitMemorySubop sub;std::set<U>lines;
  for(const auto&r:n.kind=="global"?n.global:n.shared){auto x=n.kind=="global"?m_.range(c,n,r):r;
   if(n.kind=="global"){const J*target=nullptr;for(const auto&o:current_.at("objects").items()){U base=p::natural(o.value().at("source_pointer"));if(x.address>=base&&p::add(x.address,x.width)<=p::add(base,p::natural(o.value().at("bytes")))){p::need(!target,"unique old helper role");target=&o.value();}}
    p::need(target,"source helper complete range role");x.address=p::add(p::natural(target->at("pointer")),x.address-p::natural(target->at("source_pointer")));p::need(p::add(x.address,x.width)<=p::natural(target->at("end_exclusive")),"current helper whole width");
    for(U line=x.address/128*128,last=(x.address+x.width-1)/128*128;;line+=128){(void)mapper_.map({1,line});lines.insert(line);if(line==last)break;}
   }
   sub.ranges.push_back({x.lane,x.address,x.width});sub.source_member_ordinals.push_back(x.lane);sub.requested_bytes+=x.width;
  }
  if(n.kind=="global"){d.global_bytes=sub.requested_bytes;d.global_subops.push_back(std::move(sub));for(U line:lines)d.lines.push_back({1,line});}
  else{d.shared_bytes=sub.requested_bytes;d.shared_service=g::describe_explicit_sram_ranges(sub);d.shared_subops.push_back(std::move(sub));}return d;
 }
 J evidence()const override{return {{"source_model_key",m_.key},{"source_call",m_.call},{"all_original_compute_control_shared_barrier_nodes_preserved",true},{"all_original_issue_and_completion_edges_preserved",true},{"global_address_change","current typed-role rebase only"},{"secondary_predicate_values_known",false},{"implicit_register_dependencies_complete",false},{"current_occupancy_observed",false},{"compute_transfer_qualified",false},{"full_simulation_admitted",false}};}
};
} // namespace current_helpers
