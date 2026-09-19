// Standalone CPU tests: independent buffers, boundaries, and refusal semantics.
#include "argument_capture.h"
#include <functional>
#include <iostream>
#include <limits>
#include <cstring>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
using namespace sgargs;
static int checks=0;
void check(bool ok,const char* what){++checks;if(!ok)throw std::runtime_error(what);}
template<class F> void rejects(F f){bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}check(rejected,"expected rejection");}
string hash(const string& s){unsigned char digest[32];
#ifdef __APPLE__
  CC_SHA256(s.data(),CC_LONG(s.size()),digest);
#else
  SHA256(reinterpret_cast<const unsigned char*>(s.data()),s.size(),digest);
#endif
  return hex(string(reinterpret_cast<char*>(digest),32));
}
struct Fixture {
  std::vector<Entry> p;
  Limits l;
  uint32_t scalar=0x12345678;
  uint64_t pointer_bits=0xdeadbeef00112233ull;
  char bytes[3]={'a','\0','b'};
  void* params[3]={&scalar,&pointer_bits,bytes};
  Fixture(){
    Entry e;e.epoch=1;e.phase="Prefill";e.module="model.layers.0.self_attn";e.layer=0;
    e.code=string(64,'a');e.layout=hash("[4,8,3]");e.code_kind="sha256_nvbit_decoded_instruction_rows_v1";
    e.api="cuLaunchKernel";e.attrs="[]";e.grid={{2,1,1}};e.block={{32,4,1}};
    e.static_shared=16;e.dynamic_shared=32;e.registers=40;e.sizes={4,8,3};
    p.push_back(e);e.ordinal=1;e.module_ordinal=1;p.push_back(e);
    e.epoch=2;e.ordinal=0;e.module_ordinal=0;e.phase="Decode1";e.api="cuLaunchKernelEx";p.push_back(e);
    l.launches=3;l.arguments=9;l.raw_bytes=45;l.max_arguments=3;l.max_argument_bytes=8;l.max_launch_bytes=15;
    l.max_row_bytes=4096;l.max_file_bytes=3*4096;
  }
  Actual actual(size_t i) const {
    Actual a;static_cast<Entry&>(a)=p.at(i);a.pid=900;a.start_ticks=1234;a.context_id=7;a.function_id=8;
    a.call_id=i<2?30:99;a.native_launch_id=100+i;a.forward=int64_t(a.epoch-1);a.scope_bound=true;a.role="measurement";
    return a;
  }
};
void reject_actual(const std::function<void(Actual&)>& change) {
  Fixture f;Ledger g(f.p,f.l);auto a=f.actual(0);change(a);
  rejects([&]{g.before(a,f.params,nullptr);});check(!g.closed(),"failed ledger not closed");
}
void reject_plan(const std::function<void(Fixture&)>& change) {
  Fixture f;change(f);rejects([&]{Ledger g(f.p,f.l);});
}
int main(){try{
  Fixture f;Ledger g(f.p,f.l);
  auto first=g.before(f.actual(0),f.params,nullptr);
  check(first.raw.size()==3&&first.raw[0]==string(reinterpret_cast<char*>(&f.scalar),4)&&
        first.raw[1]==string(reinterpret_cast<char*>(&f.pointer_bits),8)&&first.raw[2]==string("a\0b",3),"independent actual byte vectors");
  const string original=serialize(first,hash);
  check(f.scalar==0x12345678&&f.pointer_bits==0xdeadbeef00112233ull&&string(f.bytes,3)==string("a\0b",3),"capture does not write host buffers");
  f.scalar=0;f.pointer_bits=0;std::memset(f.bytes,'z',3);
  check(serialize(first,hash)==original,"late host-buffer reuse cannot alter captured record");
  check(original.find("\"parameter_buffer_offset\":null")!=string::npos&&original.find("\"raw_bytes_hex\":\"610062\"")!=string::npos,"nonpacked serialization and embedded NUL");
  check(original.find("\"forward_id\":0")!=string::npos&&original.find("\"registers\":40")!=string::npos,"scope and resources serialized");
  check(g.complete(100,true),"first paired return");
  for(size_t i=1;i<3;++i){auto r=g.before(f.actual(i),f.params,nullptr);check(r.sequence==i,"strict sequence");check(g.complete(100+i,true),"paired return");}
  check(g.closed()&&g.entries()==3&&g.returns()==3&&g.arguments()==9&&g.raw_bytes()==45,"exact global/per-epoch/argument census");
  check(!g.complete(77,true),"unselected warmup return ignored");
  rejects([&]{g.complete(102,true);});check(!g.closed(),"duplicate return poisons finish");
  Fixture warm;Ledger warm_g(warm.p,warm.l);Actual w;w.role="warmup";
  check(!warm_g.before(w,nullptr,nullptr).selected&&!warm_g.closed(),"unmarked warmup neither copied nor admitted");

  reject_actual([](Actual&a){a.epoch=0;});
  reject_actual([](Actual&a){a.epoch=2;});
  reject_actual([](Actual&a){a.forward=1;});
  reject_actual([](Actual&a){a.role="warmup";});
  reject_actual([](Actual&a){a.scope_bound=false;});
  reject_actual([](Actual&a){a.pid=0;});
  reject_actual([](Actual&a){a.function_id=0;});
  reject_actual([](Actual&a){a.call_id=0;});
  reject_actual([](Actual&a){a.phase="Decode1";});
  reject_actual([](Actual&a){a.module="other";});
  reject_actual([](Actual&a){a.layer=1;});
  reject_actual([](Actual&a){a.api="cuGraphLaunch";});
  reject_actual([](Actual&a){a.code[0]='b';});
  reject_actual([](Actual&a){a.code_kind="cubin_sha256";});
  reject_actual([](Actual&a){a.layout[0]='0';});
  reject_actual([](Actual&a){a.sizes[0]=8;});
  reject_actual([](Actual&a){a.grid[0]=1;});
  reject_actual([](Actual&a){a.block[0]=16;});
  reject_actual([](Actual&a){a.dynamic_shared=16;});
  reject_actual([](Actual&a){a.static_shared=0;});
  reject_actual([](Actual&a){a.registers=41;});
  reject_actual([](Actual&a){a.local_bytes=1;});
  reject_actual([](Actual&a){a.attrs="[{}]";});

  for(int mode=0;mode<3;++mode){Fixture x;Ledger ledger(x.p,x.l);void* extra=nullptr;
    if(mode==2)x.params[1]=nullptr;
    rejects([&]{ledger.before(x.actual(0),mode==0?nullptr:x.params,mode==1?&extra:nullptr);});
    check(!ledger.closed(),"unsupported transport remains unqualified");
  }
  for(int mode=0;mode<6;++mode){Fixture x;Ledger ledger(x.p,x.l);ledger.before(x.actual(0),x.params,nullptr);ledger.complete(100,true);
    auto a=x.actual(1);if(mode==0)++a.pid;if(mode==1)++a.start_ticks;if(mode==2)++a.context_id;
    if(mode==3)++a.stream;if(mode==4)a.native_launch_id=100;if(mode==5)++a.call_id;
    rejects([&]{ledger.before(a,x.params,nullptr);});check(!ledger.closed(),"current binding/ordinal drift refused");
  }
  {Fixture x;Ledger ledger(x.p,x.l);for(size_t i=0;i<3;++i){auto a=x.actual(i);a.pid=9999;a.start_ticks=8888;a.context_id=333;
      a.function_id+=100;a.call_id+=1000;a.native_launch_id+=2000;ledger.before(a,x.params,nullptr);ledger.complete(a.native_launch_id,true);}
    check(ledger.closed(),"fresh identities rebound consistently, not compared with old process");}
  {Fixture x;Ledger ledger(x.p,x.l);ledger.before(x.actual(0),x.params,nullptr);rejects([&]{ledger.complete(100,false);});check(!ledger.closed(),"failed launch cannot close");}
  {Fixture x;Ledger ledger(x.p,x.l);for(size_t i=0;i<3;++i){ledger.before(x.actual(i),x.params,nullptr);if(i)ledger.complete(100+i,true);}check(!ledger.closed(),"missing return rejected at finish");}
  {Fixture x;Ledger ledger(x.p,x.l);for(size_t i=0;i<3;++i){ledger.before(x.actual(i),x.params,nullptr);ledger.complete(100+i,true);}rejects([&]{ledger.before(x.actual(2),x.params,nullptr);});}

  reject_plan([](Fixture&x){x.l.launches=0;});
  reject_plan([](Fixture&x){x.l.arguments=8;});
  reject_plan([](Fixture&x){x.l.raw_bytes=44;});
  reject_plan([](Fixture&x){x.l.max_launch_bytes=14;});
  reject_plan([](Fixture&x){x.l.max_argument_bytes=7;});
  reject_plan([](Fixture&x){x.l.max_arguments=2;});
  reject_plan([](Fixture&x){x.l.max_row_bytes=(1ull<<20)+1;});
  reject_plan([](Fixture&x){x.l.max_file_bytes=(256ull<<20)+1;});
  reject_plan([](Fixture&x){x.p[0].sizes[1]=UINT64_MAX;});
  reject_plan([](Fixture&x){x.p[1].ordinal=0;});
  reject_plan([](Fixture&x){x.p[2].epoch=3;});
  reject_plan([](Fixture&x){x.p[0].api="cuGraphLaunch";});
  reject_plan([](Fixture&x){x.p[0].grid[1]=0;});
  reject_plan([](Fixture&x){x.p[0].sizes[0]=0;});
  reject_plan([](Fixture&x){x.p[0].code_kind="unknown";});
  check_output_budget(f.l,f.l.max_file_bytes-f.l.max_row_bytes,f.l.max_row_bytes);++checks;
  rejects([&]{check_output_budget(f.l,f.l.max_file_bytes-f.l.max_row_bytes+1,f.l.max_row_bytes);});
  rejects([&]{check_output_budget(f.l,0,f.l.max_row_bytes+1);});
  rejects([&]{check_output_budget(f.l,UINT64_MAX,1);});
  rejects([&]{add(UINT64_MAX,1,UINT64_MAX);});
  std::cout<<"{\"status\":\"PASS_HOST_ARGUMENT_CAPTURE\",\"checks\":"<<checks<<",\"GPU_executed\":false}\n";
  return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
