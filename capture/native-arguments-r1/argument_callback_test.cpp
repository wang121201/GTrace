// Actual launch_body is extracted verbatim by test_producer.py. Real pinned CUDA
// callback structs are used; runtime/CUDA queries are CPU stubs, never GPU calls.
#include "argument_plan.h"
// Same header mode set by pinned nvbit.h before generated_cuda_meta.h.
#define __CUDA_API_VERSION_INTERNAL
#include "generated_cuda_meta.h"
#include <algorithm>
#include <iostream>
#include <memory>
#include <map>
#include <set>
#include <cstring>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
using std::string;
using nvbit_api_cuda_t=int;
const int API_CUDA_cuLaunchKernel=1,API_CUDA_cuLaunchKernel_ptsz=2,API_CUDA_cuLaunchKernelEx=3,API_CUDA_cuLaunchKernelEx_ptsz=4;
const unsigned MAX_FUNCTIONS=4096;
void need(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
string quote(const string&s){return sgargs::quote(s);}
string number(uint64_t n){return std::to_string(n);}
string signed_number(int64_t n){return std::to_string(n);}
template<class T>uint64_t ptr(T p){return reinterpret_cast<uintptr_t>(p);}
string sha(const string&s){unsigned char digest[32];
#ifdef __APPLE__
  CC_SHA256(s.data(),CC_LONG(s.size()),digest);
#else
  SHA256(reinterpret_cast<const unsigned char*>(s.data()),s.size(),digest);
#endif
  return sgargs::hex(string(reinterpret_cast<char*>(digest),32));
}
string uarray(const std::vector<uint64_t>&v){string o="[";for(size_t i=0;i<v.size();++i){if(i)o+=',';o+=number(v[i]);}return o+"]";}
string dims(unsigned x,unsigned y,unsigned z){return uarray({x,y,z});}
uint64_t tid(){return 21;}
struct Internal {};
struct Function {uint64_t id=0,module=0,module_epoch=0;string name,code_hash;std::vector<uint64_t>args;};
struct Scope {bool bound=true;uint64_t call=0;int64_t forward=-1;int layer=-1;string phase,module,role="measurement";}scope;
struct File {uint64_t bytes=0;};
struct State {
  uint64_t active_epoch=0,owner=12345,ticks=98765;
  sgargs::Ledger argument_ledger;
  File argument_file;string last;
  std::set<uint64_t>launch_threads,streams,epoch_threads,epoch_streams;
  State(const std::vector<sgargs::Entry>&p,sgargs::Limits l):argument_ledger(p,l){}
  uint64_t context(CUcontext){return 77;}
  void emit(File&f,const string&r){f.bytes+=r.size();last=r;}
};
std::unique_ptr<State>current_state;
State&state(){return *current_state;}
Function current_function;
sgargs::Entry current_entry;
uint64_t instrumentation_checks=0;
bool fail_resource=false;
Function describe(CUcontext,CUfunction){return current_function;}
std::vector<CUfunction> nvbit_get_related_functions(CUcontext,CUfunction){return {};}
const char*nvbit_get_func_name(CUcontext,CUfunction){return "CPU_mock_kernel";}
string bounded(const char*p){return p;}
void nvbit_enable_instrumented(CUcontext,CUfunction,bool enabled){need(!enabled,"must execute original kernel");++instrumentation_checks;}
string attributes(const CUlaunchConfig&){return current_entry.attrs;}
extern "C" CUresult CUDAAPI cuFuncGetAttribute(int*out,CUfunction_attribute which,CUfunction){
  if(fail_resource)return CUDA_ERROR_INVALID_VALUE;
  switch(which){case CU_FUNC_ATTRIBUTE_NUM_REGS:*out=int(current_entry.registers);break;
    case CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES:*out=int(current_entry.static_shared);break;
    case CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES:*out=int(current_entry.local_bytes);break;
    default:*out=89;}
  return CUDA_SUCCESS;
}
#include "argument_runtime.inc"
#include "launch_body_under_test.inc"

sgargs::Limits limits_for(const std::vector<sgargs::Entry>&p){
  sgargs::Limits l;l.launches=p.size();l.max_row_bytes=16384;l.max_file_bytes=p.size()*l.max_row_bytes;
  for(const auto&e:p){l.arguments+=e.sizes.size();uint64_t sum=0;l.max_arguments=std::max<uint64_t>(l.max_arguments,e.sizes.size());
    for(auto n:e.sizes){sum+=n;l.max_argument_bytes=std::max(l.max_argument_bytes,n);}l.raw_bytes+=sum;l.max_launch_bytes=std::max(l.max_launch_bytes,sum);}
  return l;
}
void execute(const std::vector<sgargs::Entry>&plan,sgargs::Limits limits,const string&mode,bool output){
  current_state.reset(new State(plan,limits));auto&s=state();uint64_t call_counter=300,max_row=0;
  std::map<std::pair<uint64_t,string>,uint64_t>calls;
  for(size_t i=0;i<plan.size();++i){const auto&e=plan[i];current_entry=e;s.active_epoch=e.epoch;
    const auto key=std::make_pair(e.epoch,e.module);if(e.module_ordinal==0)calls[key]=++call_counter;
    scope.call=calls.at(key);scope.forward=int64_t(e.epoch-1);scope.layer=e.layer;scope.phase=e.phase;scope.module=e.module;
    current_function.id=i+1;current_function.code_hash=e.code;current_function.args=e.sizes;current_function.name="CPU_mock_kernel";
    std::vector<string> storage;std::vector<void*>params;
    for(size_t n=0;n<e.sizes.size();++n){string b(size_t(e.sizes[n]),'\0');for(size_t j=0;j<b.size();++j)b[j]=char((i*7+n*13+j*17)&255);storage.push_back(b);}
    for(auto&v:storage)params.push_back(&v[0]);const auto expected=storage;
    CUfunction f=reinterpret_cast<CUfunction>(uintptr_t(99));CUstream stream=nullptr;
    cuLaunchKernel_params d{f,unsigned(e.grid[0]),unsigned(e.grid[1]),unsigned(e.grid[2]),unsigned(e.block[0]),unsigned(e.block[1]),unsigned(e.block[2]),unsigned(e.dynamic_shared),stream,params.data(),nullptr};
    cuLaunchKernel_ptsz_params dp{f,d.gridDimX,d.gridDimY,d.gridDimZ,d.blockDimX,d.blockDimY,d.blockDimZ,d.sharedMemBytes,stream,params.data(),nullptr};
    CUlaunchConfig config{};config.gridDimX=d.gridDimX;config.gridDimY=d.gridDimY;config.gridDimZ=d.gridDimZ;
    config.blockDimX=d.blockDimX;config.blockDimY=d.blockDimY;config.blockDimZ=d.blockDimZ;config.sharedMemBytes=d.sharedMemBytes;config.hStream=stream;
    cuLaunchKernelEx_params ex{&config,f,params.data(),nullptr};cuLaunchKernelEx_ptsz_params exp{&config,f,params.data(),nullptr};
    void*extra=nullptr;int cbid=0;void*callback=nullptr;
    if(e.api=="cuLaunchKernel"){cbid=API_CUDA_cuLaunchKernel;callback=&d;}
    else if(e.api=="cuLaunchKernel_ptsz"){cbid=API_CUDA_cuLaunchKernel_ptsz;callback=&dp;}
    else if(e.api=="cuLaunchKernelEx"){cbid=API_CUDA_cuLaunchKernelEx;callback=&ex;}
    else if(e.api=="cuLaunchKernelEx_ptsz"){cbid=API_CUDA_cuLaunchKernelEx_ptsz;callback=&exp;}
    if(mode=="packed"){d.extra=dp.extra=ex.extra=exp.extra=&extra;}
    if(mode=="null_function"){d.f=dp.f=ex.f=exp.f=nullptr;}
    if(mode=="null_config"){ex.config=exp.config=nullptr;}
    if(mode=="bad_api")cbid=99;
    if(mode=="resource_failure")fail_resource=true;
    if(mode=="journal_cap")s.argument_file.bytes=limits.max_file_bytes;
    const string body=launch_body(reinterpret_cast<CUcontext>(uintptr_t(3)),cbid,callback,50000+i,e.api);
    need(storage==expected,"host parameter buffers must remain unchanged");
    const string captured=s.last;for(auto&v:storage)std::fill(v.begin(),v.end(),char(0xa5));
    need(s.last==captured&&captured.back()=='\n',"captured row survives later storage reuse");
    const string payload=captured.substr(0,captured.size()-1);
    need(body.find("\"payload_sha256\":"+quote(sha(payload)))!=string::npos,"body refers to actual emitted argument row");
    // Return callback uses Pending::body; the source-level runner checks that
    // producer code reuses p.body rather than calling launch_body again.
    const string return_body=body;need(return_body==body,"one reference for before and return");
    need(s.argument_ledger.complete(50000+i,true),"actual ledger return");
    max_row=std::max<uint64_t>(max_row,captured.size());
    if(output)std::cout<<"{\"argument\":"<<payload<<",\"body\":{"<<body.substr(1)<<"}}\n";
  }
  need(s.argument_ledger.closed(),"actual callback plan/counts closed");
  std::cout<<"{\"status\":\"PASS_ACTUAL_ARGUMENT_CALLBACK\",\"launches\":"<<s.argument_ledger.entries()
    <<",\"arguments\":"<<s.argument_ledger.arguments()<<",\"raw_bytes\":"<<s.argument_ledger.raw_bytes()
    <<",\"serialized_bytes\":"<<s.argument_file.bytes<<",\"max_row_bytes\":"<<max_row<<",\"GPU_executed\":false}\n";
}
int main(int argc,char**argv){if(argc!=2)return 2;const string mode=argv[1];
  bool negative=mode!="full"&&mode!="four-api";
  try{
    if(mode=="full")execute(sgargs::make_plan(),sgargs::make_limits(),mode,true);
    else {auto e=sgargs::make_plan().front();e.epoch=1;e.ordinal=e.module_ordinal=0;
      if(mode=="four-api")for(const char*api:{"cuLaunchKernel","cuLaunchKernel_ptsz","cuLaunchKernelEx","cuLaunchKernelEx_ptsz"}){e.api=api;execute({e},limits_for({e}),mode,false);}
      else {if(mode=="null_config")e.api="cuLaunchKernelEx";execute({e},limits_for({e}),mode,false);}}
    if(negative){std::cerr<<"negative fixture unexpectedly passed\n";return 3;}
    return 0;
  }catch(const std::exception&e){if(!negative){std::cerr<<e.what()<<"\n";return 4;}
    std::cout<<"{\"status\":\"EXPECTED_CALLBACK_REJECTION\",\"reason\":"<<quote(e.what())<<"}\n";return 0;}
}
