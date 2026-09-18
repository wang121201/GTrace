#pragma once
#include "silu_catalog.h"
#include "silu_qualified.h"
namespace native_family {
namespace sp=native_program;
inline void silu_keys(const sp::J& j,std::initializer_list<const char*> names){
 sp::need(j.is_object()&&j.size()==names.size(),"closed SiLU object fields");
 for(const auto* name:names)sp::need(j.contains(name),"missing SiLU object field");
}
inline bool is_silu_key(const std::string& key){for(const auto& d:silu_catalog())if(d.at("source_launch_key")==key)return true;return false;}
inline sp::U silu_argument(const sp::J& arg,int index,int width){
 silu_keys(arg,{"index","size_bytes","parameter_buffer_offset","raw_bytes_hex","sha256"});
 sp::need(sp::natural(arg.at("index"))==sp::U(index)&&sp::natural(arg.at("size_bytes"))==sp::U(width)&&arg.at("parameter_buffer_offset").is_null(),"separate native SiLU argument layout");
 const auto hex=arg.at("raw_bytes_hex").get<std::string>();sp::need(hex.size()==std::size_t(width*2),"SiLU argument byte width");
 std::string bytes;sp::U value=0;
 auto nibble=[](char c)->unsigned{sp::need((c>='0'&&c<='9')||(c>='a'&&c<='f'),"SiLU argument hex");return c<='9'?c-'0':c-'a'+10;};
 for(int i=0;i<width;++i){unsigned v=(nibble(hex[i*2])<<4)|nibble(hex[i*2+1]);bytes.push_back(char(v));value|=sp::U(v)<<(i*8);}
 sp::need(tiny_sha::sha256(bytes)==arg.at("sha256").get<std::string>(),"SiLU argument raw-byte SHA");return value;
}
inline const sp::J& silu_descriptor(const sp::J& b){
 const sp::J* row=nullptr;for(const auto& d:silu_catalog())if(d.at("source_launch_key")==b.at("source_launch_key"))row=&d;sp::need(row,"sealed SiLU key");const auto& d=*row;
 for(const char* k:{"source_launch_key","phase","module_scope","code_sha256","grid","block","fit_ctas","holdout_ctas"})sp::need(b.at(k)==d.at(k),std::string("sealed SiLU descriptor ")+k);
 sp::need(d.at("warps_per_cta")==32&&d.at("block")==sp::J::array({1024,1,1})&&d.at("shared_bytes")==0,"closed SiLU geometry");
 sp::need(sp::natural(b.at("host_argument_bytes"))==20&&b.at("host_argument_u32").is_null(),"SiLU separate20B ABI, no padded argument image");
 const auto& a=b.at("host_arguments");silu_keys(a,{"schema","argument_transport","capture_before_original_launch","device_memory_dereferenced","parameter_layout_sha256","total_bytes","arguments"});
 sp::need(a.at("schema")=="SG_PROGRAM_HOST_ARGUMENTS_V1"&&a.at("argument_transport")=="kernelParams"&&a.at("capture_before_original_launch")==true&&a.at("device_memory_dereferenced")==false,"native SiLU argument origin");
 sp::need(a.at("parameter_layout_sha256")==d.at("parameter_layout_sha256")&&sp::natural(a.at("total_bytes"))==20&&a.at("arguments").is_array()&&a.at("arguments").size()==3,"SiLU native ABI binding");
 sp::need(silu_argument(a.at("arguments")[0],0,8)>0&&silu_argument(a.at("arguments")[1],1,8)>0&&silu_argument(a.at("arguments")[2],2,4)==14336,"SiLU pointer/dimension arguments");
 const auto& r=b.at("native_resources");sp::need(r.at("threads_per_cta")==1024&&r.at("max_active_blocks_per_sm")==1&&r.at("registers")==40&&r.at("static_shared_bytes")==0&&r.at("dynamic_shared_bytes")==0&&r.at("local_bytes_per_thread")==0&&r.at("binary_version")==89&&r.at("ptx_version")==89,"sealed native SiLU resource attributes");return d;
}
inline bool memory_holdout_or_exact(const sp::J& memory){
 const auto& b=memory.at("source").at("begin");const auto& a=memory.at("qualification").at("admission");
 if(!is_silu_key(b.at("source_launch_key").get<std::string>()))return a.at("independent_holdout_prediction")==true;
 const auto& d=silu_descriptor(b);
 if(d.at("sampling_mode")=="exact_anchors")return memory.at("generation_kind")=="exact_anchors"&&memory.at("grid")==sp::J::array({1,1,1})&&memory.at("programs").size()==1&&memory.at("programs")[0].at("source_cta")==0&&a.at("exact_anchor")==true&&a.at("independent_holdout_prediction")==false&&b.at("fit_ctas")==sp::J::array({0})&&b.at("holdout_ctas").empty();
 return memory.at("generation_kind")=="affine_periodic"&&a.at("exact_anchor")==false&&a.at("independent_holdout_prediction")==true&&!b.at("holdout_ctas").empty();
}
inline void silu_native_binding(const sp::J& memory,const sp::J& reg){
 const auto& b=memory.at("source").at("begin");const auto& d=silu_descriptor(b);const auto& binding=memory.at("source").at("native_binding");const auto& sig=binding.at("kernel_metadata_signature");
 for(const char* k:{"code_sha256","grid","block","parameter_layout_sha256"})sp::need(sig.at(k)==d.at(k),std::string("SiLU native metadata ")+k);
 sp::need(binding.at("phase")==d.at("phase")&&binding.at("canonical_module")==d.at("module_scope")&&binding.at("function_id")==b.at("function_id"),"SiLU module/phase/function binding");
 sp::need(sig.at("static_shared_bytes")==0&&sig.at("dynamic_shared_bytes")==0&&sig.at("registers")==40,"SiLU metadata resources");
 sp::need(reg.at("warps").size()==32&&reg.at("shared_allocation_bytes")==0&&memory_holdout_or_exact(memory),"SiLU complete source scope");
 for(const auto& w:reg.at("warps"))for(const auto& n:w.at("nodes"))sp::need(n.at("function_id")==b.at("function_id")&&n.at("kind")!="shared"&&n.at("kind")!="barrier","SiLU node function and no-shared contract");
 const auto& q=reg.at("independent_qualification");sp::need(q.at("formula").at("status")=="PASS_OBSERVED_PROGRAM_AND_SOURCE_FORMULAS"&&q.at("dag").at("status")=="PASS_NATIVE_PC_DEPENDENCY_AND_MEMORY_BINDING","SiLU independent source audits required");
 sp::need(q.at("dag").at("shared_memory_order").at("status")=="NOT_APPLICABLE_NO_SHARED_OPERATIONS","SiLU no-shared audit");
 std::vector<sp::U> selected;for(const char* k:{"fit_ctas","holdout_ctas"})for(const auto& c:d.at(k))selected.push_back(sp::natural(c));std::sort(selected.begin(),selected.end());sp::need(reg.at("sampled_ctas")==sp::J(selected),"SiLU selected CTA coverage");
}
inline void silu_sealed_artifact(const sp::J& entry){
 const auto key=entry.at("program").at("source").at("begin").at("source_launch_key");const sp::J* row=nullptr;for(const auto& q:silu_qualified())if(q.at("source_launch_key")==key)row=&q;sp::need(row,"SiLU source has no independently qualified compute artifact");
 sp::need(entry.at("program_file").at("sha256")==row->at("memory_sha256")&&entry.at("register_file").at("sha256")==row->at("register_sha256"),"SiLU artifact differs from frozen independent qualification");
}
}
