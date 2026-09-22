#pragma once
#include "runner.h"
#include "../fast-prefill-r2/native_prefill.h"
#include "contracts.h"
#include "gate_source.h"
#include "o_prefill.h"
#include "down_source.h"
#include "qkv_source.h"
namespace native_prefill_sweep {
using namespace source_cache;
inline const J& contracts(){static const J c=J::parse(prefill_sweep_contracts);return c;}
template<class Sink> void generate_p64(const J& command,const Sink& sink){
 need(command.at("type")=="qwen_prefill_sweep_program_v1","Prefill sweep command type");
 const auto variant=command.at("variant").get<std::string>();need(contracts().contains(variant),"unsupported finite Prefill sweep variant");
 const auto& c=contracts().at(variant);const auto& b=command.at("binding");need(command.at("source_contract")==c,"finite source contract differs");
 need(b.at("schema")==c.at("schema")&&b.at("code_sha256")==c.at("code"),"current binding schema/code differs");
 need(b.at("grid")==c.at("grid")&&b.at("block")==c.at("block"),"current finite geometry differs");
 const bool og=variant=="P64_o"||variant=="P64_gate",o=variant=="P64_o",gate=variant=="P64_gate",down=variant=="P64_down",qkv=variant=="P64_qkv";
 need(og||down||qkv,"unsupported finite source algorithm");
 J shape=og?J({b.at("M"),b.at("N"),b.at("K")}):b.at("shape");need(shape==c.at("shape"),"finite shape differs");need(b.at("global_address_program_complete_for_admitted_ABI")==true,"complete finite source program required");
 U M=number(shape[0]),N=number(shape[1]),K=number(shape[2]),parts=number(c.at("parts"));
 if(og)need(b.at("parts")==parts&&b.at("role")==c.at("role"),"finite role/partitions");
 if(down)need(b.at("splitK_partitions")==parts,"Down four independent scratch partitions");
 U X=number(b.at(og?"X":"activation"),1),W=number(b.at(og?"W":"weights"),1),Y=number(b.at(og?"Y":"output"),1),bias=qkv?number(b.at("bias"),1):0;
 std::vector<std::pair<U,U>> roots={{X,M*K*2},{W,N*K*2},{Y,parts*M*N*2}};if(qkv)roots.push_back({bias,N*2});
 for(auto p:roots)need(p.first%8==0&&p.first<UINT64_MAX-p.second-256,"root alignment/extent");
 if(og)need(number(b.at("C"),1)==Y,"beta zero source/output alias");
 if(down)need(number(b.at("final_output_pointer"),1)%8==0,"final output alignment");
 auto ctas=native_prefill::cta_order(command,product(c.at("grid")));
 const Policy px{!o,false,"activation"},pw{!o,false,"weights"},pb{false,false,"bias"},py{false,!o,down?"splitK_partial":"output"};
 for(U id:ctas)for(U warp=0;warp<4;++warp){
  Effect e;e.cta=id;e.warp=warp;e.effective=e.global_mask=UINT32_MAX;
  if(o){
   U cx=id%16,cy=(id/16)%6,n=cy*8+(cx&7),m=cx>>3;e.operation="READ";e.width=16;
   for(U t=0;t<12;++t){std::array<U,4> ap=t?std::array<U,4>{0x1260,0x1290,0x1320,0x14f0}:std::array<U,4>{0x7b0,0x7d0,0x800,0x870};std::array<U,4> wp=t?std::array<U,4>{0x1550,0x1590,0x15d0,0x1740}:std::array<U,4>{0x950,0x970,0x990,0x9b0};
    for(U which=0;which<2;++which)for(U j=0;j<4;++j){e.pc=which?wp[j]:ap[j];for(U lane=0;lane<32;++lane)e.addresses[lane]=(which?W:X)+2*(((which?n:m)*32+(warp*32+lane)/16+j*8)*K+t*128+((warp*32+lane)%16)*8);sink(e,which?pw:px);}}
   e.operation="WRITE";e.width=8;for(U j=0;j<2;++j){e.pc=j?0x3160:0x3150;for(U lane=0;lane<32;++lane)e.addresses[lane]=Y+2*((m*32+8*warp+lane/8+j*4)*N+n*32+4*(lane%8));sink(e,py);}
  }else if(gate){
   e.operation="GLOBAL_TO_SHARED";e.width=16;
   for(U t=0;t<48;++t){std::array<U,6> pcs=t<5?std::array<U,6>{0xba0,0xbb0,0xbc0,0xbd0,0xbf0,0xc00}:t==5?std::array<U,6>{0x13a0,0x13b0,0x13c0,0x1450,0x14d0,0x1550}:std::array<U,6>{0x1840,0x1850,0x18c0,0x1450,0x14d0,0x1550};
    for(U j=0;j<6;++j){bool weight=j<4;U extra=(weight?j:j-4)*32;e.pc=pcs[j];for(U lane=0;lane<32;++lane)e.addresses[lane]=(weight?W:X)+2*((weight?id*128:0)+(warp*32+lane)/4+extra)*K+2*(t*32+((warp*32+lane)%4)*8);sink(e,weight?pw:px);}}
   e.operation="WRITE";e.width=8;std::array<U,4> pcs={0x2f60,0x2f90,0x2fd0,0x2fe0};for(U call=0;call<4;++call)for(U j=0;j<4;++j){e.pc=pcs[j];for(U lane=0;lane<32;++lane)e.addresses[lane]=Y+2*((32*(warp/2)+lane/16+call*8+j*2)*N+id*128+(lane%16)*4+(warp%2)*64);sink(e,py);}
  }else if(down){
   U n=id%12,part=id/12;e.operation="GLOBAL_TO_SHARED";e.width=16;
   for(U t=0;t<35;++t){std::array<U,12> pcs=t<2?std::array<U,12>{0xe70,0xe80,0xe90,0xea0,0xec0,0xed0,0xee0,0xef0,0xf10,0xf20,0xf30,0xf40}:t==2?std::array<U,12>{0x1780,0x1790,0x17a0,0x1830,0x18c0,0x1930,0x19a0,0x1a10,0x1aa0,0x1b10,0x1b60,0x1bf0}:std::array<U,12>{0x2090,0x20a0,0x20b0,0x1830,0x18c0,0x1930,0x19a0,0x1a10,0x1aa0,0x1b10,0x1b60,0x1bf0};
    for(U j=0;j<12;++j){bool weight=j<8;U extra=(weight?j:j-8)*16;e.pc=pcs[j];for(U lane=0;lane<32;++lane)e.addresses[lane]=(weight?W:X)+2*(((weight?128*n:0)+(warp*32+lane)/8+extra)*K+2240*part+64*t+8*((warp*32+lane)%8));sink(e,weight?pw:px);}}
   e.operation="WRITE";e.width=8;std::array<U,4> pcs={0x3730,0x3760,0x37a0,0x37b0};for(U j=0;j<16;++j){e.pc=pcs[j%4];for(U lane=0;lane<32;++lane)e.addresses[lane]=Y+2*((32*(warp/2)+lane/16+8*(j/4)+2*(j%4))*N+128*n+64*(warp%2)+4*(lane%16)+part*64*N);sink(e,py);}
  }else{
   e.operation="GLOBAL_TO_SHARED";e.width=16;
   for(U t=0;t<24;++t){std::array<U,8> pcs=t<4?std::array<U,8>{0xc80,0xc90,0xca0,0xcb0,0xcd0,0xce0,0xcf0,0xd00}:t==4?std::array<U,8>{0x1390,0x13a0,0x1410,0x1480,0x14d0,0x1530,0x1590,0x15d0}:std::array<U,8>{0x1950,0x1960,0x1410,0x1480,0x14d0,0x1530,0x1590,0x15d0};
    for(U j=0;j<8;++j){bool weight=j<4;e.pc=pcs[j];for(U lane=0;lane<32;++lane)e.addresses[lane]=(weight?W:X)+2*(((weight?64*id:0)+(32*warp+lane)/8+16*(j%4))*K+64*t+8*((32*warp+lane)%8));sink(e,weight?pw:px);}}
   e.operation="READ";e.width=4;for(U j=0;j<2;++j){e.pc=j?0x2400:0x23b0;for(U lane=0;lane<32;++lane)e.addresses[lane]=bias+2*(64*id+32*(warp%2)+4*(lane%8))+4*j;sink(e,pb);}
   e.operation="WRITE";e.width=8;for(U call=0;call<4;++call)for(U j=0;j<2;++j){e.pc=j?0x2d70:0x2d60;for(U lane=0;lane<32;++lane)e.addresses[lane]=Y+2*((32*(warp/2)+lane/8+8*call+4*j)*N+64*id+32*(warp%2)+4*(lane%8));sink(e,py);}
  }
 }
}
// One command format for separately qualified finite source programs. The cache
// implementation is shared; source instructions remain distinct for each code.
inline J binding_shape(const J& b,const J& c){
 if(c.contains("shape_fields")){J shape=J::array();for(const auto& f:c.at("shape_fields"))shape.push_back(b.at(f.get<std::string>()));return shape;}
 return b.at("shape");
}
template<class Sink> void generate(const J& command,const Sink& sink){
 need(command.at("type")=="qwen_prefill_sweep_program_v1","Prefill sweep command type");
 const auto variant=command.at("variant").get<std::string>();need(contracts().contains(variant),"unsupported finite Prefill sweep variant");
 const auto& c=contracts().at(variant);const auto& b=command.at("binding");
 need(command.at("source_contract")==c,"finite source contract differs");
 if(variant.rfind("P64_",0)==0){generate_p64(command,sink);return;}
 need(b.at("schema")==c.at("schema")&&b.at("code_sha256")==c.at("code"),"current binding schema/code differs");
 need(binding_shape(b,c)==c.at("shape")&&b.at("grid")==c.at("grid")&&b.at("block")==c.at("block"),"current finite shape/geometry differs");
 if(c.contains("binding_requirements"))for(auto it=c.at("binding_requirements").begin();it!=c.at("binding_requirements").end();++it)need(b.contains(it.key())&&b.at(it.key())==it.value(),"finite source binding requirement");
 auto ctas=native_prefill::cta_order(command,product(c.at("grid")));
 if(variant=="P256_gate"||variant=="P512_gate")native_prefill_sweep_gate::generate(b,ctas,sink);
 else if(variant=="P256_o"||variant=="P512_o")qwen_o_prefill::generate(b,ctas,sink);
 else if(variant=="P256_down"||variant=="P512_down")native_prefill_sweep_down::generate(b,ctas,sink);
 else if(variant=="P256_qkv"||variant=="P512_qkv"){
  need(command.at("requires_explicit_execution_schedule")==true&&command.at("schedule")==c.at("schedule")&&command.at("hardware_poll_multiplicity_known")==false,"explicit functional serial schedule required");
  need(ctas.size()==product(c.at("grid")),"complete serial CTA schedule required");
  for(U i=0;i<ctas.size();++i)need(ctas[i]==i,"ascending serial CTA schedule required");
  J expected=c.at("initialization_static");
  for(const char* key:{"process","native_launch_id","argument_record_sha256","semaphore"})expected[key]=b.at(key);
  need(command.at("initialization")==expected,"current source-driven serial initialization differs");
  native_prefill_sweep_qkv_split::generate(b,ctas,sink);
 }
 else need(false,"unsupported finite source algorithm");
}
inline void execute(Runner& runner,const J& command){runner.validate_program_binding(command.at("binding"));generate(command,[&](const Effect&e,const Policy&p){runner.consume_effect(e,p);});}
}
