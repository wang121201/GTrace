#pragma once
// Original HelperBuilder/Fine DAG; only current GLOBAL role addresses change.
namespace current_controls {
using namespace native_sequence;
inline J read_pin(const J&q){std::ifstream f(q.at("path").get<std::string>(),std::ios::binary);p::need(bool(f),"controls pinned source");std::string s((std::istreambuf_iterator<char>(f)),{});p::need(s.size()==p::natural(q.at("bytes"))&&tiny_sha::sha256(s)==q.at("sha256").get<std::string>(),"controls pinned bytes");return J::parse(s);}
inline std::vector<U> raw(const J&a){std::string h=a.at("raw_bytes_hex"),bytes;p::need(h.size()==2*p::natural(a.at("size_bytes")),"actual raw parameter extent");auto nib=[](char c)->U{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("canonical raw hex");};std::vector<U>v;for(U i=0;i<h.size();i+=2){U x=(nib(h[i])<<4)|nib(h[i+1]);v.push_back(x);bytes+=char(x);}p::need(tiny_sha::sha256(bytes)==a.at("sha256").get<std::string>(),"raw parameter SHA");return v;}
inline U word(const std::vector<U>&r,U o){p::need(o+8<=r.size(),"actual pointer slot");U v=0;for(U j=0;j<8;++j)v|=r[o+j]<<(8*j);return v;}
struct Plan {
 J envelope,calls,representatives;
 std::map<std::string,std::unique_ptr<helper_bridge::Model>>models;
 explicit Plan(const J&in):envelope(in),calls(read_pin(in.at("calls"))),representatives(read_pin(in.at("expected"))){
  p::need(in.at("schema")=="CURRENT76_CONTROLS_HELPER_EXECUTION_PLAN_V1"&&in.at("call_count")==76&&calls.size()==76&&representatives.size()==38,"exact regular76 helper/control scope");
  J source=read_pin(in.at("source_models"));for(const auto&t:source.at("models").items())models.emplace(t.key(),std::make_unique<helper_bridge::Model>(t.value(),source.at("expected").at(t.key())));p::need(models.size()==38,"all original38 full templates");for(const auto&c:calls.items())admit(c.value(),std::stoull(c.key()));
 }
 const J&call(U id)const{return calls.at(std::to_string(id));}
 const helper_bridge::Model&model(U id)const{return *models.at(call(id).at("source_model_key").get<std::string>());}
 void admit(const J&c,U id)const{
  const auto&m=*models.at(c.at("source_model_key").get<std::string>());
  p::need(c.at("schema")=="CURRENT_CONTROLS_HELPER_CALL_V1"&&c.at("native_launch_id")==id&&c.at("process")==J({{"pid",3011726},{"start_ticks",897090720}}),"current source/control identity");
  for(auto k:{"code_sha256","grid","block"})p::need(c.at(k)==m.call.at(k),"original code and grid exact");
  for(auto k:{"registers","dynamic_shared_bytes","static_shared_bytes","local_bytes_per_thread","binary_version","ptx_version","attribute_query_results"})p::need(c.at("observed_static_resources").at(k)==m.call.at("native_resources").at(k),"queried static resources exact");
  p::need(c.at("source_occupancy_assumption")==m.resident&&c.at("current_occupancy_observed")==false&&c.at("implicit_register_dependencies_complete")==false&&c.at("compute_transfer_qualified")==false&&c.at("full_simulation_admitted")==false,"reused occupancy and limited compute qualification");
  const auto&a=c.at("raw_argument_record");p::need(tiny_sha::sha256(a.dump())==c.at("raw_argument_record_sha256").get<std::string>()&&a.at("native_launch_binding").at("process")==c.at("process")&&a.at("native_launch_binding").at("native_launch_id")==id&&a.at("code_sha256")==c.at("code_sha256")&&a.at("phase")==c.at("phase"),"actual raw argument identity");
  std::vector<std::vector<U>>rs;for(const auto&r:a.at("arguments")){p::need(r.at("index")==rs.size(),"ordered ABI arguments");rs.push_back(raw(r));}
  // Checks are source-derived at preparation and pinned with the complete calls file.
  for(const auto&v:c.at("used_byte_checks"))p::need(rs.at(p::natural(v.at("argument"))).at(p::natural(v.at("offset")))==p::natural(v.at("value")),"all active original control bytes");
  const J&old=m.call.contains("objects")?m.call.at("objects"):m.call.at("roles");p::need(c.at("objects").size()==old.size(),"all original operand roles retained");
  for(const auto&v:old.items()){
   const auto&o=c.at("objects").at(v.key());const auto&src=v.value();U op=p::natural(src.at(src.contains("pointer")?"pointer":"argument_pointer")),cp=p::natural(o.at("pointer"));J windows=src.contains("byte_intervals_relative_to_argument")?src.at("byte_intervals_relative_to_argument"):J::array({J::array({0,src.at("bytes")})});
   p::need(o.at("source_pointer")==op&&o.at("source_relative_windows")==windows&&cp&&cp%128==op%128,"exact source role windows/alignment");const auto&slot=c.at("pointer_slots").at(v.key());p::need(o.at("argument_slot")==slot&&word(rs.at(p::natural(slot.at(0))),p::natural(slot.at(1)))==cp,"raw current role pointer");
   U hi=0;for(const auto&w:windows){U lo=p::natural(w[0]),end=p::natural(w[1]);p::need(lo<end,"nonempty complete role window");hi=std::max(hi,end);}p::need(o.at("addressed_extent_bytes")==hi,"source request extent exact");const auto&alloc=o.at("allocation_cover");U base=p::natural(alloc.at("base_u64")),size=p::natural(alloc.at("bytes"));p::need(alloc.at("edge")=="return"&&alloc.at("action")=="allocate"&&alloc.at("cuda_status")==0&&base<=cp&&p::add(cp,hi)<=p::add(base,size),"actual allocation contains entire current source request window");
  }
  if(!m.recipe.is_null()){
   if(m.recipe.at("family")=="EmbeddingPrefill"||m.recipe.at("family")=="EmbeddingDecode")p::need(c.at("public_binding").at("input_ids")==m.recipe.at("input_ids"),"current embedding indices select same source offsets");
   if(m.recipe.at("family")=="RequestTokenIndex")p::need(c.at("public_binding").at("actual_indices")==J::array({0,m.recipe.at("positions").at(0)}),"current request index/address branch");
  }
 }
};
// Plan/model and ServiceMapper must outlive Binding and all issued work.
struct Binding:helper_bridge::Builder {
 const Plan&plan;const J&current;const coupling::ServiceMapper&mapper;tiny_sha::Sha256 current_addresses;U rebased_ranges=0,rebased_bytes=0;
 Binding(const Plan&p,U id,const coupling::ServiceMapper&map):helper_bridge::Builder(p.model(id),p.model(id).ctas),plan(p),current(p.call(id)),mapper(map){plan.admit(current,id);}
 U rebase(U address,U width)const{
  bool found=false;U result=0;
  for(const auto&o:current.at("objects").items()){U old=p::natural(o.value().at("source_pointer")),base=p::natural(o.value().at("pointer"));if(address<old)continue;U offset=address-old;for(const auto&w:o.value().at("source_relative_windows"))if(p::natural(w[0])<=offset&&p::add(offset,width)<=p::natural(w[1])){U next=p::add(base,offset);p::need(!found||result==next,"source alias roles must map to one identical current VA");found=true;result=next;}}
  p::need(found,"complete original request covered by current role window");return result;
 }
 g::CtaGraphStore::Owned build(int c){auto out=helper_bridge::Builder::build(c);const auto&model=m.at(c);p::need(out.size()==model.nodes.size(),"full original HelperBuilder graph retained");
  for(U i=0;i<out.size();++i){const auto&t=model.nodes[i];if(t.kind!="global")continue;auto&n=*out[i];p::need(n.explicit_memory_subops.size()==1,"original one global operand");auto&sub=n.explicit_memory_subops[0];p::need(sub.ranges.size()==t.global.size(),"original lane multiplicity");
   for(U k=0;k<t.global.size();++k){auto old=m.range(c,t,t.global[k]);auto&r=sub.ranges[k];p::need(r.offset_bytes==old.address&&r.byte_count==old.width&&r.source_member_ordinal==old.lane,"exact original generated range");r.offset_bytes=rebase(old.address,old.width);for(U line=r.offset_bytes/128*128,last=(r.offset_bytes+old.width-1)/128*128;;line+=128){(void)mapper.map({1,line});if(line==last)break;}for(U v:{U(c),i,U(old.lane),r.offset_bytes,old.width}){std::array<char,8>b{};for(int z=0;z<8;++z)b[z]=char(v>>(8*z));current_addresses.add(b.data(),8);}++rebased_ranges;rebased_bytes+=old.width;}
  }return out;
 }
 J evidence(){return {{"native_launch_id",current.at("native_launch_id")},{"source_graph",receipt()},{"retired_CTAs",retired_ctas},{"old_source_address_sha256",addresses.hex()},{"current_address_sha256",current_addresses.hex()},{"rebased_ranges",rebased_ranges},{"rebased_bytes",rebased_bytes},{"source_shared_compute_barrier_or_dependencies_changed",false},{"source_occupancy_assumption",m.resident},{"current_occupancy_observed",false},{"compute_transfer_qualified",false},{"full_simulation_admitted",false}};}
};
}
