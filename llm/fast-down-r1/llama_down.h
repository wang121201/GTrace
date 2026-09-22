#pragma once
#include "runner.h"
#include "work/tilegen-full-r1/driver-pooled-fusednorm-r1/sha256.h"

namespace llama_down {
using namespace source_cache;
inline constexpr const char* CODE="c6255f6e64997a866e3354880e6b9c9c9b9e8b5d19a83d46afe81434daae0f2c";
inline constexpr const char* STATIC="cc52eb98a52cfb08b901451b341eb4d740605872f4f21e0af4fb41dd7bdbd6f8";
inline std::string digest(const J& x){return tiny_sha::sha256(x.dump(-1,' ',true));}
inline void initialization(const J& b,const J& i){
 need(i.at("schema")=="CURRENT_SERIAL_SPLITK_RESET_BINDING_V1"&&i.at("process")==b.at("process")&&i.at("native_launch_id")==b.at("native_launch_id")&&i.at("argument_record_sha256")==b.at("argument_record_sha256"),"Down initialization identity");
 need(i.at("semaphore_base")==b.at("semaphore")&&i.at("initial_uint32_values")==J(std::vector<U>(16,0)),"actual Down reset values");
 const auto& n=i.at("memory_API_node");const auto& k=i.at("kernel_node");need(digest(n)==i.at("memory_API_node_sha256").get<std::string>(),"reset node SHA");
 need(n.at("kind")=="memory_api_submission"&&k.at("native_launch_id")==b.at("native_launch_id"),"Down native node identity");
 for(auto key:{"process","phase","stream_u64"})need(n.at(key)==k.at(key)&&k.at(key)==b.at(key),"same process phase stream reset");
 need(n.at("context_handle_u64")==k.at("context_handle_u64"),"same reset context");bool predecessor=false;for(const auto& p:k.at("predecessors"))predecessor|=p.at("node")==n.at("id")&&p.at("kind")=="same_context_CUstream_submission_order";
 need(predecessor&&number(n.at("return_event"))<number(k.at("submission_event")),"reset immediate stream predecessor/order");
 auto descriptor=[&](const J& o){need(o.at("action")=="memset"&&o.at("cuda_api")=="cuMemsetD8Async"&&number(o.at("requested_bytes"))==64&&number(o.at("fill_value_u32"))==0&&number(o.at("fill_element_bytes"))==1&&o.at("destination").at("address_u64")==b.at("semaphore"),"exact actual zero64 reset");};descriptor(n.at("operation"));
 for(bool returned:{false,true}){const auto& r=i.at(returned?"raw_API_return":"raw_API_before");need(r.at("schema")=="sg_nvbit_observer_event_v1"&&r.at("type")=="memory_api"&&r.at("edge")== (returned?"return":"before"),"reset raw schema/edge");descriptor(r);
  need(r.at("pid")==b.at("process").at("pid")&&r.at("start_ticks")==b.at("process").at("start_ticks")&&r.at("memory_operation_id")==n.at("native_memory_operation_id"),"reset raw process/operation");
  need(r.at("stream_u64")==b.at("stream_u64")&&r.at("context_handle_u64")==k.at("context_handle_u64")&&r.at("phase")==b.at("phase"),"reset raw stream/context/phase");
  need(r.at("device_memory_dereferenced")==false&&r.at("geometry")=="linear"&&number(r.at("width_bytes"))==64&&number(r.at("height"))==1&&number(r.at("depth"))==1,"reset geometry");
  need(r.at("event_ordinal")==n.at(returned?"return_event":"submission_event")&&(!returned||number(r.at("cuda_status"))==0),"reset event/status");
 }
}
struct Action {unsigned kind,cta,extra;};
inline std::vector<Action> schedule(const J& command){
 const auto& input=command.at("execution_schedule");need(input.is_array()&&!input.empty(),"explicit Down schedule required");
 struct State{bool begun=false,published=false;std::array<bool,8> ready{};unsigned next=0;};std::array<State,144> states{};std::array<unsigned,16> sem{};std::set<U> expected;
 for(const auto& x:command.at("required_ctas"))need(expected.insert(number(x,0,143)).second,"duplicate required CTA");need(!expected.empty(),"explicit required Down CTA domain");
 std::vector<Action> out;out.reserve(input.size());
 for(const auto& a:input){U c=number(a.at("cta"),0,143);need(expected.count(c),"Down action outside required CTA domain");auto& s=states[c];unsigned n=c%16,part=c/16;auto kind=a.at("action").get<std::string>();
  if(kind=="body"){need(!s.begun,"duplicate Down body");s.begun=true;s.ready.fill(part==0);out.push_back({0,unsigned(c),0});}
  else if(kind=="poll"){unsigned w=number(a.at("warp"),0,7);need(s.begun&&!s.published&&!s.ready[w]&&s.next==0,"poll before body/after native loop");s.ready[w]=(sem[n]==part);out.push_back({1,unsigned(c),w});}
  else if(kind=="output_group"){unsigned g=number(a.at("group"),0,7);need(s.begun&&!s.published&&std::all_of(s.ready.begin(),s.ready.end(),[](bool v){return v;})&&g==s.next,"Down barrier/output order");++s.next;out.push_back({2,unsigned(c),g});}
  else if(kind=="publish"){need(s.begun&&!s.published&&s.next==8&&sem[n]==part,"Down publication state");sem[n]=part+1;s.published=true;out.push_back({3,unsigned(c),0});}
  else throw std::invalid_argument("unknown Down schedule action");
 }
 for(U c:expected)need(states[c].begun&&states[c].published,"incomplete required Down CTA");return out;
}
template<class Sink>void generate(const J& command,const Sink& sink){
 const auto& b=command.at("binding");need(command.at("type")=="llama_down_program"&&command.at("static_sha256")==STATIC,"Down command/code qualification");
 need(b.at("schema")=="CURRENT_LLAMA_SERIAL_SPLITK_BINDING_V1"&&b.at("code_sha256")==CODE&&b.at("grid")==J({16,1,9})&&b.at("block")==J({256,1,1}),"Down finite schema/code/geometry");
 need(number(b.at("M"))==128&&number(b.at("N"))==4096&&number(b.at("K"))==14336&&number(b.at("split_count"))==9&&number(b.at("partition_k"))==1600,"Down finite shape");
 initialization(b,command.at("initialization"));auto actions=schedule(command);U W=number(b.at("weights"),1),X=number(b.at("activation"),1),Y=number(b.at("output"),1),S=number(b.at("semaphore"),1);
 for(auto pair:std::array<std::pair<U,U>,4>{{{W,4096ULL*14336*2},{X,128ULL*14336*2},{Y,128ULL*4096*2},{S,64}}})need(pair.first%4==0&&pair.first<UINT64_MAX-pair.second+1,"Down finite root extent");
 // Exact source-opcode cache interpretation, including explicit modeled EL->normal.
 need(command.at("EL_policy")=="SOURCE_EL_MODELED_NORMAL_NOT_HARDWARE_EVICT_LAST","explicit EL interpretation");
 const Policy pw{true,false,"weights"},px{true,false,"activation"},pr{false,false,"prior_split_output"},py{false,true,"output"},pp{false,false,"semaphore_poll"},ps{false,true,"semaphore_publish"};
 const U loads[3][6]={{0xba0,0xbb0,0xbc0,0xbd0,0xbf0,0xc00},{0x1590,0x15a0,0x15b0,0x1660,0x1760,0x1850},{0x1b00,0x1be0,0x1cc0,0x1660,0x1760,0x1850}};
 const U stores[4]={0x3970,0x39a0,0x39e0,0x39f0},initial[4]={0x2740,0x2770,0x27b0,0x27c0},next[4]={0x3810,0x3840,0x3880,0x3890};
 auto output=[&](unsigned c,unsigned w,unsigned group,unsigned kind){Effect e;e.cta=c;e.warp=w;e.width=8;e.effective=e.global_mask=UINT32_MAX;e.operation=kind==2?"WRITE":"READ";unsigned which=group+(kind==1);for(unsigned j=0;j<4;++j){e.pc=(kind==0?initial:kind==1?next:stores)[j];for(U l=0;l<32;++l)e.addresses[l]=Y+2*((64*(w/4)+l/16+8*which+2*j)*4096+256*(c%16)+64*(w%4)+4*(l%16));sink(e,kind==2?py:pr);}};
 for(auto a:actions){unsigned c=a.cta,n=c%16,part=c/16;Effect e;e.cta=c;e.effective=e.global_mask=UINT32_MAX;
  if(a.kind==0){e.operation="GLOBAL_TO_SHARED";e.width=16;U base=1600*part,tiles=std::min<U>(1600,14336-base)/32;for(U t=0;t<tiles;++t)for(U w=0;w<8;++w)for(U j=0;j<6;++j){bool weights=j<4;U q=weights?j:j-4;e.warp=w;e.pc=loads[t<2?0:t==2?1:2][j];for(U l=0;l<32;++l){U tid=32*w+l,row=tid/4+64*q;e.addresses[l]=(weights?W:X)+2*((weights?256*n+row:row)*14336+base+32*t+8*(tid%4));}sink(e,weights?pw:px);}}
  else if(a.kind==1){e.operation="READ";e.width=4;e.pc=0x2670;e.warp=a.extra;e.addresses.fill(S+4*n);sink(e,pp);}
  else if(a.kind==2){if(part>0&&a.extra==0)for(unsigned w=0;w<8;++w)output(c,w,0,0);for(unsigned w=0;w<8;++w){if(part>0&&a.extra<7)output(c,w,a.extra,1);output(c,w,a.extra,2);}}
  else{e.operation="WRITE";e.width=4;e.pc=0x3370;e.warp=0;e.effective=e.global_mask=1;e.addresses.fill(0);e.addresses[0]=S+4*n;sink(e,ps);}
 }
}
inline void execute(Runner& runner,const J& command){runner.validate_program_binding(command.at("binding"));generate(command,[&](const Effect& e,const Policy& p){runner.consume_effect(e,p);});}
}
