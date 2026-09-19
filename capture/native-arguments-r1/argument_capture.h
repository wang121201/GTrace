// Host-only argument transport. No CUDA work, pointer chasing, or parameter writes.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace sgargs {
using std::string;
inline void need(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
inline uint64_t add(uint64_t a, uint64_t b, uint64_t cap) {
  need(a <= cap && b <= cap - a, "argument checked aggregate bound");
  return a + b;
}
struct Entry {
  uint64_t epoch=0, ordinal=0, module_ordinal=0;
  int layer=-1;
  string phase,module,code,code_kind,layout,attrs,api;
  std::array<uint64_t,3> grid{{0,0,0}},block{{0,0,0}};
  uint64_t dynamic_shared=0,static_shared=0,registers=0,local_bytes=0;
  std::vector<uint64_t> sizes;
};
struct Actual: Entry {
  uint64_t pid=0,start_ticks=0,native_launch_id=0,call_id=0,context_id=0,function_id=0,stream=0;
  int64_t forward=-1;
  bool scope_bound=false;
  string role;
};
struct Limits {
  uint64_t launches=0,arguments=0,raw_bytes=0,max_arguments=0,max_argument_bytes=0,max_launch_bytes=0;
  uint64_t max_row_bytes=0,max_file_bytes=0;
};
struct Record {
  bool selected=false;
  uint64_t sequence=0;
  Actual launch;
  std::vector<string> raw;
};
inline string source_key(uint64_t epoch,uint64_t ordinal) {
  return "epoch-"+std::to_string(epoch)+"-launch-"+std::to_string(ordinal);
}
inline string quote(const string& s) {
  string out="\"";const char* h="0123456789abcdef";
  for(unsigned char c:s) {
    if(c=='\\'||c=='\"'){out+='\\';out+=char(c);}
    else if(c<32||c>=127){out+="\\u00";out+=h[c>>4];out+=h[c&15];}
    else out+=char(c);
  }
  return out+'\"';
}
inline string hex(const string& s) {
  string out;out.reserve(s.size()*2);const char* h="0123456789abcdef";
  for(unsigned char c:s){out+=h[c>>4];out+=h[c&15];}return out;
}
inline bool hash_text(const string& s) {
  return s.size()==64 && s.find_first_not_of("0123456789abcdef")==string::npos;
}
inline bool supported_api(const string& s) {
  return s=="cuLaunchKernel"||s=="cuLaunchKernel_ptsz"||s=="cuLaunchKernelEx"||s=="cuLaunchKernelEx_ptsz";
}
inline string dimensions(const std::array<uint64_t,3>& v) {
  return "["+std::to_string(v[0])+","+std::to_string(v[1])+","+std::to_string(v[2])+"]";
}
typedef string (*Hash)(const string&);
inline string serialize(const Record& r,Hash sha) {
  need(r.selected&&sha&&r.raw.size()==r.launch.sizes.size(),"argument serialize selected record");
  const auto& a=r.launch;const string key=source_key(a.epoch,a.ordinal);
  string out="{\"schema\":\"SG_NATIVE_ARGUMENT_VECTOR_V1\",\"sequence\":"+std::to_string(r.sequence)+
    ",\"native_launch_binding\":{\"process\":{\"pid\":"+std::to_string(a.pid)+",\"start_ticks\":"+std::to_string(a.start_ticks)+
    "},\"native_launch_id\":"+std::to_string(a.native_launch_id)+",\"source_launch_key\":"+quote(key)+"}"+
    ",\"source_launch_key\":"+quote(key)+",\"epoch_id\":"+std::to_string(a.epoch)+",\"epoch_launch_ordinal\":"+std::to_string(a.ordinal)+
    ",\"forward_id\":"+std::to_string(a.forward)+",\"phase\":"+quote(a.phase)+",\"cuda_api\":"+quote(a.api)+
    ",\"layer\":"+std::to_string(a.layer)+",\"module_scope\":"+quote(a.module)+",\"module_call_id\":"+std::to_string(a.call_id)+
    ",\"module_kernel_ordinal\":"+std::to_string(a.module_ordinal)+",\"context_id\":"+std::to_string(a.context_id)+
    ",\"function_id\":"+std::to_string(a.function_id)+",\"stream_u64\":"+std::to_string(a.stream)+
    ",\"code_sha256\":"+quote(a.code)+",\"code_sha256_kind\":"+quote(a.code_kind)+",\"parameter_layout_sha256\":"+quote(a.layout)+
    ",\"grid\":"+dimensions(a.grid)+",\"block\":"+dimensions(a.block)+",\"dynamic_shared_bytes\":"+std::to_string(a.dynamic_shared)+
    ",\"static_shared_bytes\":"+std::to_string(a.static_shared)+",\"registers\":"+std::to_string(a.registers)+
    ",\"local_bytes_per_thread\":"+std::to_string(a.local_bytes)+",\"launch_attributes\":"+a.attrs+
    ",\"argument_transport\":\"kernelParams\",\"capture_before_original_launch\":true,\"device_memory_dereferenced\":false,\"arguments\":[";
  for(size_t i=0;i<r.raw.size();++i) {
    need(r.raw[i].size()==a.sizes[i],"argument serialized width");
    if(i)out+=',';
    out+="{\"index\":"+std::to_string(i)+",\"size_bytes\":"+std::to_string(r.raw[i].size())+
      ",\"parameter_buffer_offset\":null,\"raw_bytes_hex\":"+quote(hex(r.raw[i]))+",\"sha256\":"+quote(sha(r.raw[i]))+"}";
  }
  return out+"]}";
}
inline void check_output_budget(const Limits& l,uint64_t used,uint64_t row_bytes) {
  need(row_bytes>0&&row_bytes<=l.max_row_bytes,"argument serialized row quota");
  add(used,row_bytes,l.max_file_bytes);
}

class Ledger {
  std::vector<Entry> plan_;
  Limits limits_;
  std::map<uint64_t,uint64_t> expected_epochs_,epochs_;
  std::map<std::pair<uint64_t,uint64_t>,uint64_t> module_ordinals_;
  std::set<uint64_t> selected_ids_,pending_;
  uint64_t returned_=0,args_=0,bytes_=0,pid_=0,ticks_=0,context_=0,stream_=0,last_id_=0;
  bool failed_=false;
public:
  Ledger(const std::vector<Entry>& plan,Limits limits):plan_(plan),limits_(limits) {
    need(limits.launches>0&&limits.launches<=100000&&plan.size()==limits.launches,"argument plan launch bound");
    need(limits.arguments>0&&limits.arguments<=256*limits.launches&&limits.raw_bytes>0&&limits.raw_bytes<=(64ull<<20),"argument total safety bounds");
    need(limits.max_arguments>0&&limits.max_arguments<=256&&limits.max_argument_bytes>0&&limits.max_argument_bytes<=65536&&
         limits.max_launch_bytes>0&&limits.max_launch_bytes<=(1ull<<20),"argument ABI safety bounds");
    need(limits.max_row_bytes>0&&limits.max_row_bytes<=(1ull<<20)&&limits.max_file_bytes>=limits.max_row_bytes&&
         limits.max_file_bytes<=(256ull<<20),"argument journal safety bounds");
    uint64_t args=0,bytes=0,epoch=1;
    for(const auto& e:plan_) {
      need(e.epoch>=1&&e.epoch<=64&&e.epoch>=epoch&&e.epoch<=epoch+1,"argument ordered plan epochs");epoch=e.epoch;
      need(e.ordinal==expected_epochs_[e.epoch]++&&e.module_ordinal<limits.launches,"argument ordered plan ordinals");
      need(e.layer>=-1&&e.layer<32&&e.phase.size()>0&&e.phase.size()<=128&&e.module.size()>0&&e.module.size()<=2048,"argument plan scope bounds");
      need(supported_api(e.api)&&hash_text(e.code)&&hash_text(e.layout)&&e.code_kind=="sha256_nvbit_decoded_instruction_rows_v1","argument plan code/API");
      need(!e.attrs.empty()&&e.attrs.size()<=8192&&e.attrs.front()=='['&&e.attrs.back()==']',"argument attribute envelope");
      for(auto n:e.grid)need(n>0&&n<=UINT32_MAX,"argument plan grid");
      for(auto n:e.block)need(n>0&&n<=1024,"argument plan block");
      need(!e.sizes.empty()&&e.sizes.size()<=limits.max_arguments,"argument plan vector bound");
      uint64_t sum=0;
      for(auto n:e.sizes){need(n>0&&n<=limits.max_argument_bytes,"argument plan width");sum=add(sum,n,limits.max_launch_bytes);}
      args=add(args,e.sizes.size(),limits.arguments);bytes=add(bytes,sum,limits.raw_bytes);
    }
    need(expected_epochs_.begin()->first==1&&expected_epochs_.size()==expected_epochs_.rbegin()->first,"argument contiguous plan epochs");
    need(args==limits.arguments&&bytes==limits.raw_bytes,"argument sealed aggregate totals");
  }
  Record before(Actual a,void** params,void** extra) {
    try {
      need(!failed_,"argument ledger already failed");
      Record r;
      if(!a.epoch){need(a.role!="measurement","unmarked measured argument launch");return r;}
      need(selected_ids_.size()<plan_.size(),"argument extra measured launch");
      const auto& e=plan_[selected_ids_.size()];
      a.ordinal=epochs_[a.epoch];a.module_ordinal=module_ordinals_[{a.epoch,a.call_id}];
      need(a.epoch==e.epoch&&a.ordinal==e.ordinal,"argument global launch order");
      need(a.scope_bound&&a.role=="measurement"&&a.forward==int64_t(a.epoch-1),"argument native measurement scope");
      need(a.pid&&a.start_ticks&&a.context_id&&a.function_id&&a.call_id,"argument fresh native identity");
      need(a.stream==0,"argument sealed default stream required");
      if(pid_)need(a.pid==pid_&&a.start_ticks==ticks_&&a.context_id==context_&&a.stream==stream_,"argument current-process single-stream identity");
      need(a.layer==e.layer&&a.phase==e.phase&&a.module==e.module&&a.module_ordinal==e.module_ordinal,"argument scope/ordinal drift");
      need(a.api==e.api&&a.code==e.code&&a.code_kind==e.code_kind&&a.layout==e.layout&&a.sizes==e.sizes,"argument code/ABI drift");
      need(a.grid==e.grid&&a.block==e.block&&a.dynamic_shared==e.dynamic_shared&&a.static_shared==e.static_shared&&
           a.registers==e.registers&&a.local_bytes==e.local_bytes&&a.attrs==e.attrs,"argument geometry/resource/attribute drift");
      need(params&&!extra,"argument unsupported packed or ambiguous transport");
      need(!selected_ids_.count(a.native_launch_id)&&(!pid_||a.native_launch_id>last_id_),"argument native launch order/duplicate");
      uint64_t sum=0;for(auto n:a.sizes)sum=add(sum,n,limits_.max_launch_bytes);
      add(args_,a.sizes.size(),limits_.arguments);add(bytes_,sum,limits_.raw_bytes);
      // Validate every host buffer before the first copy; never follow its pointer payload.
      for(size_t i=0;i<a.sizes.size();++i)need(params[i]!=nullptr,"argument null host parameter buffer");
      r.selected=true;r.sequence=selected_ids_.size();r.launch=a;r.raw.reserve(a.sizes.size());
      for(size_t i=0;i<a.sizes.size();++i)r.raw.emplace_back(static_cast<const char*>(params[i]),size_t(a.sizes[i]));
      ++epochs_[a.epoch];++module_ordinals_[{a.epoch,a.call_id}];
      args_+=a.sizes.size();bytes_+=sum;selected_ids_.insert(a.native_launch_id);pending_.insert(a.native_launch_id);
      pid_=a.pid;ticks_=a.start_ticks;context_=a.context_id;stream_=a.stream;last_id_=a.native_launch_id;
      return r;
    }catch(...){failed_=true;throw;}
  }
  bool complete(uint64_t id,bool success) {
    try {
      need(!failed_,"argument ledger already failed");
      if(!selected_ids_.count(id))return false;
      need(pending_.count(id)==1,"argument duplicate native return");
      need(success,"argument native return failed");pending_.erase(id);++returned_;return true;
    }catch(...){failed_=true;throw;}
  }
  bool closed() const {
    return !failed_&&entries()==limits_.launches&&returned_==limits_.launches&&pending_.empty()&&
      args_==limits_.arguments&&bytes_==limits_.raw_bytes&&epochs_==expected_epochs_;
  }
  uint64_t entries()const{return selected_ids_.size();}
  uint64_t returns()const{return returned_;}
  uint64_t arguments()const{return args_;}
  uint64_t raw_bytes()const{return bytes_;}
  const Limits& limits()const{return limits_;}
};
} // namespace sgargs
