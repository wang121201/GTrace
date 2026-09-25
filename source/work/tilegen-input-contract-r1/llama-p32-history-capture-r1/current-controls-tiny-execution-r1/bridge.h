#pragma once
#include "../current-controls-execution-r1/bridge.h"
// Isolated Tiny view over the sealed current76 Helper Plan/Model.
// Reuses exactly the Helper12 SourceNode/MemoryDescriptor mapping, with the
// current76 admitted rebase function instead of Helper12's two-role tail gate.
namespace current_controls {
class TinyBinding final:public tiny_full::KernelBinding {
 Binding source_;
 std::vector<std::vector<tiny_full::SourceNode>>classes_;
public:
 TinyBinding(const Plan&p,U id,const coupling::ServiceMapper&map):source_(p,id,map){
  for(const auto&cl:source_.m.classes){std::vector<tiny_full::SourceNode>ns;
   for(const auto&n:cl->nodes){tiny_full::SourceNode s;s.id=ns.size();s.warp=n.warp;s.ordinal=n.ordinal;s.pc=n.pc;s.source_ordinal=n.old;s.mask=n.effective;s.compute_elements=n.elements;s.pipeline=n.pipe;s.write=n.memop=="W";
    p::need(n.kind=="global"||n.kind=="shared"||n.kind=="barrier"||n.kind=="control"||n.kind=="compute","closed Helper class has no implicit async-copy conversion");
    s.kind=n.kind=="global"?tiny_full::Kind::Global:n.kind=="shared"?tiny_full::Kind::Shared:n.kind=="barrier"?tiny_full::Kind::Barrier:n.kind=="control"?tiny_full::Kind::Control:tiny_full::Kind::Compute;
    s.completion_dependencies.assign(n.deps.begin(),n.deps.end());s.issue_dependencies.assign(n.issues.begin(),n.issues.end());ns.push_back(std::move(s));
   }classes_.push_back(std::move(ns));
  }
 }
 U ctas()const override{return source_.m.ctas;}
 unsigned warps(U c)const override{p::need(c<ctas(),"current controls CTA");return source_.m.warps;}
 unsigned resident_limit()const override{return source_.m.resident;}
 U first_node(U c)const override{return source_.m.spans.at(c).first_node;}
 std::span<const tiny_full::SourceNode>nodes(U c)const override{return classes_.at(source_.m.class_for_cta.at(c));}
 U template_class(U c)const override{return source_.m.class_for_cta.at(c);}
 tiny_full::MemoryDescriptor memory(U c,unsigned member)const override{
  const auto&n=source_.m.at(c).nodes.at(member);p::need(n.kind=="global"||n.kind=="shared","original Helper memory node");tiny_full::MemoryDescriptor d;d.write=n.memop=="W";d.path=n.kind=="global"?tiny_full::Path::DirectGlobal:tiny_full::Path::Shared;g::ExplicitMemorySubop sub;std::set<U>lines;
  for(const auto&r:n.kind=="global"?n.global:n.shared){auto x=n.kind=="global"?source_.m.range(c,n,r):r;
   if(n.kind=="global"){x.address=source_.rebase(x.address,x.width);for(U line=x.address/128*128,last=(x.address+x.width-1)/128*128;;line+=128){(void)source_.mapper.map({1,line});lines.insert(line);if(line==last)break;}}
   sub.ranges.push_back({x.lane,x.address,x.width});sub.source_member_ordinals.push_back(x.lane);sub.requested_bytes+=x.width;
  }
  if(n.kind=="global"){d.global_bytes=sub.requested_bytes;d.global_subops.push_back(std::move(sub));for(U line:lines)d.lines.push_back({1,line});}
  else{d.shared_bytes=sub.requested_bytes;d.shared_service=g::describe_explicit_sram_ranges(sub);d.shared_subops.push_back(std::move(sub));}return d;
 }
 J evidence()const override{return {{"native_launch_id",source_.current.at("native_launch_id")},{"source_model_key",source_.m.key},{"source_call",source_.m.call},{"all_original_compute_control_shared_barrier_nodes_preserved",true},{"all_original_issue_and_completion_edges_preserved",true},{"global_address_change","current admitted role rebase only"},{"async_copy_nodes_in_this_helper_domain",0},{"source_occupancy_assumption",source_.m.resident},{"current_occupancy_observed",false},{"compute_transfer_qualified",false},{"full_simulation_admitted",false},{"Tiny_execution_tested_in_sealed_Fine_component",false}};}
};
}
