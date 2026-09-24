#pragma once
#include "runner.h"
#include "qwen_gemv_stages.h"
namespace native_gemv {
using namespace source_cache;
struct Step {U pc,index;bool weights;};
inline Policy policy(const J& j,const std::string& semantic){Policy p;p.bypass=boolean(j.at("bypass_l1"));auto s=j.at("l2_priority").get<std::string>();need(s=="normal"||s=="evict_first","GEMV explicit priority");p.low_priority=s=="evict_first";p.semantic=j.value("semantic",semantic);return p;}
template<class Sink>void generate(const J& command,const Sink& sink){
 const auto& b=command.at("binding");const auto code=b.at("code_sha256").get<std::string>();static const J contract=J::parse(qwen_gemv_stage_contract);need(contract.contains(code),"unsupported Qwen GEMV code");const auto& c=contract.at(code);need(command.at("stages")==c.at("stages")&&command.at("epilog")==c.at("epilog"),"derived full-SASS stage/epilog sequence differs from frozen source");
 const auto schema=b.at("schema").get<std::string>();bool llama=schema=="CURRENT_LLAMA_GEMV_SOURCE_BINDING_V1";need(llama||schema=="CURRENT_QWEN_GEMV_SOURCE_BINDING_V1","finite native GEMV binding schema");
 U variant=number(b.at("variant"),6,7),tx=number(b.at("thread_x")),K=number(b.at("K")),N=number(b.at("N")),lda=number(b.at("lda")),ty=llama?number(b.at("rows_per_cta")):4;need(lda==K,"GEMV contiguous lda");
 need(code==(variant==6?"a67f8d265deb5e1402037e2c45c1a28c7dc5c343e97e493dd24f0727b4ac50ed":"31af276ca5f2a53c3d67f65622dcee3ec8662093c79ce0dd6fed10ce5acd00d5"),"native code variant");
 bool qshape=!llama&&ty==4&&((variant==6&&tx==16&&K==1536&&(N==1536||N==2048||N==17920||N==151936))||(variant==7&&tx==32&&K==8960&&N==1536));
 bool lshape=llama&&((variant==6&&tx==16&&ty==4&&K==4096&&(N==6144||N==4096))||(variant==7&&tx==32&&ty==4&&((K==4096&&N==28672)||(K==14336&&N==4096)))||(variant==7&&tx==16&&ty==8&&K==4096&&N==128256));
 need(qshape||lshape,"finite native GEMV shape/code/thread specialization");need(b.at("grid")==J({N/ty,1,1})&&b.at("block")==J({tx,ty,1}),"native geometry");U W=number(b.at("weights"),1),X=number(b.at("activation"),1),Y=number(b.at("output"),1),bias=number(b.at("bias")),flags=number(b.at("epilog_flags"));need((flags==1&&bias==0)||(flags==4&&!llama&&bias>0&&N==2048),"Qwen epilog bias contract");need(W%2==0&&X%2==0&&Y%2==0&&W<=UINT64_MAX-N*K*2&&X<=UINT64_MAX-K*2&&Y<=UINT64_MAX-N*2&&(!bias||(bias%2==0&&bias<=UINT64_MAX-N*2)),"GEMV global root extents");
 std::array<std::vector<Step>,4> stages;const std::array<U,4> widths={64,32,16,4};for(unsigned i=0;i<4;++i)for(const auto& row:c.at("stages").at(std::to_string(widths[i])))stages[i].push_back({number(row.at(0)),number(row.at(3)),row.at(2)=="weights"});
 const auto& policies=command.at("policies");auto pw=policy(policies.at("weights"),"weights"),px=policy(policies.at("activation"),"activation"),py=policy(policies.at("output"),"activation"),pb=policy(policies.at("bias"),"weights");
 std::vector<U> ctas;if(command.contains("ctas")){need(!command.contains("cta_range"),"choose ctalist or range");for(const auto& x:command.at("ctas"))ctas.push_back(number(x,0,N/ty-1));std::set<U> unique(ctas.begin(),ctas.end());need(unique.size()==ctas.size(),"duplicate selected CTA");}else{const auto& range=command.at("cta_range");need(range.is_array()&&range.size()==2,"explicit CTA half-open range");U lo=number(range[0],0,N/ty),hi=number(range[1],0,N/ty);need(lo<=hi,"CTA range reversed");for(U cta=lo;cta<hi;++cta)ctas.push_back(cta);}
 need(!ctas.empty(),"empty GEMV CTA program");
 for(U cta:ctas)for(U warp=0;warp<tx*ty/32;++warp){
  Effect e;e.cta=cta;e.warp=warp;e.width=2;e.operation="READ";e.effective=e.global_mask=UINT32_MAX;
  for(U outer=0;outer<K/tx;outer+=64){U remaining=std::min<U>(64,K/tx-outer),consumed=0;for(unsigned s=0;s<4;++s)while(remaining>=widths[s]){
    for(const auto& step:stages[s]){e.pc=step.pc;U column=outer+consumed+step.index;for(U lane=0;lane<32;++lane){U tid=warp*32+lane,output_row=cta*ty+tid/tx;e.addresses[lane]=(step.weights?W:X)+2*((step.weights?output_row*lda:0)+tid%tx+column*tx);}sink(e,step.weights?pw:px);}
    remaining-=widths[s];consumed+=widths[s];
   }need(remaining==0,"unimplemented scalar remainder");}
  e.addresses.fill(0);e.effective=e.global_mask=0;for(U lane=0;lane<32;++lane)if((warp*32+lane)%tx==0)e.effective|=U{1}<<lane;e.global_mask=e.effective;
  if(flags==4){e.operation="READ";e.pc=number(c.at("epilog")[0]);for(U lane=0;lane<32;++lane)if(e.global_mask&(U{1}<<lane))e.addresses[lane]=bias+2*(cta*ty+(warp*32+lane)/tx);sink(e,pb);}
  e.operation="WRITE";e.pc=number(c.at("epilog")[1]);for(U lane=0;lane<32;++lane)if(e.global_mask&(U{1}<<lane))e.addresses[lane]=Y+2*(cta*ty+(warp*32+lane)/tx);sink(e,py);
 }
}
inline void execute(Runner& runner,const J& command){runner.validate_program_binding(command.at("binding"));generate(command,[&](const Effect& e,const Policy& p){runner.consume_effect(e,p);});}
}
