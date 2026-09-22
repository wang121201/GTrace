#pragma once
#include "qwen_gemv.h"
#include "qwen_p32_gemm_contracts.h"
namespace qwen_p32_gemm {
using namespace source_cache;
template<class Sink>void generate(const J& command,const Sink& sink){
 const auto& b=command.at("binding");auto role=b.at("role").get<std::string>();static const J contracts=J::parse(qwen_p32_gemm_contracts);
 need(contracts.contains(role),"finite P32 GEMM role");const auto& contract=contracts.at(role);
 need(command.at("static_contract")==contract,"complete static code/opcode/predicate signature differs");
 need(b.at("schema")=="CURRENT_P32_QWEN_GEMM_SOURCE_BINDING_V1"&&b.at("code_sha256")==contract.at("code"),"current finite GEMM binding/code");
 U M=number(b.at("M")),N=number(b.at("N")),K=number(b.at("K")),parts=number(b.at("parts"));
 bool gate=role=="gate_up_proj",qkv=role=="qkv_proj",down=role=="down_proj";
 need(M==32&&((qkv&&N==2048&&K==1536&&parts==1)||(role=="o_proj"&&N==1536&&K==1536&&parts==1)||(down&&N==1536&&K==8960&&parts==2)||(gate&&N==17920&&K==1536&&parts==1)),"unsupported P32 shape/partition");
 J grid=gate?J({140,1,1}):J({8,N/256,parts});need(b.at("grid")==grid&&b.at("block")==J({128,1,1}),"finite P32 geometry");
 U X=number(b.at("X"),1),W=number(b.at("W"),1),C=number(b.at("C"),1),Y=number(b.at("Y"),1);
 for(const auto& pair:std::array<std::pair<U,U>,4>{{{X,32*K*2},{W,N*K*2},{Y,32*N*parts*2},{C,qkv?N*2:32*N*2}}})need(pair.first%8==0&&pair.first<UINT64_MAX-pair.second,"GEMM root alignment/extent");
 if(gate)need(C==Y,"gate beta-zero C/D alias");
 const auto& policies=command.at("policies");auto px=qwen_gemv::policy(policies.at("activation"),"activation"),pw=qwen_gemv::policy(policies.at("weights"),"weights"),pb=qwen_gemv::policy(policies.at("bias"),"weights"),py=qwen_gemv::policy(policies.at("output"),"activation");
 U total=product(grid);std::vector<U> ctas;
 if(command.contains("ctas")){need(!command.contains("cta_range")&&command.at("ctas").is_array(),"explicit CTA list or range");for(const auto& x:command.at("ctas"))ctas.push_back(number(x,0,total-1));std::set<U> unique(ctas.begin(),ctas.end());need(unique.size()==ctas.size(),"duplicate selected GEMM CTA");}
 else{const auto& range=command.at("cta_range");need(range.is_array()&&range.size()==2,"CTA half-open range");U lo=number(range[0],0,total),hi=number(range[1],0,total);need(lo<=hi,"reversed CTA range");for(U c=lo;c<hi;++c)ctas.push_back(c);}
 need(!ctas.empty(),"empty GEMM program");
 for(U c:ctas)for(U warp=0;warp<4;++warp){
  Effect e;e.cta=c;e.warp=warp;e.effective=e.global_mask=UINT32_MAX;
  if(!gate){
   U gy=N/256,cx=c%8,cy=(c/8)%gy,part=c/(8*gy),tile_n=cy*8+(cx&7),tile_m=cx>>3,segment=K/parts;
   for(U kt=0;kt<segment/128;++kt){
    std::array<U,4> ap=kt==0?std::array<U,4>{0x7b0,0x7d0,0x800,0x870}:std::array<U,4>{0x1260,0x1290,0x1320,0x14f0};
    std::array<U,4> wp=kt==0?std::array<U,4>{0x950,0x970,0x990,0x9b0}:std::array<U,4>{0x1550,0x1590,0x15d0,0x1740};
    e.operation="READ";e.width=16;
    for(U which=0;which<2;++which)for(U j=0;j<4;++j){e.pc=which?wp[j]:ap[j];U base=which?W:X,tile=which?tile_n:tile_m;for(U lane=0;lane<32;++lane)e.addresses[lane]=base+2*((tile*32+(warp*32+lane)/16+j*8)*K+part*segment+kt*128+((warp*32+lane)%16)*8);sink(e,which?pw:px);}
   }
   if(qkv){e.operation="READ";e.width=8;for(U pc:{0x2b00ULL,0x2b50ULL}){e.pc=pc;for(U lane=0;lane<32;++lane)e.addresses[lane]=C+2*(tile_n*32+4*(lane%8));sink(e,pb);}}
   e.operation="WRITE";e.width=8;
   for(U j=0;j<2;++j){e.pc=qkv?(j?0x2ed0:0x2e10):(j?0x3160:0x3150);for(U lane=0;lane<32;++lane)e.addresses[lane]=Y+2*(part*32*N+(tile_m*32+8*warp+lane/8+j*4)*N+tile_n*32+4*(lane%8));sink(e,py);}
  }else{
   e.operation="GLOBAL_TO_SHARED";e.width=16;
   for(U kt=0;kt<48;++kt){
    std::array<U,6> pcs=kt<5?std::array<U,6>{0xba0,0xbb0,0xbc0,0xbd0,0xbf0,0xc00}:kt==5?std::array<U,6>{0x13a0,0x13b0,0x13c0,0x1450,0x14d0,0x1550}:std::array<U,6>{0x1840,0x1850,0x18c0,0x1450,0x14d0,0x1550};
    for(U j=0;j<6;++j){bool weight=j<4;U mask=(weight||j==4)?UINT32_MAX:0;if(kt>=5&&!mask)continue;e.pc=pcs[j];e.global_mask=mask;U extra=(weight?j:j-4)*32,base=weight?W:X,rowtile=weight?c*128:0;for(U lane=0;lane<32;++lane)e.addresses[lane]=base+2*((rowtile+(warp*32+lane)/4+extra)*K+kt*32+((warp*32+lane)%4)*8);sink(e,weight?pw:px);}
   }
   if(warp<2){e.operation="WRITE";e.width=8;e.global_mask=UINT32_MAX;const std::array<U,4> pcs={0x2f60,0x2f90,0x2fd0,0x2fe0};for(U call=0;call<4;++call)for(U j=0;j<4;++j){e.pc=pcs[j];for(U lane=0;lane<32;++lane)e.addresses[lane]=Y+2*((lane/16+call*8+j*2)*N+c*128+(lane%16)*4+(warp%2)*64);sink(e,py);}}
  }
 }
}
inline void execute(Runner& runner,const J& command){runner.validate_program_binding(command.at("binding"));generate(command,[&](const Effect& e,const Policy& p){runner.consume_effect(e,p);});}
}
