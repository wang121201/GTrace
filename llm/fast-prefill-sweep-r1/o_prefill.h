#pragma once
#include "runner.h"
// Source projection only. CTA traversal is caller-declared, not observed GPU issue.
// Exact finite current ABI admission lives in command_builder.py/provider.py.
namespace qwen_o_prefill {
using namespace source_cache;
inline constexpr const char* schema="CURRENT_QWEN_P256_P512_O_SOURCE_BINDING_V1";
inline constexpr const char* code="520ea53d58df9aab50398fdf065e25a7092d9c33cd8a9d52abaeea5140383a89";
inline constexpr const char* static_sha="7ad95d8ce58e821f5a417149a825821b1456017d7d64534257a22b5dc3283d78";
inline constexpr U loads[3][16]={
 {0x1060,0x1070,0x1080,0x1090,0x10b0,0x10c0,0x10d0,0x10e0,0x1100,0x1110,0x1120,0x1130,0x1150,0x1160,0x1170,0x1180},
 {0x1c70,0x1c80,0x1c90,0x1ca0,0x1d90,0x1e30,0x1ee0,0x1f60,0x1ff0,0x20a0,0x2150,0x2200,0x2290,0x2320,0x23f0,0x24c0},
 {0x2970,0x2980,0x2a40,0x2ad0,0x1d90,0x1e30,0x1ee0,0x1f60,0x1ff0,0x20a0,0x2150,0x2200,0x2290,0x2320,0x23f0,0x24c0}};
inline constexpr U stores[4]={0x4760,0x4790,0x47d0,0x47e0};
inline std::vector<U> ctas(const J& c){
 need(c.contains("ctas")!=c.contains("cta_range"),"exactly one CTA traversal");std::vector<U> v;
 if(c.contains("ctas")){need(c.at("ctas").is_array(),"CTA array");for(auto& n:c.at("ctas"))v.push_back(number(n,0,47));}
 else{auto& r=c.at("cta_range");need(r.is_array()&&r.size()==2,"CTA half-open range");U a=number(r[0],0,48),b=number(r[1],0,48);need(a<b,"nonempty CTA range");for(U n=a;n<b;++n)v.push_back(n);}
 need(!v.empty()&&std::set<U>(v.begin(),v.end()).size()==v.size(),"nonempty unique CTAs");return v;
}
template<class Sink> inline void generate(const J& c,Sink&& sink){
 need(c.at("type")=="qwen_o_prefill_program_v1"&&c.at("source_static_sha256")==static_sha,"finite O command/static identity");auto& b=c.at("binding");
 need(b.at("schema")==schema&&b.at("code_sha256")==code,"finite O source identity");U P=number(b.at("rows"));need(P==256||P==512,"finite O shape");bool split=P==256;
 need(b.at("columns")==1536&&b.at("reduction_K")==1536&&b.at("partition_K")== (split?768:1536)&&b.at("partition_elements")==(split?393216:0),"finite O partition dimensions");
 need(b.at("grid")==J::array({12,split?2:4,split?2:1})&&b.at("block")==J::array({128,1,1}),"finite O grid/block");
 need(b.at("parallel_split")==split&&b.at("serial_split")==false&&b.at("cta_swizzle")=="SERPENTINE_N_BY_PHYSICAL_Y_WITH_EXPONENT_ZERO","finite O branch/swizzle");
 U W=number(b.at("weights"),1),X=number(b.at("activation"),1),Y=number(b.at("output"),1),F=number(b.at("final_output"),1),pk=split?768:1536,parts=split?2:1;
 need(split?Y!=F:Y==F,"O final/scratch identity");std::vector<std::pair<U,U>> ranges;
 auto extent=[&](U a,U n){need(a%16==0&&a<UINT64_MAX-n,"operand alignment/overflow");ranges.emplace_back(a,a+n);};
 extent(W,1536*1536*2);extent(X,P*1536*2);extent(Y,P*1536*2*parts);if(split)extent(F,P*1536*2);std::sort(ranges.begin(),ranges.end());for(size_t i=1;i<ranges.size();++i)need(ranges[i-1].second<=ranges[i].first,"operand overlap");
 Policy wp{true,false,"weights"},xp{true,false,"activation"},yp{false,true,split?"splitK_partial":"output"};
 Effect e;e.effective=e.global_mask=UINT32_MAX;
 for(U cta:ctas(c)){U x=cta%12,m=(cta/12)%(split?2:4),part=cta/(12*(split?2:4)),n=(m&1)?11-x:x;e.cta=cta;
  for(U warp=0;warp<4;++warp){e.warp=warp;e.operation="GLOBAL_TO_SHARED";e.width=16;
   for(U tile=0;tile<pk/64;++tile)for(U j=0;j<16;++j){bool w=j<8;U group=j%8;e.pc=loads[tile<2?0:tile==2?1:2][j];
    for(U lane=0;lane<32;++lane){U t=warp*32+lane,row=128*(w?n:m)+t/8+16*group,col=part*pk+64*tile+8*(t%8);e.addresses[lane]=(w?W:X)+2*(row*1536+col);}sink(e,w?wp:xp);
   }
   e.operation="WRITE";e.width=8;for(U s=0;s<32;++s){e.pc=stores[s%4];for(U lane=0;lane<32;++lane){U row=128*m+64*(warp/2)+lane/16+2*s,col=128*n+64*(warp%2)+4*(lane%16);e.addresses[lane]=Y+2*(row*1536+col+part*(split?393216:0));}sink(e,yp);}
  }
 }
}
// Optional integration API; preserves the same finite guard and Effect sequence.
template<class Sink> inline void generate(const J& binding,const std::vector<U>& order,Sink&& sink){
 J command={{"type","qwen_o_prefill_program_v1"},{"binding",binding},{"source_static_sha256",static_sha},{"ctas",order}};
 generate(command,std::forward<Sink>(sink));
}
inline void execute(Runner& runner,const J& c){runner.validate_program_binding(c.at("binding"));generate(c,[&](const Effect& e,const Policy& p){runner.consume_effect(e,p);});}
}
