#pragma once
#include "runner.h"
namespace native_prefill_sweep_qkv_split {
using namespace source_cache;
// Sparse CTA vectors are projections of the complete ascending CTA schedule.
// The caller must complete every predecessor split before a later split.
template<class Sink> void generate(const J& b,const std::vector<U>& ctas,const Sink& sink){
 need(b.at("schema")=="CURRENT_QWEN_P256_P512_QKV_SERIAL_SOURCE_BINDING_V1"&&b.at("code_sha256")=="e40d6335f3a21cb81835b78143b875f9daddce89681226c40356cc41b3b17883","finite QKV source");
 U M=number(b.at("shape")[0]),N=number(b.at("shape")[1]),K=number(b.at("shape")[2]);
 need((M==256||M==512)&&N==2048&&K==1536,"finite QKV shape");
 need(b.at("grid")==J({16,M/128,3})&&b.at("block")==J({128,1,1}),"finite QKV dispatch");
 need(b.at("global_address_program_complete_for_admitted_ABI")==true&&b.at("hardware_poll_multiplicity_known")==false&&b.at("schedule")=="cta_linear_ascending_split_k_no_wait","functional serial schedule contract");
 need(number(b.at("finite_swizzle_flags"))==(M==256?12:4620),"finite swizzle flags");
 U W=number(b.at("weights"),1),X=number(b.at("activation"),1),Y=number(b.at("output"),1),B=number(b.at("bias"),1),S=number(b.at("semaphore"),1),xy=16*(M/128);
 std::vector<std::pair<U,U>> roots={{W,N*K*2},{X,M*K*2},{Y,M*N*2},{B,N*2},{S,xy*4}};
 for(auto [a,n]:roots)need(a%4==0&&a<UINT64_MAX-n,"QKV roots");
 for(U i=0;i<roots.size();++i)for(U j=i+1;j<roots.size();++j)need(roots[i].first+roots[i].second<=roots[j].first||roots[j].first+roots[j].second<=roots[i].first,"QKV overlap");
 const Policy pw{false,false,"weights"},px{false,false,"activation"},pr{false,false,"output"},py{false,true,"output"},pb{false,false,"bias"},psr{false,false,"semaphore"},psw{false,true,"semaphore"};
 const std::array<U,8> p0={0x9b0,0x9e0,0xa10,0xa40,0xaf0,0xb20,0xb50,0xb80},p1={0x1220,0x1250,0x1280,0x12b0,0x1320,0x1350,0x1380,0x13b0},pn={0x1870,0x18d0,0x1930,0x1990,0x1a50,0x1b70,0x1bd0,0x1c30};
 const std::array<U,8> r0={0x29d0,0x2a00,0x2a40,0x2a70,0x2ab0,0x2ae0,0x2b20,0x2b30},rn={0x40a0,0x40d0,0x4110,0x4140,0x4180,0x41b0,0x41f0,0x4200},st={0x47a0,0x47d0,0x4810,0x4840,0x4880,0x48b0,0x48f0,0x4900};
 bool first=true;U previous=0;
 for(U c:ctas){
  need(c<xy*3&&(first||previous<c),"ascending unique legal QKV CTAs");first=false;previous=c;
  U split=c/xy,linear=c%xy,cx=M==256?linear%16:linear/4,cy=M==256?linear/16:linear%4;
  for(U w=0;w<4;++w){
   Effect e;e.cta=c;e.warp=w;e.effective=e.global_mask=UINT32_MAX;e.operation="READ";e.width=16;
   for(U tile=0;tile<18;++tile){e.global_mask=tile<16?UINT32_MAX:0;const auto& pcs=tile==0?p0:tile==1?p1:pn;
    for(U j=0;j<8;++j){bool weight=j<4;e.pc=pcs[j];
     for(U lane=0;lane<32;++lane){U thread=32*w+lane;U row=(weight?cx:cy)*128+thread/4+32*(j%4),col=split*512+tile*32+(thread%4)*8;e.addresses[lane]=(weight?W:X)+2*(row*K+col);}
     sink(e,weight?pw:px);
    }
   }
   e.global_mask=UINT32_MAX;
   if(split){e.pc=0x28a0;e.width=4;e.addresses.fill(S+4*(cy*16+cx));sink(e,psr);}
   auto output=[&](U row_delta){for(U lane=0;lane<32;++lane){U row=128*cy+64*(w/2)+lane/16+row_delta,col=128*cx+64*(w%2)+4*(lane%16);e.addresses[lane]=Y+2*(row*N+col);}};
   e.width=8;
   if(split)for(U j=0;j<8;++j){e.pc=r0[j];output(2*j);sink(e,pr);}
   if(split==2){e.width=4;for(U j=0;j<2;++j){e.pc=j?0x2c40:0x2bf0;for(U lane=0;lane<32;++lane)e.addresses[lane]=B+2*(128*cx+64*(w%2)+4*(lane%16))+4*j;sink(e,pb);}}
   e.width=8;
   for(U call=0;call<4;++call){
    if(split&&call<3){e.operation="READ";for(U j=0;j<8;++j){e.pc=rn[j];output(16*(call+1)+2*j);sink(e,pr);}}
    e.operation="WRITE";for(U j=0;j<8;++j){e.pc=st[j];output(16*call+2*j);sink(e,py);}
   }
  }
  Effect publish;publish.cta=c;publish.warp=0;publish.pc=0x3660;publish.width=4;publish.effective=publish.global_mask=1;publish.operation="WRITE";publish.addresses[0]=S+4*(cy*16+cx);sink(publish,psw);
 }
}
}
