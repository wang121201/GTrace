#pragma once
#include "fused_model_sources.h"
// Exact source-specific explicit-dependency MODEL admission only.
namespace native_family {
namespace fp=native_program;
inline bool is_fused_key(const std::string& key){for(const auto& r:fused_model_manifest().at("qualified_model_sources"))if(r.at("source_launch_key")==key)return true;return false;}
inline const fp::J& fused_seal(const std::string& key){for(const auto& r:fused_model_manifest().at("qualified_model_sources"))if(r.at("source_launch_key")==key)return r;fp::need(false,"unsealed Fused Norm model source");return fused_model_manifest();}
inline const fp::J& all_model_source_keys(){static const fp::J keys=[](){fp::J j=model_source_keys();for(const auto& r:fused_model_manifest().at("qualified_model_sources"))j.push_back(r.at("source_launch_key"));auto rank=[](const fp::J& x){auto s=x.get<std::string>();return std::make_pair(std::stoi(s.substr(6,s.find("-launch-")-6)),std::stoi(s.substr(s.find("-launch-")+8)));};std::sort(j.begin(),j.end(),[&](const fp::J& a,const fp::J& b){return rank(a)<rank(b);});return j;}();return keys;}
inline const fp::J& fused_descriptor(const fp::J& b){const auto& s=fused_seal(b.at("source_launch_key").get<std::string>());fp::need(b==s.at("source_begin"),"Fused exact current source BEGIN/arguments/resources differ");return s.at("descriptor");}
inline bool fused_memory_exact_or_holdout(const fp::J& m){const auto& b=m.at("source").at("begin");const auto& d=fused_descriptor(b);const auto& a=m.at("qualification").at("admission");
 if(d.at("sampling_mode")=="exact_anchors")return m.at("generation_kind")=="exact_anchors"&&m.at("grid")==fp::J::array({1,1,1})&&m.at("programs").size()==1&&m.at("programs")[0].at("source_cta")==0&&a.at("exact_anchor")==true&&a.at("independent_holdout_prediction")==false&&b.at("fit_ctas")==fp::J::array({0})&&b.at("holdout_ctas").empty();
 return m.at("generation_kind")=="affine_periodic"&&m.at("programs").size()==1&&m.at("programs")[0].at("source_cta")==0&&a.at("exact_anchor")==false&&a.at("independent_holdout_prediction")==true&&!b.at("holdout_ctas").empty();}
inline void fused_register_envelope(const fp::J& r,const fp::J& b){
 fp::need(r.at("schema")=="TILEGEN_FUSED_NORM_REGISTER_CANDIDATE_V1"&&r.at("status")=="CPU_CANDIDATE_NOT_NATIVE_QUALIFIED","Fused original explicit candidate envelope required");
 fp::need(r.value("synthetic_fixture_only",false)==false,"Fused cannot be a synthetic fixture");
 for(const char* k:{"native_qualified","timing_qualified","driver_admission","cycle_accuracy_claimed","pipeline_parameters_fitted","full_grid_dynamic_trace_saved","implicit_register_dependencies_complete"})fp::need(r.at(k)==false,std::string("Fused unchanged qualification ")+k);
 fp::need(r.at("full_LLM_time").is_null()&&r.at("bandwidth").is_null(),"Fused candidate cannot supply timing");
 const auto& d=fused_descriptor(b);const auto& s=fused_seal(b.at("source_launch_key").get<std::string>());
 fp::need(r.at("source_begin")==b&&r.at("source_launch_key")==b.at("source_launch_key"),"Fused compute/memory source binding");
 fp::need(r.at("grid")==d.at("grid")&&r.at("block")==d.at("block")&&r.at("code_sha256")==d.at("code_sha256"),"Fused geometry/code binding");
 fp::need(r.at("warps").size()==16&&r.at("shared_allocation_bytes")==16448&&r.at("native_resources")==b.at("native_resources"),"Fused complete warp/shared/native resources");
 fp::need(r.at("candidate_validation")==s.at("candidate_validation"),"Fused source formula/DAG proof differs");
 const auto& shared=r.at("shared_memory_order_contract");fp::need(shared==r.at("candidate_validation").at("dag").at("shared_order")&&shared.at("required_driver_all_warp_BAR_successor_completion_edges")==true,"Fused all-warp BAR completion contract");
 fp::U count=0;for(const auto& w:r.at("warps"))for(const auto& n:w.at("nodes")){fp::need(n.at("function_id")==b.at("function_id"),"Fused stale node function");++count;}fp::need(count==s.at("node_count"),"Fused reviewed node census");
}
inline void fused_native_binding(const fp::J& memory,const fp::J& reg){const auto& b=memory.at("source").at("begin");fused_register_envelope(reg,b);const auto& s=fused_seal(b.at("source_launch_key").get<std::string>());fp::need(memory.at("source").at("native_binding")==s.at("source_native_binding"),"Fused process/function/module binding differs");fp::need(fused_memory_exact_or_holdout(memory),"Fused exact singleton or heldout source proof required");}
inline void fused_sealed_artifact(const fp::J& entry){const auto& s=fused_seal(entry.at("program").at("source").at("begin").at("source_launch_key").get<std::string>());fp::need(entry.at("program_file").at("sha256")==s.at("memory_sha256")&&entry.at("register_file").at("sha256")==s.at("register_sha256"),"Fused differs from sealed source artifacts");}
inline const fp::J& fused_assumptions(const std::string& key){return fused_seal(key).at("source_assumptions");}
inline bool fused_barrier_form(const fp::J& n){return n.at("opcode")=="BAR.SYNC.DEFER_BLOCKING"&&((n.at("barrier_round")==0&&n.at("pc")==0xff0)||(n.at("barrier_round")==1&&n.at("pc")==0x1110));}
inline int fused_shared_width(const fp::J& n){
 const auto pc=fp::natural(n.at("pc"));const auto op=n.at("opcode").get<std::string>();const auto width=fp::natural(n.at("width"),16);
 const bool wide_store=pc==0x6e0||pc==0x700||pc==0xad0||pc==0xaf0||pc==0xef0||pc==0xf10;
 const bool wide_load=pc==0x1400||pc==0x1410||pc==0x17b0||pc==0x17c0||pc==0x1bc0||pc==0x1bd0;
 const bool scalar_store=pc==0xfe0||pc==0x10f0,scalar_load=pc==0x1030||pc==0x1140;
 fp::need((width==16&&((wide_store&&op=="STS.128")||(wide_load&&op=="LDS.128")))||(width==4&&((scalar_store&&op=="STS")||(scalar_load&&op=="LDS"))),"only sealed Fused shared code/PC/width forms");return int(width);
}
inline fp::J fused_model_scope(){return {{"schema","FUSED_NORM_EXPLICIT_DEPENDENCY_MODEL_SCOPE_V1"},{"modeled_source_count",3},{"original_candidate_native_qualified",false},{"full_native_hardware_qualified",false},{"control_domain","Only exact source observed control and independently checked candidate; no unvisited helper semantics"},{"implicit_descriptor_dependencies_complete",false},{"constant_service_readiness_recovered",false},{"BAR_DEFER_timing_recovered",false},{"shared_numeric_winner_recovered",false},{"driver_all_warp_BAR_completion_required",true},{"wide_shared_bank_phases_recovered",false},{"cross_process_or_target_transfer",false},{"full_LLM_qualified",false}};}
}
