#pragma once
#include "linear31_admission.h"
#include "a67_catalog.h"
#include "a67_qualified.h"
#include "silu_admission.h"
#include "norm_admission.h"
#include "fused_admission.h"
namespace native_family {
namespace p=native_program;using J=p::J;using U=p::U;
inline bool is_a67_key(const std::string& key){for(const auto& d:a67_catalog())if(d.at("source_launch_key")==key)return true;return false;}
inline const J& a67_descriptor(const J& begin){
 const J* row=nullptr;for(const auto& d:a67_catalog())if(d.at("source_launch_key")==begin.at("source_launch_key"))row=&d;p::need(row,"unsealed a67 source key");const auto& d=*row;
 for(const char* k:{"source_launch_key","phase","module_scope","code_sha256","grid","block","fit_ctas","holdout_ctas"})p::need(begin.at(k)==d.at(k),std::string("sealed a67 descriptor ")+k);
 p::need(d.at("block")==J::array({16,4,1})&&d.at("warps_per_cta")==2,"sealed a67 two-warp geometry");
 p::need(begin.at("host_argument_bytes")==152&&begin.at("host_argument_u32").is_array()&&begin.at("host_argument_u32").size()==38,"fresh a67 152B arguments");for(const auto& v:begin.at("host_argument_u32"))(void)p::natural(v,UINT32_MAX);
 p::need(begin.at("host_argument_u32")[16]==d.at("K")&&begin.at("host_argument_u32")[17]==d.at("N"),"a67 K/N argument binding");
 const auto& r=begin.at("native_resources");p::need(p::add(p::natural(r.at("static_shared_bytes")),p::natural(r.at("dynamic_shared_bytes")))==p::natural(d.at("shared_bytes")),"a67 shared allocation");
 p::need(r.at("threads_per_cta")==64&&r.at("max_active_blocks_per_sm")==6&&r.at("registers")==168,"a67 sealed native resource capacity");return d;
}
inline const J& begin_descriptor(const J& b){if(is_fused_key(b.at("source_launch_key").get<std::string>()))return fused_descriptor(b);if(is_norm_key(b.at("source_launch_key").get<std::string>()))return norm_descriptor(b);return is_silu_key(b.at("source_launch_key").get<std::string>())?silu_descriptor(b):is_a67_key(b.at("source_launch_key").get<std::string>())?a67_descriptor(b):native_linear31::begin_descriptor(b);}
inline std::size_t index_for(const std::string& key){for(std::size_t i=0;i<all_model_source_keys().size();++i)if(all_model_source_keys()[i]==key)return i;p::need(false,"unsealed selected source descriptor");return 0;}
inline U expected_threads(const J& b){if(is_fused_key(b.at("source_launch_key").get<std::string>()))return 512;if(is_norm_key(b.at("source_launch_key").get<std::string>()))return 512;return is_silu_key(b.at("source_launch_key").get<std::string>())?1024:is_a67_key(b.at("source_launch_key").get<std::string>())?64:128;}
inline void native_binding(const J& memory,const J& reg){
 const auto& b=memory.at("source").at("begin");if(is_fused_key(b.at("source_launch_key").get<std::string>())){fused_native_binding(memory,reg);return;}if(is_norm_key(b.at("source_launch_key").get<std::string>())){norm_native_binding(memory,reg);return;}if(is_silu_key(b.at("source_launch_key").get<std::string>())){silu_native_binding(memory,reg);return;}if(!is_a67_key(b.at("source_launch_key").get<std::string>())){native_linear31::native_binding(memory,reg);return;}
 const auto& d=a67_descriptor(b);const auto& binding=memory.at("source").at("native_binding");const auto& sig=binding.at("kernel_metadata_signature");
 for(const char* k:{"code_sha256","grid","block","parameter_layout_sha256"})p::need(sig.at(k)==d.at(k),std::string("a67 native metadata descriptor ")+k);
 p::need(binding.at("phase")==d.at("phase")&&binding.at("canonical_module")==d.at("module_scope"),"a67 native module/phase");
 p::need(p::add(p::natural(sig.at("static_shared_bytes")),p::natural(sig.at("dynamic_shared_bytes")))==p::natural(d.at("shared_bytes"))&&sig.at("registers")==b.at("native_resources").at("registers"),"a67 native metadata resource binding");
 p::need(binding.at("function_id")==b.at("function_id"),"a67 fresh native function binding");
 p::need(reg.at("warps").size()==2&&reg.at("shared_allocation_bytes")==d.at("shared_bytes"),"a67 complete warp/shared binding");
 for(const auto& w:reg.at("warps"))for(const auto& n:w.at("nodes"))p::need(n.at("function_id")==b.at("function_id"),"a67 node stale function_id");
 const auto& q=reg.at("independent_qualification");p::need(q.at("formula").at("status")=="PASS_OBSERVED_PROGRAM_AND_SOURCE_FORMULAS"&&q.at("dag").at("status")=="PASS_NATIVE_PC_DEPENDENCY_AND_MEMORY_BINDING","a67 formula/DAG audit required");
 p::need(q.at("dag").at("shared_memory_order").at("status")=="PASS_ALL_SAMPLED_SHARED_ALIAS_OBLIGATIONS","a67 shared ordering audit required");
 std::vector<U> selected;for(const char* key:{"fit_ctas","holdout_ctas"})for(const auto& c:d.at(key))selected.push_back(p::natural(c));std::sort(selected.begin(),selected.end());p::need(reg.at("sampled_ctas")==J(selected),"a67 selected CTA coverage");
}
inline void sealed_artifact(const J& entry,bool fixture){
 if(fixture)return;const auto& b=entry.at("program").at("source").at("begin");const auto key=b.at("source_launch_key").get<std::string>();if(is_fused_key(key)){fused_sealed_artifact(entry);return;}if(is_norm_key(key)){norm_sealed_artifact(entry);return;}if(is_silu_key(key)){silu_sealed_artifact(entry);return;}if(!is_a67_key(key))return;
 const J* row=nullptr;for(const auto& q:a67_qualified())if(q.at("source_launch_key")==key)row=&q;p::need(row,"a67 descriptor has no fresh qualified compute artifact");
 p::need(entry.at("program_file").at("sha256")==row->at("memory_sha256")&&entry.at("register_file").at("sha256")==row->at("register_sha256"),"a67 artifact differs from independently qualified frozen source");
}
inline bool model_memory_holdout_or_exact(const J& m){if(is_fused_key(m.at("source").at("begin").at("source_launch_key").get<std::string>()))return fused_memory_exact_or_holdout(m);return is_norm_key(m.at("source").at("begin").at("source_launch_key").get<std::string>())?norm_memory_exact_or_holdout(m):memory_holdout_or_exact(m);}

}
