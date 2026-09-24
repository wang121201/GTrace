#pragma once
#include "runner.h"
namespace native_prefill_sweep_gate {
using namespace source_cache;
// Caller additionally checks the sealed shape/source contract and current ABI.
template<class Sink> void generate(const J& b,const std::vector<U>& ctas,const Sink& sink){
 need(b.at("schema")=="CURRENT_QWEN_P256_P512_GATE_SOURCE_BINDING_V1"&&b.at("code_sha256")=="052781bb02e0ca149af997b5c255334a0cb3daaae0c0d348963e8cad393667ed","finite Gate source identity");
 U M=number(b.at("shape")[0]),N=number(b.at("shape")[1]),K=number(b.at("shape")[2]);
 need((M==256||M==512)&&N==17920&&K==1536,"finite Gate shape");
 need(b.at("grid")==J({M/16,9,1})&&b.at("block")==J({256,1,1}),"finite Gate dispatch");
 need(b.at("global_address_program_complete_for_admitted_ABI")==true,"complete source binding required");
 U X=number(b.at("activation"),1),W=number(b.at("weights"),1),Y=number(b.at("output"),1);
 std::vector<std::pair<U,U>> roots={{X,M*K*2},{W,N*K*2},{Y,M*N*2}};
 for(auto [a,s]:roots)need(a%16==0&&a<UINT64_MAX-s,"Gate root alignment/extent");
 for(U i=0;i<roots.size();++i)for(U j=i+1;j<roots.size();++j)need(roots[i].first+roots[i].second<=roots[j].first||roots[j].first+roots[j].second<=roots[i].first,"Gate operand overlap");
 const Policy px{true,false,"activation"},pw{true,false,"weights"},py{false,false,"output"};
 struct Op{U pc;bool weight;U row;};
 const std::array<Op,6> pro0={{{0xd40,false,0},{0xf00,false,8},{0xff0,true,0},{0x1020,true,16},{0x1120,true,8},{0x1140,true,24}}};
 const std::array<Op,6> pro1={{{0x14a0,false,0},{0x14d0,false,8},{0x15a0,true,0},{0x15e0,true,8},{0x1620,true,16},{0x1660,true,24}}};
 const std::array<Op,6> loop={{{0x1bc0,false,0},{0x1c20,true,8},{0x1c60,true,16},{0x1ca0,true,24},{0x1cf0,true,0},{0x1d40,false,8}}};
 const std::array<U,16> stores={0x11970,0x11980,0x11f90,0x11fc0,0x12620,0x12660,0x12ca0,0x12ce0,0x13320,0x13360,0x139a0,0x139e0,0x14020,0x14060,0x144b0,0x145e0};
 std::set<U> seen;
 for(U id:ctas){
  need(id<(M/16)*9&&seen.insert(id).second,"explicit unique legal Gate CTA order");
  U cx=id%(M/16),cy=id/(M/16),m=cx>>3,n=cy*8+(cx&7);
  if(m>=M/128||n>=70)continue;
  for(U w=0;w<8;++w){
   Effect e;e.cta=id;e.warp=w;e.effective=e.global_mask=UINT32_MAX;e.width=16;e.operation="GLOBAL_TO_SHARED";
   for(U t=0;t<48;++t)for(const auto& op:t==0?pro0:t==1?pro1:loop){
    e.pc=op.pc;
    for(U l=0;l<32;++l)e.addresses[l]=(op.weight?W:X)+2*(((op.weight?256*n:128*m)+w*(op.weight?32:16)+l/4+op.row)*K+t*32+8*(l%4));
    sink(e,op.weight?pw:px);
   }
   e.operation="WRITE";
   for(U j=0;j<16;++j){
    e.pc=stores[j];
    for(U l=0;l<32;++l)e.addresses[l]=Y+2*((128*m+64*(w/4)+2*(w%4)+l/16+8*(j/2))*N+256*n+8*(l%16)+128*(j%2));
    sink(e,py);
   }
  }
 }
}
}
