#pragma once
namespace hybrid_full {
struct Options {
 bool all_fine=false,silu_q1=false;unsigned q=16;unsigned phase_width=1;unsigned prefill_compute_width=1;unsigned memory_epoch=1;
 std::set<std::string> fine_keys,fine_families;
 bool use_tiny(const std::string& family,const std::string& key)const {
  return !all_fine&&Bindings::supports(family)&&!fine_keys.count(key)&&!fine_families.count(family);
 }
 void validate(const canonical_full::Model& model)const {
  std::set<std::string> keys,families;for(const auto&t:model.calls){keys.insert(t.call->at("source_launch_key").get<std::string>());families.insert(t.family);}
  for(const auto&k:fine_keys)native_program::need(keys.count(k),"fine override must name a selected canonical call");
  for(const auto&f:fine_families)native_program::need(families.count(f),"fine override family must occur in selected canonical calls");
 }
 bool parse(const std::string&a){
  if(a=="--memory-epoch=1"){memory_epoch=1;return true;}
  if(a=="--memory-epoch=4"){memory_epoch=4;return true;}
  if(a=="--memory-epoch=8"){memory_epoch=8;return true;}
  if(a=="--prefill-compute=1"){prefill_compute_width=1;return true;}
  if(a=="--prefill-compute=4"){prefill_compute_width=4;return true;}
  if(a=="--prefill-compute=16"){prefill_compute_width=16;return true;}
  if(a=="--memory-phase=1"){phase_width=1;return true;}
  if(a=="--memory-phase=4"){phase_width=4;return true;}
  if(a=="--memory-phase=16"){phase_width=16;return true;}
  if(a=="--all-fine"){all_fine=true;return true;}
  if(a=="--silu-q1"){silu_q1=true;return true;}
  if(a=="--q1"){q=1;return true;}if(a=="--q4"){q=4;return true;}if(a=="--q8"){q=8;return true;}if(a=="--q16"){q=16;return true;}
  if(a.rfind("--fine-key=",0)==0){auto v=a.substr(11);native_program::need(!v.empty()&&fine_keys.insert(v).second,"unique nonempty fine key override");return true;}
  if(a.rfind("--fine-family=",0)==0){auto v=a.substr(14);native_program::need(Bindings::supports(v)&&fine_families.insert(v).second,"known unique fine family override");return true;}
  return false;
 }
};
inline Options options;
}
