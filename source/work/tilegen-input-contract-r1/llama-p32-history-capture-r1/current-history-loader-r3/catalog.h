#pragma once
// r1 routes are the default. Conditional initialization work requires an explicit option.
#include "../current-history-loader-r1/catalog.h"
#include "../current-controls-tiny-execution-r1/bridge.h"
#include "../current-outside-execution-r1/bridge.h"
#include "../current-initial-rope-execution-r1/bridge_strict.h"
#include "../current-initial-cutlass-execution-r1/bridge.h"
#include "../current-initial-rope-execution-r1/bridge_summary.h"
#include "../current-initial-rope-execution-r1/bridge_cat.h"
namespace current_history_loader_v3 {
using J=nlohmann::json;using U=std::uint64_t;namespace g=GTSim;
using current_history_loader::need;using current_history_loader::num;using current_history_loader::read_pin;
enum class InitializationWork { Unselected, Min, Max };
struct Options { bool controls_tiny=false;InitializationWork initialization_work=InitializationWork::Unselected; };
inline std::string mode(InitializationWork x){switch(x){case InitializationWork::Unselected:return "unselected";case InitializationWork::Min:return "min";case InitializationWork::Max:return "max";}throw std::runtime_error("invalid initialization work option");}
struct State {
 J manifest,effective;const g::L2DramAddressMapper&base;const coupling::ServiceMapper&map;Options options;
 std::unique_ptr<current_history_loader::Catalog>regular;
 std::map<std::string,J>jsons,read_pins;std::unique_ptr<current_controls::Plan>controls;
 State(const J&pin,const g::L2DramAddressMapper&b,const coupling::ServiceMapper&m,Options opts):manifest(read_pin(pin)),effective(manifest.at("entries")),base(b),map(m),options(opts){
  need(manifest.at("schema")=="CURRENT_HISTORY_ALL2426_DEFAULT_DISPATCH_MANIFEST_V3"&&manifest.at("status")=="PASS_SAVED2421_BRIDGES_PLUS_FIVE_EXPLICIT_CONDITIONAL_SUMMARIES","r3 closed saved source admission");
  need(manifest.at("total_current_kernels")==2426&&effective.size()==2426&&manifest.at("default_controls_route")=="Fine"&&manifest.at("summary_selection_default").is_null(),"default routes and explicit summary choice");
  regular=std::make_unique<current_history_loader::Catalog>(manifest.at("parent_regular_dispatch"),base,map);U normal=0,conditional=0,regular_count=0;std::map<std::string,U>outside;
  for(auto&item:effective.items()){
   auto&e=item.value();U id=std::stoull(item.key());need(e.at("native_launch_id")==id&&e.at("process")==manifest.at("process"),"current ID/process");
   if(e.at("scope_group")=="regular"){
    ++regular_count;const auto&old=regular->entry(id);for(const auto&v:old.items())need(e.at(v.key())==v.value(),"all original regular identity/work/default-route fields exact");
    if(options.controls_tiny&&e.at("owner")=="controls"){e["execution_route"]="Tiny";e["explicit_nondefault_controls_tiny"]=true;}
   }else{need(e.at("scope_group")=="outside_regular","closed scope");++outside[e.at("owner").get<std::string>()];}
   if(e.at("registry_state")=="BRIDGE_READY_CONTINUOUS_HISTORY_NOT_EXECUTED")++normal;
   else{
    need(e.at("registry_state")=="CONDITIONAL_INITIALIZATION_SUMMARY_REQUIRES_WORK_VARIANT"&&e.at("source_DAG_exact")==false&&e.at("hardware_cycle_bounds")==false,"explicit initialization-only approximation");++conditional;
    if(options.initialization_work!=InitializationWork::Unselected){const auto&v=e.at("variants").at(mode(options.initialization_work));e["expected"]=v.at("expected");e["call_ref"]=v.at("call_ref");e["selected_work_envelope"]=mode(options.initialization_work);}
   }
  }
  need(regular_count==2276&&normal==2421&&conditional==5&&outside==std::map<std::string,U>{{"outside_fill_arange",141},{"initial_rope_strict",3},{"initial_cutlass",1},{"initial_rope_summary",4},{"initial_cat_summary",1}},"exact current2426 owners and semantic scopes");read_pins.emplace(pin.at("path").get<std::string>(),pin);
 }
 const J&entry(U id)const{return effective.at(std::to_string(id));}
 const J&admitted(U id)const{const auto&e=entry(id);need(e.at("registry_state")=="BRIDGE_READY_CONTINUOUS_HISTORY_NOT_EXECUTED"||e.contains("selected_work_envelope"),"conditional summary work must be explicitly selected, never silently skipped");return e;}
 const J&json(const J&p){std::string path=p.at("path");auto i=jsons.find(path);if(i!=jsons.end()){need(read_pins.at(path)==p,"same pin per file");return i->second;}auto j=read_pin(p);read_pins[path]=p;return jsons.emplace(path,std::move(j)).first->second;}
 const J&call(U id){const auto&e=admitted(id);if(e.at("scope_group")=="regular")return regular->checked_call(id);const auto&r=e.at("call_ref");return json(r.at("file")).at(J::json_pointer(r.at("json_pointer").get<std::string>()));}
 std::unique_ptr<tiny_full::KernelBinding>tiny(U id){const auto&e=admitted(id);need(e.at("execution_route")=="Tiny","explicit Tiny route");const std::string owner=e.at("owner");
  if(owner=="controls"){
   need(options.controls_tiny,"Controls Fine is default");const auto&opt=manifest.at("optional_controls_tiny");const auto&iface=json(opt.at("interface"));if(!controls)controls=std::make_unique<current_controls::Plan>(json(opt.at("parent_plan")));const auto&accepted=iface.at("calls").at(std::to_string(id));const auto&w=accepted.at("model_counts");need(accepted.at("native_launch_id")==id&&accepted.at("execution_route")=="Tiny"&&w.at("CTAs")==e.at("expected").at("CTAs")&&w.at("nodes")==e.at("expected").at("nodes")&&w.at("requested_read_bytes")==e.at("expected").at("logical_read_bytes")&&w.at("requested_write_bytes")==e.at("expected").at("logical_write_bytes"),"closed candidate full work");return std::make_unique<current_controls::TinyBinding>(*controls,id,map);
  }
  if(e.at("scope_group")=="regular")return regular->bindTiny(id);
  const auto&bridge=manifest.at("bridges").at(owner);const auto&templates=json(bridge.at("templates"));const auto&c=call(id);const auto&binding=c.at("binding");
  for(auto key:{"native_launch_id","source_launch_key","phase","code_sha256","grid","block"})need(binding.at(key)==e.at(key),"actual outside identity");
  if(owner=="outside_fill_arange")return std::make_unique<current_outside::Binding>(templates,c,map);
  if(owner=="initial_rope_strict")return std::make_unique<current_initial_rope::Binding>(templates,c,map);
  if(owner=="initial_cutlass")return std::make_unique<current_initial_cutlass::Binding>(templates,c,map);
  need(e.contains("selected_work_envelope")&&c.at("mode")==e.at("selected_work_envelope")&&c.at("source_DAG_exact")==false,"explicit conditional work variant");
  if(owner=="initial_rope_summary")return std::make_unique<current_initial_rope_summary::Binding>(templates,c,map);
  need(owner=="initial_cat_summary","closed summary namespace");return std::make_unique<current_initial_cat::Binding>(templates,c,map);
 }
};
class TinyHandle final:public tiny_full::KernelBinding {
 std::shared_ptr<State>owner_;std::unique_ptr<tiny_full::KernelBinding>binding_;
public:TinyHandle(std::shared_ptr<State>s,U id):owner_(std::move(s)),binding_(owner_->tiny(id)){need(binding_->ctas()==num(owner_->entry(id).at("expected").at("CTAs")),"actual full current grid");}
 U ctas()const override{return binding_->ctas();}unsigned warps(U c)const override{return binding_->warps(c);}unsigned resident_limit()const override{return binding_->resident_limit();}U first_node(U c)const override{return binding_->first_node(c);}std::span<const tiny_full::SourceNode>nodes(U c)const override{return binding_->nodes(c);}U template_class(U c)const override{return binding_->template_class(c);}tiny_full::MemoryDescriptor memory(U c,unsigned n)const override{return binding_->memory(c,n);}J evidence()const override{return binding_->evidence();}
};
class Catalog {
 std::shared_ptr<State>state_;
public:Catalog(const J&p,const g::L2DramAddressMapper&base,const coupling::ServiceMapper&current,Options options={}):state_(std::make_shared<State>(p,base,current,options)){}
 const J&entry(U id)const{return state_->entry(id);}const J&manifest()const{return state_->manifest;}const J&checked_call(U id){return state_->call(id);}
 std::unique_ptr<tiny_full::KernelBinding>bindTiny(U id){return std::make_unique<TinyHandle>(state_,id);}
 std::shared_ptr<current_history_loader::FineHandle>bindFine(U id){const auto&e=state_->admitted(id);need(e.at("execution_route")=="Fine"&&e.at("scope_group")=="regular","original Fine default route");return state_->regular->bindFine(id);}
 J runtime_expected(U id)const{const auto&e=state_->admitted(id).at("expected");return {{"CTAs",e.at("CTAs")},{"nodes",e.at("nodes")},{"requested_read_bytes",e.at("logical_read_bytes")},{"requested_write_bytes",e.at("logical_write_bytes")}};}
 J loading_receipt()const{J pins=J::array();for(const auto&p:state_->read_pins)pins.push_back(p.second);return {{"opened_r3_pins",pins},{"regular",state_->regular->loading_receipt()},{"controls_tiny_explicit_nondefault",state_->options.controls_tiny},{"initialization_work_selection",mode(state_->options.initialization_work)},{"initialization_summary_is_original_SASS_DAG",false},{"min_max_are_hardware_cycle_bounds",false},{"continuous_history_executed",false}};}
};
// Single-threaded lazy owners. External base/current mapper must outlive all
// returned handles/stores/callbacks. No implicit L1/L2/backend/clock/PRNG reset.
}
