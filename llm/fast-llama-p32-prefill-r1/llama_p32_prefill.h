#pragma once
// Exact global Effect projection of four pinned finite Python source programs.
// CTA/warp serialization is a caller model, not measured GPU issue order.
#include "runner.h"
#include "llama_p32_prefill_contracts.h"

namespace llama_p32_prefill {
using namespace source_cache;
inline const J& contracts(){static const J c=J::parse(llama_p32_prefill_contracts);return c;}
inline std::vector<U> words(const J& a){std::vector<U> r;for(const auto& x:a)r.push_back(number(x));return r;}
inline std::vector<U> cta_order(const J& command,U total){
 std::vector<U> out;
 if(command.contains("ctas")){
  need(!command.contains("cta_range")&&command.at("ctas").is_array(),"choose CTA list or range");
  for(const auto& x:command.at("ctas"))out.push_back(number(x,0,total-1));
  std::set<U> unique(out.begin(),out.end());need(unique.size()==out.size(),"duplicate Prefill CTA");
 }else{
  const auto& r=command.at("cta_range");need(r.is_array()&&r.size()==2,"CTA half-open range");
  U lo=number(r[0],0,total),hi=number(r[1],0,total);need(lo<=hi,"reversed CTA range");
  for(U c=lo;c<hi;++c)out.push_back(c);
 }
 need(!out.empty(),"empty Prefill program");return out;
}
template<class Sink> void generate(const J& command,const Sink& sink){
 need(command.at("type")=="llama_p32_prefill_program_v1","finite Prefill command type");
 const auto& b=command.at("binding");const auto role=b.at("role").get<std::string>();
 need(contracts().contains(role),"unsupported finite Prefill role");const auto& c=contracts().at(role);
 need(command.at("source_contract")==c,"complete finite SASS contract changed");
 need(b.at("schema")=="CURRENT_LLAMA_P32_PREFILL_SOURCE_BINDING_V1"&&b.at("code_sha256")==c.at("code"),"source binding schema/code changed");
 need(J({b.at("M"),b.at("N"),b.at("K")})==J({32,c.at("N"),c.at("K")})&&b.at("grid")==c.at("grid")&&b.at("block")==c.at("block"),"finite matrix/dispatch shape");
 need(b.at("global_address_program_complete_for_admitted_ABI")==true&&b.at("current_dynamic_lane_sample")==false&&b.at("actual_SM_or_warp_schedule_claimed")==false,"source qualification changed");
 const U M=32,N=number(b.at("N")),K=number(b.at("K")),W=number(b.at("W"),1),X=number(b.at("X"),1),Y=number(b.at("Y"),1);
 need(b.at("C")==b.at("Y"),"qualified C/D alias");
 // Masked-off addresses are fingerprinted but never dereferenced. The source
 // program carries zero-fill X rows up to63 while only rows0..31 are active.
 for(auto [base,extent]:std::array<std::pair<U,U>,3>{{{W,N*K*2},{X,64*K*2},{Y,M*N*2}}})
  need(base%8==0&&base<UINT64_MAX-extent,"source root alignment/address arithmetic");
 U tk=number(c.at("tk")),stages=number(c.at("stages")),wg=tk/8,xg=tk/16,step=1024/tk;
 auto pro=words(c.at("prologue_pcs")),first=words(c.at("first_main_pcs")),recur=words(c.at("recurrent_pcs")),stores=words(c.at("store_pcs"));
 const Policy pw{true,false,"weights"},px{true,false,"activation"},py{false,true,"output"};
 for(U cta:cta_order(command,product(c.at("grid"))))for(U warp=0;warp<4;++warp){
  Effect e;e.cta=cta;e.warp=warp;e.effective=UINT32_MAX;e.operation="GLOBAL_TO_SHARED";e.width=16;
  for(U tile=0;tile<K/tk;++tile){const auto& pcs=tile<stages-1?pro:tile==stages-1?first:recur;
   for(U j=0;j<pcs.size();++j){bool weight=j<wg;U group=weight?j:j-wg;e.global_mask=weight||group<xg/2?UINT32_MAX:0;
    if(tile>=stages-1&&e.global_mask==0)continue; // predicate-false, no instruction
    e.pc=pcs[j];for(U lane=0;lane<32;++lane){U tid=32*warp+lane,row=(weight?cta*128:0)+tid/(tk/8)+step*group,col=tk*tile+8*(tid%(tk/8));e.addresses[lane]=(weight?W:X)+2*(row*K+col);}
    sink(e,weight?pw:px); // executed zero-fill remains an Effect with global_mask0
   }
  }
  if(warp<2){e.operation="WRITE";e.width=8;e.global_mask=UINT32_MAX;
   for(U call=0;call<4;++call)for(U j=0;j<stores.size();++j){e.pc=stores[j];for(U lane=0;lane<32;++lane)e.addresses[lane]=Y+2*((lane/16+call*8+j*2)*N+cta*128+(lane%16)*4+(warp%2)*64);sink(e,py);}
  }
 }
}
inline void execute(Runner& runner,const J& command){runner.validate_program_binding(command.at("binding"));generate(command,[&](const Effect& e,const Policy& p){runner.consume_effect(e,p);});}
}
