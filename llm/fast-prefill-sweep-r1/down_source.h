#pragma once
#include "runner.h"
namespace native_prefill_sweep_down {
using namespace source_cache;
// Caller additionally checks the sealed shape/source contract and current ABI.
template<class Sink> void generate(const J& b,const std::vector<U>& ctas,const Sink& sink){
 need(b.at("schema")=="CURRENT_QWEN_P256_P512_DOWN_SOURCE_BINDING_V1"&&b.at("code_sha256")=="b179847b55b02ca8e36ba7d1656af997925ef52b8918e073c54ac44916166c01","finite Down source identity");
 U M=number(b.at("shape")[0]),N=number(b.at("shape")[1]),K=number(b.at("shape")[2]);
 need((M==256||M==512)&&N==1536&&K==8960,"finite Down shape");
 U parts=M==256?4:2;need(number(b.at("partitions"))==parts&&number(b.at("partition_elements"))==M*N,"parallel-split scratch layout");
 need(b.at("grid")==J({M/32,2,parts})&&b.at("block")==J({256,1,1}),"finite Down dispatch");
 need(b.at("global_address_program_complete_for_admitted_ABI")==true,"complete source binding required");
 U X=number(b.at("activation"),1),W=number(b.at("weights"),1),Y=number(b.at("output"),1);
 std::vector<std::pair<U,U>> roots={{X,M*K*2},{W,N*K*2},{Y,parts*M*N*2}};
 for(auto [a,s]:roots)need(a%16==0&&a<UINT64_MAX-s,"Down root alignment/extent");
 for(U i=0;i<roots.size();++i)for(U j=i+1;j<roots.size();++j)need(roots[i].first+roots[i].second<=roots[j].first||roots[j].first+roots[j].second<=roots[i].first,"Down operand overlap");
 const Policy px{true,false,"activation"},pw{true,false,"weights"},py{false,false,"output"};
 struct Op{U pc;bool weight;U row;};
 const std::array<Op,6> pro0={{{0xcd0,false,0},{0xd10,false,16},{0xe70,false,8},{0xfe0,true,0},{0x1100,true,8},{0x1130,false,24}}};
 const std::array<Op,6> pro1={{{0x1400,false,0},{0x1460,false,8},{0x14a0,false,16},{0x14f0,false,24},{0x1530,true,0},{0x1570,true,8}}};
 const std::array<Op,6> loop={{{0x1c00,false,8},{0x1c30,false,0},{0x1c70,false,16},{0x1cb0,false,24},{0x1d00,true,8},{0x1d30,true,0}}};
 const std::array<U,16> stores={0x8a00,0x8c60,0x90d0,0x9330,0x9780,0x99e0,0x9e30,0xa090,0xa4e0,0xa740,0xab90,0xadf0,0xb220,0xb4a0,0xb8e0,0xbab0};
 std::set<U> seen;
 for(U id:ctas){
  need(id<(M/32)*2*parts&&seen.insert(id).second,"explicit unique legal Down CTA order");
  U gx=M/32,cx=id%gx,cy=(id/gx)%2,part=id/(gx*2),m=cx>>3,n=cy*8+(cx&7);
  if(m>=M/256||n>=12)continue;
  for(U w=0;w<8;++w){
   Effect e;e.cta=id;e.warp=w;e.effective=e.global_mask=UINT32_MAX;e.width=16;e.operation="GLOBAL_TO_SHARED";
   for(U t=0;t<K/parts/32;++t)for(const auto& op:t==0?pro0:t==1?pro1:loop){
    e.pc=op.pc;
    for(U l=0;l<32;++l)e.addresses[l]=(op.weight?W:X)+2*(((op.weight?128*n:256*m)+w*(op.weight?16:32)+l/4+op.row)*K+part*(K/parts)+t*32+8*(l%4));
    sink(e,op.weight?pw:px);
   }
   e.operation="WRITE";
   for(U j=0;j<16;++j){
    e.pc=stores[j];
    for(U l=0;l<32;++l)e.addresses[l]=Y+2*(part*M*N+(256*m+64*(w/2)+4*(w%2)+l/16+8*(j/2)+2*(j%2))*N+128*n+8*(l%16));
    sink(e,py);
   }
  }
 }
}
}
