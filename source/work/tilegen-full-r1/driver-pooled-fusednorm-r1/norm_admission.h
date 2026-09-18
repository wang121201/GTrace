#pragma once
#include "norm_model_sources.h"
// Only exact, separately reviewed explicit-dependency source candidates.
// The original candidate retains native_qualified=false; this is model admission.
namespace native_family {
namespace np=native_program;
inline bool is_norm_key(const std::string& key){for(const auto& r:norm_model_manifest().at("qualified_model_sources"))if(r.at("source_launch_key")==key)return true;return false;}
inline const np::J& norm_seal(const std::string& key){for(const auto& r:norm_model_manifest().at("qualified_model_sources"))if(r.at("source_launch_key")==key)return r;np::need(false,"unsealed Norm model source");return norm_model_manifest();}
inline const np::J& norm_descriptor(const np::J& b){
 const auto& s=norm_seal(b.at("source_launch_key").get<std::string>());
 np::need(b==s.at("source_begin"),"Norm exact current source BEGIN/arguments/resources differ");
 return s.at("descriptor");
}
inline bool norm_memory_exact_or_holdout(const np::J& m){
 const auto& b=m.at("source").at("begin");const auto& d=norm_descriptor(b);const auto& a=m.at("qualification").at("admission");
 if(d.at("sampling_mode")=="exact_anchors")return m.at("generation_kind")=="exact_anchors"&&m.at("grid")==np::J::array({1,1,1})&&m.at("programs").size()==1&&m.at("programs")[0].at("source_cta")==0&&a.at("exact_anchor")==true&&a.at("independent_holdout_prediction")==false&&b.at("fit_ctas")==np::J::array({0})&&b.at("holdout_ctas").empty();
 return m.at("generation_kind")=="affine_periodic"&&m.at("programs").size()==1&&m.at("programs")[0].at("source_cta")==0&&a.at("exact_anchor")==false&&a.at("independent_holdout_prediction")==true&&!b.at("holdout_ctas").empty();
}
inline void norm_register_envelope(const np::J& r,const np::J& b){
 np::need(r.at("schema")=="TILEGEN_PLAIN_NORM_REGISTER_CANDIDATE_V1"&&r.at("status")=="CPU_CANDIDATE_NOT_NATIVE_QUALIFIED","Norm original explicit candidate envelope required");
 np::need(r.value("synthetic_fixture_only",false)==false,"Norm cannot be a synthetic fixture");
 for(const char* k:{"native_qualified","timing_qualified","driver_admission","cycle_accuracy_claimed","pipeline_parameters_fitted","full_grid_dynamic_trace_saved"})np::need(r.at(k)==false,std::string("Norm unchanged candidate qualification ")+k);
 np::need(r.at("full_LLM_time").is_null()&&r.at("bandwidth").is_null(),"Norm candidate cannot supply timing");
 const auto& d=norm_descriptor(b);const auto& s=norm_seal(b.at("source_launch_key").get<std::string>());
 np::need(r.at("source_begin")==b&&r.at("source_launch_key")==b.at("source_launch_key"),"Norm compute/memory source binding");
 np::need(r.at("grid")==d.at("grid")&&r.at("block")==d.at("block")&&r.at("code_sha256")==d.at("code_sha256"),"Norm geometry/code binding");
 np::need(r.at("warps").size()==16&&r.at("shared_allocation_bytes")==64&&r.at("native_resources")==b.at("native_resources"),"Norm full warp/shared/native resources");
 np::need(r.at("candidate_validation")==s.at("candidate_validation"),"Norm independent formula/DAG proof differs");
 const auto& shared=r.at("shared_memory_order_contract");np::need(shared==r.at("candidate_validation").at("dag").at("shared_order")&&shared.at("required_driver_all_warp_BAR_successor_completion_edges")==true,"Norm requires all-warp BAR completion contract");
 np::U count=0;for(const auto& w:r.at("warps"))for(const auto& n:w.at("nodes")){np::need(n.at("function_id")==b.at("function_id"),"Norm node stale function");++count;}
 np::need(count==2991,"Norm reviewed node census");
}
inline void norm_native_binding(const np::J& memory,const np::J& reg){
 const auto& b=memory.at("source").at("begin");norm_register_envelope(reg,b);const auto& s=norm_seal(b.at("source_launch_key").get<std::string>());
 np::need(memory.at("source").at("native_binding")==s.at("source_native_binding"),"Norm native process/function/module binding differs");
 np::need(norm_memory_exact_or_holdout(memory),"Norm exact singleton or heldout source proof required");
}
inline void norm_sealed_artifact(const np::J& entry){
 const auto& s=norm_seal(entry.at("program").at("source").at("begin").at("source_launch_key").get<std::string>());
 np::need(entry.at("program_file").at("sha256")==s.at("memory_sha256")&&entry.at("register_file").at("sha256")==s.at("register_sha256"),"Norm source differs from reviewed explicit-model artifacts");
}
inline const np::J& norm_assumptions(const std::string& key){return norm_seal(key).at("source_assumptions");}
inline bool norm_barrier_form(const np::J& n){return n.at("opcode")=="BAR.SYNC.DEFER_BLOCKING"&&((n.at("barrier_round")==0&&n.at("pc")==0xd60)||(n.at("barrier_round")==1&&n.at("pc")==0xe80));}
inline np::J norm_model_scope(){return {{"schema","NORM_EXPLICIT_DEPENDENCY_MODEL_SCOPE_V1"},{"modeled_source_count",2},{"original_candidate_native_qualified",false},{"full_native_hardware_qualified",false},{"BRA_DIV","Only source PC0xdb0 warp0 observed ~URZ fallthrough to0xdc0"},{"implicit_descriptor_dependencies_complete",false},{"constant_service_readiness_recovered",false},{"BAR_DEFER_timing_recovered",false},{"shared_numeric_winner_recovered",false},{"driver_all_warp_BAR_completion_required",true},{"cross_process_or_target_transfer",false},{"full_LLM_qualified",false}};}
}
