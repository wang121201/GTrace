// Synthetic integration regression: sink's associated namespace changes ADL.
#include "../llm/fast-prefill-sweep-r1/native_prefill_sweep.h"
#include <iostream>
#include <sstream>

namespace prefill_o_adl_test {
using namespace source_cache;
struct Digest {
 U hash=1469598103934665603ULL,events=0,read=0,write=0;
 void byte(unsigned char x){hash^=x;hash*=1099511628211ULL;}
 void word(U x){for(unsigned i=0;i<8;++i)byte((x>>(8*i))&255);}
 void add(const Effect& e,const Policy& p){
  ++events;for(U x:{e.cta,e.warp,e.pc,e.width,e.effective,e.global_mask,
      U(e.operation=="READ"?1:e.operation=="WRITE"?2:3),U(p.bypass),U(p.low_priority)})word(x);
  for(auto x:p.semantic)byte(x);byte(0);for(U x:e.addresses)word(x);
  (e.operation=="WRITE"?write:read)+=std::popcount(e.global_mask)*e.width;
 }
 J json()const{return {{"events",events},{"fnv1a64",hash},{"source_read_bytes",read},{"source_write_bytes",write}};}
};
J command(U P,bool full=true){
 const std::string variant="P"+std::to_string(P)+"_o";
 const auto& c=native_prefill_sweep::contracts().at(variant);
 J b={{"schema",c.at("schema")},{"code_sha256",c.at("code")},
      {"rows",P},{"columns",1536},{"reduction_K",1536},
      {"partition_K",P==256?768:1536},{"partition_elements",P==256?393216:0},
      {"grid",c.at("grid")},{"block",c.at("block")},
      {"parallel_split",P==256},{"serial_split",false},
      {"cta_swizzle","SERPENTINE_N_BY_PHYSICAL_Y_WITH_EXPONENT_ZERO"},
      {"activation",U{1}<<32},{"weights",U{2}<<32},{"output",U{3}<<32},
      {"final_output",(P==256?U{4}:U{3})<<32},
      {"native_launch_id",1},{"phase","Synthetic/Prefill"}};
 for(auto i=c.at("binding_requirements").begin();i!=c.at("binding_requirements").end();++i)b[i.key()]=i.value();
 J out={{"type","qwen_prefill_sweep_program_v1"},{"variant",variant},{"binding",b},{"source_contract",c}};
 if(full)out["cta_range"]=J::array({0,48});else out["ctas"]=J::array({0,47});
 return out;
}
J direct_command(const J& c){
 J out={{"type","qwen_o_prefill_program_v1"},{"binding",c.at("binding")},
        {"source_static_sha256",qwen_o_prefill::static_sha}};
 for(const char* key:{"ctas","cta_range"})if(c.contains(key))out[key]=c.at(key);
 return out;
}
// Like the ordinary probe, this closure is outside native_prefill_sweep.
J ordinary_probe(const J& c){Digest d;native_prefill_sweep::generate(c,[&](const Effect&e,const Policy&p){d.add(e,p);});return d.json();}
J direct_probe(const J& c){Digest d;qwen_o_prefill::generate(direct_command(c),[&](const Effect&e,const Policy&p){d.add(e,p);});return d.json();}
std::vector<J> stream(const J& c){
 const auto& b=c.at("binding");return {
  {{"type","run_begin"},{"schema","TILEGEN_SOURCE_CACHE_STREAM_V1"},{"sm_policy","cta_mod_48"}},
  {{"type","begin_kernel"},{"id",1},{"phase",b.at("phase")},{"grid",b.at("grid")},
   {"block",b.at("block")},{"semantic","synthetic_O_ADL_regression"},{"observed_shared_bytes",32768}},
  c,{{"type","end_kernel"},{"id",1}},{{"type","run_end"}}};
}
J cache_run(const J& c,bool dispatcher){
 std::ostringstream output;Runner runner(output);auto commands=stream(c);
 for(size_t i=0;i<commands.size();++i){
  if(i==2){if(dispatcher)native_prefill_sweep::execute(runner,c);else qwen_o_prefill::execute(runner,direct_command(c));}
  else runner.command(commands[i]);
 }
 J result=runner.summary();result.erase("CPU_minutes");result.erase("wall_minutes");return result;
}
}

namespace native_prefill_sweep {
// Exact relevant closure namespace of execute(), absent from the old probe.
inline source_cache::J adl_namespace_probe(const source_cache::J& c){
 prefill_o_adl_test::Digest d;
 native_prefill_sweep::generate(c,[&](const source_cache::Effect&e,const source_cache::Policy&p){d.add(e,p);});
 return d.json();
}
}

int main(int argc,char**argv){using namespace source_cache;using namespace prefill_o_adl_test;
 try{
  if(argc==3&&std::string(argv[1])=="--emit-stream"){
   U P=std::stoul(argv[2]);need(P==256||P==512,"finite synthetic P");
   for(const auto& c:stream(command(P,false)))std::cout<<c.dump()<<'\n';return 0;
  }
  const bool expect_failure=argc==2&&std::string(argv[1])=="--expect-adl-failure";
  need(argc==1||expect_failure,"test CLI");J rows=J::array();U total=0;
  for(U P:{256,512}){
   auto c=command(P);auto ordinary=ordinary_probe(c),direct=direct_probe(c);
   need(ordinary==direct,"ordinary probe changed");
   if(expect_failure){
    bool rejected=false;try{native_prefill_sweep::adl_namespace_probe(c);}catch(const std::invalid_argument&e){rejected=std::string(e.what())=="Prefill sweep command type";}
    need(rejected,"old ADL failure not reproduced");
    rows.push_back({{"prefill",P},{"ordinary_probe",ordinary},{"namespace_sink_rejected",true}});
   }else{
    need(native_prefill_sweep::adl_namespace_probe(c)==ordinary,"namespace sink changes source effects");
    auto small=command(P,false);auto actual=cache_run(small,true),standalone=cache_run(small,false);
    need(actual==standalone,"execute cache output differs from standalone O");
    need(actual.at("configuration").at("L2").at("EF_hit_numerator")==288&&actual.at("configuration").at("L2").at("dirty_age_accesses")==64000000,"fixed diagnostic settings");
    rows.push_back({{"prefill",P},{"full48_CTA_source",ordinary},{"two_CTA_execute_equal",true},
                    {"two_CTA_snapshot",actual.at("snapshot")}});
   }
   total+=ordinary.at("events").get<U>();
  }
  std::cout<<J({{"status",expect_failure?"PASS_REPRODUCED_OLD_ADL_FAILURE":"PASS_PREFILL_O_QUALIFIED_ADL_REGRESSION"},
               {"full_CTA_source_events",total},{"cases",rows},{"synthetic_roots_only",true},
               {"whole_model_or_hardware_test",false}}).dump(2)<<'\n';return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
