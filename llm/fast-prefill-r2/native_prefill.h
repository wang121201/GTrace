#pragma once
// Exact global Effect projection of seven frozen finite Python source programs.
// CTA order is supplied by the caller. This does not reconstruct GPU issue order.
#include "runner.h"
#include "native_prefill_contracts.h"

namespace native_prefill {
using namespace source_cache;
inline const J& contracts(){static const J c=J::parse(native_prefill_contracts);return c;}
inline std::vector<U> cta_order(const J& command,U total){
 std::vector<U> out;
 if(command.contains("ctas")){
  need(!command.contains("cta_range")&&command.at("ctas").is_array(),"explicit CTA list or range");
  for(const auto& x:command.at("ctas"))out.push_back(number(x,0,total-1));
  std::set<U> unique(out.begin(),out.end());need(unique.size()==out.size(),"duplicate Prefill CTA");
 }else{
  const auto& r=command.at("cta_range");need(r.is_array()&&r.size()==2,"CTA half-open range");
  U lo=number(r[0],0,total),hi=number(r[1],0,total);need(lo<=hi,"reversed CTA range");
  for(U c=lo;c<hi;++c)out.push_back(c);
 }
 need(!out.empty(),"empty Prefill program");return out;
}
inline std::vector<U> words(const J& a){std::vector<U> r;for(const auto& x:a)r.push_back(number(x));return r;}
struct Load {U pc,extra;bool weight;};
inline std::vector<Load> qkv_loads(const J& a){
 std::vector<Load> r;for(const auto& x:a){std::string role=x.at(1).get<std::string>();need(role=="activation"||role=="weights","qualified load role");r.push_back({number(x.at(0)),number(x.at(2)),role=="weights"});}return r;
}
template<class Sink> void generate(const J& command,const Sink& sink){
 need(command.at("type")=="native_prefill_program_v1","finite Prefill command type");
 const auto variant=command.at("variant").get<std::string>();need(contracts().contains(variant),"unsupported finite Prefill variant");
 const auto& c=contracts().at(variant);const auto& b=command.at("binding");
 need(command.at("source_contract")==c,"complete finite source/SASS contract changed");
 need(b.at("schema")==c.at("schema")&&b.at("code_sha256")==c.at("code"),"source binding schema/code changed");
 need(b.at("grid")==c.at("grid")&&b.at("block")==c.at("block"),"finite Prefill launch geometry");
 const bool llama=c.at("model")=="llama";const std::string family=c.at("family").get<std::string>();
 J shape=llama?J({b.at("M"),b.at("N"),b.at("K")}):b.at("shape");need(shape==c.at("shape"),"finite Prefill matrix shape");
 U M=number(shape[0]),N=number(shape[1]),K=number(shape[2]),parts=number(c.at("grid")[2]);
 if(llama){need(b.at("family")==family&&b.at("source_global_program_complete")==true&&b.at("qualification")=="SOURCE_SASS_ONLY_NOT_DYNAMICALLY_SAMPLED","Llama source qualification");}
 else{need(b.at("global_address_program_complete_for_admitted_ABI")==true,"Qwen source qualification");if(family!="qkv")need(number(b.at("splitK_partitions"))==parts,"Qwen split partition count");}
 U X=number(b.at("activation"),1),W=number(b.at("weights"),1),Y=number(b.at("output"),1);
 std::vector<std::pair<U,U>> roots={{X,M*K*2},{W,N*K*2},{Y,parts*M*N*2}};
 U bias=0;if(!llama&&family=="qkv"){bias=number(b.at("bias"),1);roots.push_back({bias,N*2});}
 // Gate's final all-predicated-off LDG carries addresses at K+64. Preserve them
 // in the digest without dereferencing; allow 256 bytes arithmetic headroom.
 for(auto [base,extent]:roots)need(base%(llama?2:16)==0&&base<UINT64_MAX-extent-256,"native root alignment/arithmetic extent");
 if(!llama){auto sorted=roots;std::sort(sorted.begin(),sorted.end());for(size_t i=1;i<sorted.size();++i)need(sorted[i-1].first+sorted[i-1].second<=sorted[i].first,"Qwen operand overlap");}
 if(family!="qkv"||llama){U final=number(b.at("final_output_pointer"),1);need(final%(llama?2:16)==0&&final<UINT64_MAX-M*N*2,"final output extent");if((llama&&family!="o")||(!llama&&family!="down"))need(final==Y,"direct final output alias");}
 auto ctas=cta_order(command,product(c.at("grid")));
 const bool bypass=llama||family!="gate";
 const Policy px{bypass,false,"activation"},pw{bypass,false,"weights"},pb{false,false,"bias"};
 const Policy py{false,llama||family!="qkv",(!llama&&family=="down")||(llama&&family=="o")?"splitK_partial":"output"};
 if(!llama&&family=="qkv"){
  std::vector<std::vector<Load>> pro;for(const auto& row:c.at("prologue"))pro.push_back(qkv_loads(row));auto loop=qkv_loads(c.at("mainloop"));
  std::vector<std::pair<U,U>> stores;for(const auto& x:c.at("stores"))stores.push_back({number(x[0]),number(x[1])});
  for(U id:ctas){U cx=id%16,cy=id/16,m=cx/8,n=8*cy+cx%8;
   for(U warp=0;warp<4;++warp){Effect e;e.cta=id;e.warp=warp;e.effective=e.global_mask=UINT32_MAX;e.operation="GLOBAL_TO_SHARED";e.width=16;
    for(U t=0;t<48;++t)for(const auto& l:t<5?pro[t]:loop){e.pc=l.pc;for(U lane=0;lane<32;++lane){U row=warp*(l.weight?32:16)+lane/4+l.extra;U logical=(l.weight?128*n:64*m)+row;e.addresses[lane]=(l.weight?W:X)+2*(K*logical+32*t+8*(lane%4));}sink(e,l.weight?pw:px);}
    e.operation="READ";e.pc=0x8170;for(U lane=0;lane<32;++lane)e.addresses[lane]=bias+2*(128*n+8*(lane%16));sink(e,pb);
    e.operation="WRITE";for(auto [pc,off]:stores){e.pc=pc;for(U lane=0;lane<32;++lane)e.addresses[lane]=Y+2*(N*(64*m+32*(warp/2)+4*(warp%2)+lane/16+off)+128*n+8*(lane%16));sink(e,py);}
   }
  }return;
 }
 const auto& cfg=c.at("cfg");U gx=number(c.at("grid")[0]),gy=number(c.at("grid")[1]);
 std::vector<std::vector<U>> pcs;std::vector<std::pair<bool,U>> load_roles;
 if(llama){for(const auto& row:cfg.at("load_pcs"))pcs.push_back(words(row));}
 else{for(const auto& key:{"prologue","first","loop"})pcs.push_back(words(cfg.at(key)));for(const auto& r:cfg.at("load_roles"))load_roles.push_back({r[0]=="weights",number(r[1])});}
 auto stores=words(cfg.at(llama?"store_pcs":"stores"));
 U threads=number(c.at("block")[0]);U prologue=number(cfg.at(llama?"prologue":"prologue_tiles"));
 U tk=llama?number(cfg.at("tk")):64,wgroups=llama?number(cfg.at("W_groups")):0,tn=llama?number(cfg.at("tn")):0;
 U partition_k=llama?number(cfg.at("partition_k")):K/parts;
 for(U id:ctas){
  U cx=id%gx,cy=(id/gx)%gy,part=id/(gx*gy),n=cx,m=cy;
  if(!llama&&family=="gate"){n=(cx+gx*cy)/2;m=(cx+gx*cy)%2;}
  if(!llama&&family=="o")n=cy%2?gx-1-cx:cx;
  U kbase=part*partition_k,klength=std::min(partition_k,K-kbase);
  U tiles=llama?klength/tk:number(cfg.at("tiles"));need(!llama||klength%tk==0,"unqualified partial K transfer");
  for(U warp=0;warp<threads/32;++warp){
   Effect e;e.cta=id;e.warp=warp;e.effective=e.global_mask=UINT32_MAX;e.operation=(!llama&&family=="gate")?"READ":"GLOBAL_TO_SHARED";e.width=16;
   for(U t=0;t<tiles;++t){const auto& opcs=pcs[t<prologue?0:t==prologue?1:2];e.global_mask=(!llama&&family=="gate"&&t==24)?0:UINT32_MAX;
    for(U j=0;j<opcs.size();++j){bool weight=llama?j<wgroups:load_roles[j].first;U extra=llama?(threads/(tk/8))*(weight?j:j-wgroups):load_roles[j].second;e.pc=opcs[j];
     for(U lane=0;lane<32;++lane){U tid=32*warp+lane,row=tid/(tk/8)+extra;U base_row=weight?(llama?tn*n:(family=="gate"?128*n:64*n)):((llama||family=="down")?128*m:64*m);e.addresses[lane]=(weight?W:X)+2*((base_row+row)*K+kbase+tk*t+8*(tid%(tk/8)));}
     sink(e,weight?pw:px);
    }
   }
   e.operation="WRITE";e.width=8;e.global_mask=UINT32_MAX;U store_events=llama?(family=="gate"?16:32):number(cfg.at("store_events"));
   for(U j=0;j<store_events;++j){e.pc=stores[j%stores.size()];for(U lane=0;lane<32;++lane){U row,col;
     if(llama){if(family=="gate"){row=128*m+64*(warp/2)+lane/8+4*j;col=64*n+32*(warp%2)+4*(lane%8);}else{U group=family=="o"?4:2;row=128*m+64*(warp/group)+lane/16+2*j;col=tn*n+64*(warp%group)+4*(lane%16);}}
     else if(family=="gate"){row=64*m+warp/2+2*(lane/16)+4*j;col=128*n+64*(warp%2)+4*(lane%16);}
     else{row=(family=="down"?128*m+64*(warp/2):64*m+32*(warp/2))+lane/8+4*j;col=64*n+32*(warp%2)+4*(lane%8);}
     U scratch_offset=((!llama&&family=="down")||(llama&&family=="o"))?part*128*N:0;e.addresses[lane]=Y+2*(row*N+col+scratch_offset);
    }sink(e,py);
   }
  }
 }
}
inline void execute(Runner& runner,const J& command){runner.validate_program_binding(command.at("binding"));generate(command,[&](const Effect& e,const Policy& p){runner.consume_effect(e,p);});}
}
