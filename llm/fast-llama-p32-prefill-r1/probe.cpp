#include "llama_p32_prefill.h"
#include <iostream>
#include <fstream>
#include <map>
#include <string>
using namespace source_cache;
struct Digest {
 U events=0, read=0, write=0, zero=0, hash=14695981039346656037ULL, policy_hash=14695981039346656037ULL;
 static void word(U& h,U value){for(unsigned i=0;i<8;++i){h^=(value>>(8*i))&255;h*=1099511628211ULL;}}
 void consume(const Effect& e,const Policy& p){
  U operation=e.operation=="READ"?0:e.operation=="WRITE"?1:e.operation=="ATOMIC_RMW"?2:e.operation=="GLOBAL_TO_SHARED"?3:99;
  need(operation!=99,"unknown probe operation");
  for(U x:{operation,e.cta,e.warp,e.pc,e.width,e.effective,e.global_mask})word(hash,x);
  for(U x:e.addresses)word(hash,x);
  word(policy_hash,U(p.bypass));word(policy_hash,U(p.low_priority));word(policy_hash,p.semantic.size());
  for(unsigned char c:p.semantic){policy_hash^=c;policy_hash*=1099511628211ULL;}
  U bytes=U(__builtin_popcountll(e.global_mask))*e.width;
  ++events;if(!bytes)++zero;if(operation!=1)read+=bytes;if(operation==1||operation==2)write+=bytes;
 }
 J json()const{return {{"events",events},{"read_bytes",read},{"write_bytes",write},{"zero_global_events",zero},{"source_effect_projection_fnv1a64",hash},{"policy_projection_fnv1a64",policy_hash}};}
};
int main(int argc,char** argv){try{
 std::ofstream trace;if(argc==2){trace.open(argv[1],std::ios::binary);need(bool(trace),"probe trace open");}
 auto word=[&](U x){for(unsigned i=0;i<8;++i)trace.put(char((x>>(8*i))&255));};J command;std::cin>>command;Digest all;std::map<U,Digest> ctas;
 llama_p32_prefill::generate(command,[&](const Effect& e,const Policy& p){all.consume(e,p);ctas[e.cta].consume(e,p);if(trace.is_open()){U op=e.operation=="WRITE"?1:e.operation=="GLOBAL_TO_SHARED"?3:99;for(U x:{op,e.cta,e.warp,e.pc,e.width,e.effective,e.global_mask})word(x);for(U x:e.addresses)word(x);word(p.bypass);word(p.low_priority);word(p.semantic.size());trace.write(p.semantic.data(),p.semantic.size());need(bool(trace),"probe trace write");}});
 J per=J::object();for(const auto& [id,d]:ctas)per[std::to_string(id)]=d.json();
 std::cout<<J({{"aggregate",all.json()},{"per_CTA",per}}).dump()<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
