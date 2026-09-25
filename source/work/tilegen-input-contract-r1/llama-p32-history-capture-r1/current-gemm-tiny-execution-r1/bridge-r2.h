#pragma once
#include "../current-gemm-execution-r1/bridge-r2.h"
namespace current_gemm_tiny {
using namespace current_gemm;
inline J role_map(const J&objects){
 std::vector<std::pair<U,U>>spans;for(const auto&o:objects.items()){U lo=num(o.value().at("pointer")),bytes=num(o.value().at("bytes"));need(lo%128==0&&bytes%128==0,"typed role mapping alignment");spans.emplace_back(lo,p28::add(lo,bytes));}
 std::sort(spans.begin(),spans.end());J rows=J::array();U off=0,last=0;for(auto[lo,hi]:spans){need(lo>=last,"no typed role overlap");rows.push_back({{"source_base",lo},{"bytes",hi-lo},{"service_base",off}});off=p28::add(off,hi-lo);last=hi;}return {{"schema","SG_SOURCE_TO_SERVICE_MAP_V1"},{"qualification","PACKED_SOURCE_GENERATED_128B_LINES_NOT_PHYSICAL_HARDWARE_ADDRESSES"},{"spans",rows}};
}
inline void match_old(const J&e,const J&old){
 const J&b=e.at("binding");const std::string phase=b.at("phase"),op=old.at("phase");need(b.at("stream_u64")==0,"actual current default stream; old sealed call has no stream_u64 field");need((phase=="Warmup/"+op||phase=="Measured/"+op)&&b.at("layer")==old.at("layer")&&b.at("module_scope")==old.at("module_scope"),"same actual family phase/layer/module");
 for(auto k:{"code_sha256","parameter_layout_sha256","nonpointer_sha256","nonpointer_bytes","grid","block","context_id"})need(b.at(k)==old.at(k),std::string("exact old/current specialization ")+k);
 for(auto k:{"registers","dynamic_shared_bytes","static_shared_bytes","local_bytes_per_thread","binary_version","ptx_version","attribute_query_results"})need(b.at("current_resources").at(k)==old.at("native_resources").at(k),"observed resource equality without occupancy upgrade");
 for(auto role:{"weight","input","output"})need(b.at("objects").at(role).at("bytes")==old.at("objects").at(role).at("bytes"),"complete typed role extent equal");
 need(old.at("native_resources").at("max_active_blocks_per_sm")==1,"inherited original source resident capacity");
}
class Binding final:public tiny_full::KernelBinding {
 J current_,old_;std::unique_ptr<tiny_full::KernelBinding>graph_;const coupling::ServiceMapper&map_;
public:
 Binding(std::unique_ptr<tiny_full::KernelBinding>graph,const J&old,const J&entry,const J&frames,const coupling::ServiceMapper&map):current_(entry.at("binding")),old_(old),graph_(std::move(graph)),map_(map){
  validate_call(entry,frames);match_old(entry,old_);need(bool(graph_)&&graph_->evidence().at("canonical_call")==old_&&graph_->evidence().at("family")==entry.at("frame_key"),"original exact old-PID sealed Tiny binding required");need(graph_->ctas()==num(current_.at("grid")[0])&&graph_->resident_limit()==1,"full original CTA domain/capacity");
 }
 U ctas()const override{return graph_->ctas();}unsigned warps(U c)const override{return graph_->warps(c);}unsigned resident_limit()const override{return graph_->resident_limit();}U first_node(U c)const override{return graph_->first_node(c);}std::span<const tiny_full::SourceNode>nodes(U c)const override{return graph_->nodes(c);}U template_class(U c)const override{return graph_->template_class(c);}
 tiny_full::MemoryDescriptor memory(U c,unsigned member)const override{
  auto out=graph_->memory(c,member);if(out.path==tiny_full::Path::Shared)return out;
  need(out.path==tiny_full::Path::DirectGlobal||out.path==tiny_full::Path::AsyncGlobalToShared,"closed original global paths");need(out.global_subops.size()==1,"one explicit source subop including empty copy");
  std::unordered_set<U>seen;out.lines.clear();
  for(auto&sub:out.global_subops)for(auto&r:sub.ranges){
   const U end=p28::add(r.offset_bytes,r.byte_count);const J*cur=nullptr;U offset=0;
   for(const auto&kv:old_.at("objects").items()){const auto&o=kv.value();U lo=num(o.at("pointer")),hi=p28::add(lo,num(o.at("bytes")));if(r.offset_bytes>=lo&&end<=hi){need(!cur,"complete global range has unique old role");cur=&current_.at("objects").at(kv.key());offset=r.offset_bytes-lo;}}
   need(cur&&r.byte_count,"nonempty range owned by exact role");r.offset_bytes=p28::add(num(cur->at("pointer")),offset);need(p28::add(r.offset_bytes,r.byte_count)<=num(cur->at("end_exclusive")),"complete current range extent");
   const U last=(p28::add(r.offset_bytes,r.byte_count)-1)/128*128;for(U line=r.offset_bytes/128*128;;line=p28::add(line,128)){g::CacheLineKey key{1,line};(void)map_.map(key);if(seen.insert(line).second)out.lines.push_back(key);if(line==last)break;}
  }
  // Preserve instruction masks/source masks, explicit empty subop, lane order,
  // all shared destinations/service, bypass policy, and every SourceNode verbatim.
  return out;
 }
 J evidence()const override{auto e=graph_->evidence();e["original_canonical_call"]=e.at("canonical_call");e["original_source_launch_key"]=e.at("source_launch_key");e["canonical_call"]=current_;e["source_launch_key"]=current_.at("source_launch_key");e["current_binding"]=current_;e["current_bridge_schema"]="CURRENT_P32_GEMM_TINY_ROLE_REBASE_V1";e["original_PID_validator_modified"]=false;e["current_occupancy_observed"]=false;e["compute_transfer_qualified"]=false;e["full_simulation_admitted"]=false;return e;}
 const tiny_full::KernelBinding& original()const{return *graph_;}
 // Both old/current mappers and their underlying base mapper must outlive this
 // binding, its Port, and all actual callbacks. Template owner is shared const.
};
}
