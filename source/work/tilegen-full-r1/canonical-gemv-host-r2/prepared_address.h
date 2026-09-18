#pragma once
#include "../canonical-gemv-driver-r2/model_plan.h"
namespace canonical_gemv {
namespace host_address { inline bool json_reference=false;inline bool materialization_hash=false; }
// Per-call immutable metadata, after the original Model has validated the plan.
// The original codec performs every CTA/lane rule and source bound calculation.
struct PreparedAddress {
 struct Binding{U source_base,target_base,target_bytes;};
 const p::Program& program;std::vector<Binding> bindings;
 PreparedAddress(const Model&m,const J&c):program(m.source(c).program){
  const auto&t=m.tpl(c);const auto&roles=t.at("object_roles");p::need(roles.size()==program.objects.size(),"prepared object domain");bindings.reserve(roles.size());
  for(U i=0;i<roles.size();++i){auto role=roles.at(i).get<std::string>();const auto&o=c.at("objects").at(role);U origin=t.contains("object_argument_bases")?p::natural(t.at("object_argument_bases").at(i)):program.objects.at(i).base;bindings.push_back({origin,p::natural(o.at("pointer")),p::natural(o.at("bytes"))});}
 }
 U address(const p::Lane&l,U bytes,U cta)const{const auto&b=bindings.at(l.object);U source_va=program.address(l,bytes,cta);p::need(source_va>=b.source_base,"source offset");U off=source_va-b.source_base;p::need(p::add(off,bytes)<=b.target_bytes,"target affine extent");return p::add(b.target_base,off);}
};
}
