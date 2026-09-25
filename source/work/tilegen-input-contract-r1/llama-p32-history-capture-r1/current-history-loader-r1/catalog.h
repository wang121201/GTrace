#pragma once
// Include after the immutable canonical streaming TU. No scheduler/cache state here.
#include "../current-gemv-bridge-r1/bridge.h"
#include "../current-elementwise-bridge-r1/bridge.h"
#include "../current-gemm-tiny-execution-r1/bridge-r2.h"
#include "../current-rotary-execution-r1/bridge.h"
#include "../current-attention-execution-r1/bridge.h"
#include "../current-index-copy-execution-r1/bridge.h"
#include "../current-helpers-execution-r1/bridge.h"
#include "../current-controls-execution-r1/bridge.h"
namespace current_history_loader {
using J=nlohmann::json;using U=std::uint64_t;namespace g=GTSim;
inline U checks=0;
inline void need(bool v,const std::string&m){++checks;if(!v)throw std::runtime_error("history loader: "+m);}
inline U num(const J&j){need(j.is_number_integer()&&!j.is_boolean()&&(j.is_number_unsigned()||j.get<std::int64_t>()>=0),"unsigned integer");return j.get<U>();}
inline std::string read_bytes(const J&pin){std::ifstream f(pin.at("path").get<std::string>(),std::ios::binary);need(bool(f),"pinned file open");std::string s((std::istreambuf_iterator<char>(f)),{});need(s.size()==num(pin.at("bytes"))&&tiny_sha::sha256(s)==pin.at("sha256").get<std::string>(),"exact bytes/SHA "+pin.at("path").get<std::string>());return s;}
inline J read_pin(const J&pin){return J::parse(read_bytes(pin));}
template<class Model>const J&source_call(const Model&m,const J&c){const J*out=nullptr;for(const J*v:m.calls)if(v->at("source_launch_key")==c.at("legacy_source_key")){need(!out,"unique old call");out=v;}need(out,"original source call");return *out;}
// Each state/owner is lazy. No decoded frame DOM is retained after the original
// PrefillTemplateCache has compiled its immutable source class.
struct State {
 J manifest;const g::L2DramAddressMapper&base;const coupling::ServiceMapper&map;
 std::map<std::string,J> jsons;std::map<std::string,J>checked_pins;
 std::unique_ptr<current_gemv::Plan> gemv;
 std::unique_ptr<canonical_norm::Model> norm;std::unique_ptr<canonical_fused::Model> fused;std::unique_ptr<canonical_silu::Model>silu;
 std::unique_ptr<canonical_rotary::Model>rotary;
 std::map<std::string,std::unique_ptr<coupling::ServiceMapper>>oldmaps;
 std::unique_ptr<current_attention::Plan>attention;std::unique_ptr<current_index_copy::Catalog>index;
 std::unique_ptr<current_controls::Plan>controls;
 std::map<std::string,std::unique_ptr<helper_bridge::Model>>tail;
 std::unique_ptr<compressed_frame::Cache>frames;tiny_full::PrefillTemplateCache templates;
 State(const J&pin,const g::L2DramAddressMapper&b,const coupling::ServiceMapper&m):manifest(read_pin(pin)),base(b),map(m){
  need(manifest.at("schema")=="CURRENT_HISTORY_REGULAR2276_DISPATCH_MANIFEST_V1"&&manifest.at("status")=="PASS_SAVED_UNIQUE_OWNERSHIP_AND_FULL_WORK_CENSUS","sealed registry schema/status");
  need(manifest.at("process")==J({{"pid",3011726},{"start_ticks",897090720}})&&manifest.at("entries").size()==2276,"complete fixed process regular domain");
  const std::map<std::string,U>counts={{"gemv",518},{"gemm",256},{"elementwise",582},{"rotary",192},{"attention",192},{"index_copy",448},{"tail_helpers",12},{"controls",76}};std::map<std::string,U>actual;
  for(const auto&i:manifest.at("entries").items()){const auto&e=i.value();need(num(e.at("native_launch_id"))==std::stoull(i.key())&&e.at("process")==manifest.at("process"),"key/process identity");std::string owner=e.at("owner");need(counts.count(owner)&&e.at("execution_route")==manifest.at("bridges").at(owner).at("route"),"closed route");++actual[owner];U c=1;for(const auto&x:e.at("grid")){need(num(x)>0&&c<=UINT64_MAX/num(x),"grid product");c*=num(x);}need(e.at("expected").at("CTAs")==c,"full current grid");for(const auto&x:e.at("qualification").items())need(x.value()==false,"no qualification upgrade");}
  need(actual==counts,"exact exclusive owner census");checked_pins.emplace(pin.at("path").get<std::string>(),pin);
 }
 const J&json(const J&p){std::string path=p.at("path");auto it=jsons.find(path);if(it!=jsons.end()){need(checked_pins.at(path)==p,"consistent pin per path");return it->second;}auto j=read_pin(p);checked_pins[path]=p;return jsons.emplace(path,std::move(j)).first->second;}
 const J&entry(U id)const{return manifest.at("entries").at(std::to_string(id));}
 const J&plan(const std::string&o){return json(manifest.at("bridges").at(o).at("plan"));}
 const J&call(U id){const auto&r=entry(id).at("call_ref");return json(r.at("file")).at(J::json_pointer(r.at("json_pointer").get<std::string>()));}
 coupling::ServiceMapper&oldmap(const std::string&key,const J&m){auto&v=oldmaps[key];if(!v)v=std::make_unique<coupling::ServiceMapper>(m,base,20ULL<<30);return *v;}
 void load_frames(){if(frames)return;const auto&p=plan("gemm").at("input"); // Streaming frame reader checks each encoded/decompressed pin.
  (void)read_bytes(p);checked_pins[p.at("path").get<std::string>()]=p;std::ifstream f(p.at("path").get<std::string>(),std::ios::binary);J c=compressed_frame::read_control(f);frames=std::make_unique<compressed_frame::Cache>(c.at("frames"));for(U i=0;i<c.at("frames").size();++i)frames->read_one(f);frames->finish(f);
 }
 std::unique_ptr<tiny_full::KernelBinding>tiny(U id){const auto&e=entry(id);need(e.at("execution_route")=="Tiny","requested Tiny route");std::string owner=e.at("owner");const auto&c=call(id);const auto&p=plan(owner);
  if(owner=="gemv"){if(!gemv)gemv=std::make_unique<current_gemv::Plan>(p);return std::make_unique<current_gemv::CurrentGemvBinding>(*gemv,id,map);}
  if(owner=="gemm"){
   current_gemm::validate_call(c,p.at("frames"));const auto&op=json(c.at("source_plan"));const J*old=nullptr;for(const auto&o:op.at("calls"))if(o.at("layer")==c.at("binding").at("layer")&&o.at("module_scope")==c.at("binding").at("module_scope")){need(!old,"unique old GEMM phase/layer/module");old=&o;}need(old,"old GEMM call");current_gemm_tiny::match_old(c,*old);const std::string key=c.at("frame_key");load_frames();const auto&fd=p.at("frames").at(key).at("frame_declaration");J identity={{"key",key},{"bytes",fd.at("decoded_bytes")},{"sha256",fd.at("decoded_sha256")}};
   auto&om=oldmap("gemm:"+old->at("source_launch_key").get<std::string>(),current_gemm_tiny::role_map(old->at("objects")));
   auto graph=templates.bind(key,identity,[&](){return frames->with_decoded(key,[](const std::string&s){return J::parse(s);});},*old,num(e.at("expected").at("CTAs")),om);
   return std::make_unique<current_gemm_tiny::Binding>(std::move(graph),*old,c,p.at("frames"),map);
  }
  if(owner=="elementwise"){
   const std::string family=c.at("family");const auto&env=p.at("legacy_envelopes").at(family);const J*old=nullptr;std::unique_ptr<tiny_full::KernelBinding>graph;auto&om=oldmap(family,env.at("service_address_map"));U count=num(e.at("expected").at("CTAs"));
   if(family=="rmsnorm"){if(!norm)norm=std::make_unique<canonical_norm::Model>(env);old=&source_call(*norm,c);graph=std::make_unique<tiny_full::PlainNormBinding>(*norm,*old,count,om);}
   else if(family=="fused_norm"){if(!fused)fused=std::make_unique<canonical_fused::Model>(env);old=&source_call(*fused,c);graph=std::make_unique<tiny_full::FusedNormBinding>(*fused,*old,count,om);}
   else{need(family=="silu","known elementwise family");if(!silu)silu=std::make_unique<canonical_silu::Model>(env);old=&source_call(*silu,c);graph=std::make_unique<tiny_full::SiluBinding>(*silu,*old,count,om);}return std::make_unique<current_elementwise::Binding>(std::move(graph),*old,c,map);
  }
  if(owner=="rotary"){if(!rotary)rotary=std::make_unique<canonical_rotary::Model>(p.at("legacy_envelope"));return std::make_unique<current_rotary::Binding>(*rotary,source_call(*rotary,c),c,oldmap("rotary",p.at("legacy_envelope").at("service_address_map")),map);}
  need(owner=="tail_helpers","closed Tiny owner");std::string key=c.at("source_model_key");auto&model=tail[key];if(!model){J source=read_pin(p.at("source_models"));model=std::make_unique<helper_bridge::Model>(source.at("models").at(key),source.at("expected").at(key));checked_pins[p.at("source_models").at("path").get<std::string>()]=p.at("source_models");}return std::make_unique<current_helpers::Binding>(*model,c,map);
 }
};
// This forwarding owner keeps all lazy models/old mappers alive even when the
// Catalog itself is released. The externally supplied base/current mapper must
// still outlive all returned handles, graph stores, and pending completions.
class TinyHandle final:public tiny_full::KernelBinding{
 std::shared_ptr<State>owner_;std::unique_ptr<tiny_full::KernelBinding>binding_;
public:TinyHandle(std::shared_ptr<State>s,U id):owner_(std::move(s)),binding_(owner_->tiny(id)){need(binding_->ctas()==num(owner_->entry(id).at("expected").at("CTAs")),"actual Tiny full grid");}
 U ctas()const override{return binding_->ctas();}unsigned warps(U c)const override{return binding_->warps(c);}unsigned resident_limit()const override{return binding_->resident_limit();}U first_node(U c)const override{return binding_->first_node(c);}std::span<const tiny_full::SourceNode>nodes(U c)const override{return binding_->nodes(c);}U template_class(U c)const override{return binding_->template_class(c);}tiny_full::MemoryDescriptor memory(U c,unsigned n)const override{return binding_->memory(c,n);}J evidence()const override{return binding_->evidence();}
};
class FineHandle:public std::enable_shared_from_this<FineHandle>{
 std::shared_ptr<State>owner_;U id_;std::unique_ptr<current_attention::Binding>a_;std::unique_ptr<current_index_copy::PreparedMemory>i_;std::unique_ptr<current_controls::Binding>c_;
public:
 FineHandle(std::shared_ptr<State>s,U id):owner_(std::move(s)),id_(id){const auto&e=owner_->entry(id);need(e.at("execution_route")=="Fine","requested Fine route");const std::string kind=e.at("owner");const auto&p=owner_->plan(kind);
  if(kind=="attention"){if(!owner_->attention)owner_->attention=std::make_unique<current_attention::Plan>(p);a_=std::make_unique<current_attention::Binding>(*owner_->attention,id,owner_->map);}
  else if(kind=="index_copy"){if(!owner_->index)owner_->index=std::make_unique<current_index_copy::Catalog>(p,owner_->base);i_=std::make_unique<current_index_copy::PreparedMemory>(*owner_->index,owner_->call(id),owner_->map);}
  else{need(kind=="controls","closed Fine owner");if(!owner_->controls)owner_->controls=std::make_unique<current_controls::Plan>(p);c_=std::make_unique<current_controls::Binding>(*owner_->controls,id,owner_->map);}
 }
 const J&entry()const{return owner_->entry(id_);}unsigned resident()const{return num(entry().at("source_resident_limit"));}
 template<class F>void visitFine(F&&f){if(a_)f(*a_);else if(i_)i_->visit(f);else f(*c_);}
 g::CtaGraphStore::Owned build_cta(int c){need(c>=0&&U(c)<num(entry().at("expected").at("CTAs")),"full Fine CTA domain");return a_?a_->build(c):i_?i_->build_cta(c):c_->build(c);}
 void retire(int c,const std::vector<g::DAGNode*>&ns,g::Cycle cycle){if(a_)a_->retire(c,ns,cycle);else if(i_)i_->retire(c,ns,cycle);else c_->retire(c,ns,cycle);}
 J source_work()const{if(a_)return {{"CTAs",a_->retired_ctas},{"nodes",a_->retired},{"requested_read_bytes",a_->read},{"requested_write_bytes",a_->write}};if(c_)return {{"CTAs",c_->retired_ctas},{"nodes",c_->retired_nodes},{"requested_read_bytes",c_->read},{"requested_write_bytes",c_->write}};J out;i_->visit([&](const auto&b){out={{"CTAs",b.retired_ctas},{"nodes",b.retired_nodes},{"requested_read_bytes",b.read},{"requested_write_bytes",b.write}};});return out;}
 std::unique_ptr<g::CtaGraphStore>make_store(){g::CtaGraphStore::Spec spec{};spec.cta_count=num(entry().at("expected").at("CTAs"));spec.sm_count=48;spec.total_nodes=num(entry().at("expected").at("nodes"));spec.resident_cta_limit_per_sm=resident();spec.per_sm_warp_placement=true;spec.allow_declared_tensor_work=true;spec.allow_observed_async_shared_service=true;spec.allow_abstract_async_copy=false;g::CtaGraphStore::Limits limits;
  if(a_){const auto&m=a_->m;spec.warps_per_cta=4;spec.spans=m.spans;limits.max_nodes_per_cta=m.nodes.size();limits.max_explicit_ranges_per_cta=m.ranges;limits.max_live_nodes=m.live_nodes;limits.max_live_explicit_ranges=m.live_ranges;}
  else if(c_){const auto&m=c_->m;auto x=m.census(m.ctas);spec.warps_per_cta=m.warps;spec.spans=m.spans;limits.max_nodes_per_cta=x.max_nodes;limits.max_explicit_ranges_per_cta=x.max_ranges;limits.max_live_nodes=x.max_nodes*std::min(U(m.ctas),U(48*m.resident));limits.max_live_explicit_ranges=x.max_ranges*std::min(U(m.ctas),U(48*m.resident));}
  else i_->visit([&](auto&b){spec.warps_per_cta=b.program.warps;spec.spans=b.spans;limits.max_nodes_per_cta=b.registers.nodes.size();U ranges=0;for(const auto&n:b.registers.nodes)if(n.kind=="global")ranges+=32;limits.max_explicit_ranges_per_cta=ranges;U live=std::min(U(spec.cta_count),U(48*resident()));limits.max_live_nodes=limits.max_nodes_per_cta*live;limits.max_live_explicit_ranges=ranges*live;});
  U total=0;for(const auto&s:spec.spans){need(s.first_node==int(total)&&s.node_count>0,"original contiguous spans");total+=s.node_count;}need(total==spec.total_nodes&&spec.spans.size()==U(spec.cta_count),"original full Fine work census");auto self=shared_from_this();return std::make_unique<g::CtaGraphStore>(spec,[self](int c){return self->build_cta(c);},[self](int c,const auto&ns,g::Cycle at){self->retire(c,ns,at);},limits);
 }
};
class Catalog {
 std::shared_ptr<State>state_;
public:Catalog(const J&dispatch_pin,const g::L2DramAddressMapper&base,const coupling::ServiceMapper&current):state_(std::make_shared<State>(dispatch_pin,base,current)){}
 const J&entry(U id)const{return state_->entry(id);}const J&manifest()const{return state_->manifest;}
 const J&checked_call(U id){return state_->call(id);}
 std::unique_ptr<tiny_full::KernelBinding>bindTiny(U id){return std::make_unique<TinyHandle>(state_,id);}
 std::shared_ptr<FineHandle>bindFine(U id){return std::make_shared<FineHandle>(state_,id);}
 J runtime_expected(U id)const{const auto&e=entry(id).at("expected");return {{"CTAs",e.at("CTAs")},{"nodes",e.at("nodes")},{"requested_read_bytes",e.at("logical_read_bytes")},{"requested_write_bytes",e.at("logical_write_bytes")}};}
 J loading_receipt()const{J pins=J::array();for(const auto&p:state_->checked_pins)pins.push_back(p.second);return {{"opened_pins",pins},{"decoded_frame_JSON_retained",false},{"template_cache",state_->templates.receipt()},{"actual_execution",false}};}
};
}
