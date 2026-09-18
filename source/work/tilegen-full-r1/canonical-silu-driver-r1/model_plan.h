#pragma once
#include "sealed_inputs.h"
namespace canonical_silu {
using namespace native_sequence;
struct Formula {U warp,ordinal,pc,width,occurrence,part;char op;std::string role,opcode;};
struct Model {
 J plan,templ;std::vector<Formula> records;std::vector<const J*> calls;std::unique_ptr<SourceCatalog> catalog;
 Model(const J& input){
  keys(input,{"schema","plan_file","memory_model","service_address_map","selected_source_keys","max_kernel_cycles","aggregate_observations"});
  p::need(input.at("schema")=="CANONICAL_SILU_MODELED_SEQUENCE_V1"&&input.at("aggregate_observations").is_boolean(),"explicit modeled-only input");const auto sealed=seals();p::need(input.at("plan_file")==sealed.at("plan_file")&&input.at("memory_model")==sealed.at("memory_model"),"fixed plan/model input seal");
  plan=strict_parse(check_pin(sealed.at("plan_file")));templ=strict_parse(check_pin(sealed.at("template_file")));
  p::need(plan.at("schema")=="CANONICAL_SILU_STRUCTURAL_MODEL_PLAN_V1"&&plan.at("driver_admitted")==false&&plan.at("native_target_qualified")==false&&plan.at("full_workflow_qualified")==false,"model estimate boundary");
  p::need(plan.at("template")==sealed.at("template_file")&&templ.at("register_program")==sealed.at("register_file"),"single frozen neutral template");
  J refs=J::array({{{"catalog_id","observed-structural-template"},{"program_file",sealed.at("program_file")},{"register_file",sealed.at("register_file")}}});catalog=std::make_unique<SourceCatalog>(refs,false);
  catalog->seals.evidence(sealed.at("plan_file"));catalog->seals.evidence(sealed.at("template_file"));catalog->seals.evidence(sealed.at("model_manifest"));catalog->seals.evidence(sealed.at("model_source"));
  const auto& s=source();p::need(s.program.warps==32&&s.registers.nodes.size()==11800&&s.registers.global_nodes==744&&s.registers.shared_nodes==0&&s.registers.barrier_nodes==0,"closed no-shared SiLU template");
  std::map<std::pair<U,U>,Formula> formulas;
  for(const auto& r:templ.at("memory_records")){Formula v{p::natural(r.at("warp"),31),p::natural(r.at("ordinal"),31),p::natural(r.at("pc")),p::natural(r.at("width"),16),p::natural(r.at("occurrence"),5),p::natural(r.at("part_bytes")),r.at("op").get<std::string>().at(0),r.at("role").get<std::string>(),r.at("opcode").get<std::string>()};
   p::need(formulas.emplace(std::make_pair(v.warp,v.ordinal),v).second,"unique template warp ordinal");
  }
  std::array<U,32> ordinal{};for(const auto& r:s.program.bodies[0].records){auto key=std::make_pair(U(r.warp),ordinal[r.warp]++);auto it=formulas.find(key);p::need(it!=formulas.end(),"missing formula ordinal");const auto& v=it->second;p::need(v.pc==r.pc&&v.width==U(r.width)&&v.op==r.op&&v.opcode==s.program.opcodes[r.opcode]&&r.mask==UINT32_MAX&&r.lanes.size()==32,"observed neutral memory source join");for(int lane=0;lane<32;++lane)p::need(r.lanes[lane].lane==lane,"original lane order");records.push_back(v);}
  p::need(records.size()==formulas.size()&&records.size()==744,"complete formula source bijection");
  const auto& chosen=input.at("selected_source_keys");p::need(chosen.is_array()&&chosen.size()>=1&&chosen.size()<=96,"bounded chosen source list");std::set<std::string>wanted;for(const auto& k:chosen)p::need(k.is_string()&&wanted.insert(k.get<std::string>()).second,"unique selected key");
  U prior_launch=0;J context;U total_ctas=0;
  for(const auto& c:plan.at("calls"))if(wanted.count(c.at("source_launch_key").get<std::string>())){validate_call(c);p::need(chosen.at(calls.size())==c.at("source_launch_key"),"actual launch order required");const auto launch=p::natural(c.at("native_launch_binding").at("native_launch_id"));p::need(launch>prior_launch,"strict native call order");prior_launch=launch;
   J current={{"process",c.at("process")},{"context_id",c.at("context_id")},{"stream_u64",c.at("stream_u64")}};if(calls.empty())context=current;else p::need(context==current,"same current PID/context/stream only");calls.push_back(&c);total_ctas+=p::natural(c.at("grid")[0]);}
  p::need(calls.size()==chosen.size()&&total_ctas<=1088,"chosen targets complete");p::need(input.at("service_address_map")==service_map(),"canonical current target VA mapping required");
 }
 const SourceBundle& source()const{return catalog->get("observed-structural-template");}
 void validate_call(const J& c)const{
  p::need(c.at("schema")=="CANONICAL_SILU_ESTIMATED_TARGET_V1"&&c.at("process")==plan.at("source_process")&&c.at("native_launch_binding").at("process")==c.at("process"),"actual current target process");
  p::need(c.at("native_launch_binding").at("source_launch_key")==c.at("source_launch_key")&&c.at("code_sha256")==source().input.at("register_program").at("code_sha256"),"target code and launch binding");
  for(auto key:{"estimated_compute","estimated_address"})p::need(c.at(key)==true,"explicit target estimate flag");for(auto key:{"native_target_qualified","driver_admitted","implicit_descriptor_dependencies_complete"})p::need(c.at(key)==false,"no hidden native qualification");
  p::need(c.at("implicit_descriptor_base").is_null()&&c.at("implicit_descriptor_width").is_null(),"no invented descriptor");
  auto phase=c.at("phase").get<std::string>();p::need(phase=="Prefill"||phase=="Decode1"||phase=="Decode2","phase domain");U rows=phase=="Prefill"?32:1;
  p::need(c.at("grid")==J::array({rows,1,1})&&c.at("block")==J::array({1024,1,1})&&p::natural(c.at("arguments").at("d"))==14336,"closed shape");
  const auto& res=c.at("native_resources");p::need(res.at("max_active_blocks_per_sm")==1&&res.at("threads_per_cta")==1024&&res.at("static_shared_bytes")==0&&res.at("dynamic_shared_bytes")==0&&res.at("occupancy_query_result")==0,"actual native capacity");
  for(auto role:{"input","out"}){const auto& o=c.at("objects").at(role);U base=p::natural(c.at("arguments").at(role)),width=std::string(role)=="input"?57344:28672,extent=rows*width;
   p::need(base>0&&base%16==0&&base==p::natural(o.at("pointer"))&&p::natural(o.at("bytes"))==extent&&p::add(base,extent)==p::natural(o.at("end_exclusive")),"actual pointer full extent");
   p::need(p::add(p::natural(o.at("root").at("base_address")),p::natural(o.at("storage_offset_bytes")))==base&&p::add(base,extent)<=p::add(p::natural(o.at("root").at("base_address")),p::natural(o.at("root").at("storage_nbytes"))),"logical current view root bounds");
   std::string logical=std::to_string(p::natural(c.at("process").at("pid")))+":"+std::to_string(p::natural(c.at("process").at("start_ticks")))+":"+o.at("root").at("id").get<std::string>();p::need(o.at("logical_identity")==logical,"separate logical object identity");}
  const auto& a=c.at("objects").at("input");const auto& b=c.at("objects").at("out");p::need(p::natural(a.at("end_exclusive"))<=p::natural(b.at("pointer"))||p::natural(b.at("end_exclusive"))<=p::natural(a.at("pointer")),"restrict nonalias");
 }
 U address(const J& c,U cta,const Formula& r,U lane)const{
  p::need(cta<p::natural(c.at("grid")[0])&&lane<32,"CTA lane domain");U tid=r.warp*32+lane,element;
  p::need((r.width==16||r.width==2)&&(r.role=="input"||r.role=="out"),"memory formula kind");
  if(r.width==16){p::need(r.occurrence<(r.warp<24?2:1),"vector loop count");element=(tid+1024*r.occurrence)*8;}else{p::need(r.occurrence<6,"scalar loop count");element=8192+tid+1024*r.occurrence;}
  p::need(element+r.width/2<=14336,"element extent");U base=p::natural(c.at("arguments").at(r.role)),stride=r.role=="input"?57344:28672;
  U a=p::add(base,p::add(cta*stride,p::add(r.part,element*2)));const auto&o=c.at("objects").at(r.role);p::need(a>=p::natural(o.at("pointer"))&&p::add(a,r.width)<=p::natural(o.at("end_exclusive")),"target source span");return a;
 }
 J service_map()const{
  std::vector<std::pair<U,U>> spans;for(auto* c:calls)for(auto role:{"input","out"}){const auto&o=c->at("objects").at(role);U base=p::natural(o.at("pointer")),end=p::natural(o.at("end_exclusive"));p::need(end<=UINT64_MAX-127,"aligned map extent");spans.emplace_back(base/128*128,(end+127)/128*128);}std::sort(spans.begin(),spans.end());std::vector<std::pair<U,U>> merged;for(auto x:spans){if(!merged.empty()&&x.first<=merged.back().second)merged.back().second=std::max(x.second,merged.back().second);else merged.push_back(x);}J rows=J::array();U offset=0;for(auto [lo,hi]:merged){rows.push_back({{"source_base",lo},{"bytes",hi-lo},{"service_base",offset}});offset=p::add(offset,hi-lo);}return {{"schema","SG_SOURCE_TO_SERVICE_MAP_V1"},{"qualification","PACKED_SOURCE_GENERATED_128B_LINES_NOT_PHYSICAL_HARDWARE_ADDRESSES"},{"spans",rows}};
 }
 J address_oracle(const J& c)const {tiny_sha::Sha256 h;U count=0;for(U cta=0;cta<p::natural(c.at("grid")[0]);++cta)for(U ri=0;ri<records.size();++ri){const auto&r=records[ri];for(U lane=0;lane<32;++lane){h.add(std::to_string(cta)+":"+std::to_string(r.warp)+":"+std::to_string(r.ordinal)+":"+std::to_string(lane)+":"+std::to_string(address(c,cta,r,lane))+"\n");++count;}}return {{"source_launch_key",c.at("source_launch_key")},{"lane_addresses",count},{"sha256",h.hex()}};}
};
}
