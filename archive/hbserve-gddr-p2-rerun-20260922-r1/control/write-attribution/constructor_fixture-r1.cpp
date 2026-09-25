#include <nlohmann/json.hpp>
#include <iostream>
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/tilegen-full-r1/core-native-copy-r2/include/writer_observer.h"
using J=nlohmann::json;
unsigned checks=0;
void check(bool yes,const char*why){++checks;if(!yes)throw std::runtime_error(why);}
template<class F>void reject(F fn){bool rejected=false;try{fn();}catch(const std::logic_error&){rejected=true;}check(rejected,"out-of-domain constructor accepted");}
J close_zero(decode_writer::Observation&o,int count){
 const decode_writer::Native zero{};o.begin(1,zero);
 for(int i=0;i<count;++i)o.set_call(i);
 o.check_begin();o.end(zero);auto j=o.report<J>();
 check(j.at("status")=="PASS_CLOSED_PASSIVE_ATTRIBUTION","zero native ledger not closed");
 check(j.at("calls").size()==static_cast<unsigned>(count),"call domain size");
 for(int i=0;i<count;++i){auto r=j.at("calls").at(i);check(r.at("call_index")==i,"contiguous call identity");r.erase("call_index");for(auto&v:r)check(v==0,"empty observation has mutation");}
 for(const auto*k:{"initial_inherited_dirty_sectors","created_dirty_sectors","evicted_dirty_sectors","final_dirty_sectors","writeback_bytes","processed_stores"})check(j.at(k)==0,"zero native totals");
 return {{"calls",count},{"status",j.at("status")},{"dirty",{{"I",0},{"C",0},{"E",0},{"F",0}}}};
}
int main(){try{
 reject([]{decode_writer::Observation x(0,1138);});
 reject([]{decode_writer::Observation x(0,3020,false,nullptr,3020);});
 decode_writer::Observation old(0,1137);auto old_result=close_zero(old,1138);
 decode_writer::Observation extended(0,3017,false,nullptr,3020);auto new_result=close_zero(extended,3018);
 std::cout<<J({{"status","PASS_ACTUAL_CONSTRUCTOR_DOMAIN_AND_ZERO_NATIVE_LEDGER"},{"checks",checks},{"default_domain",old_result},{"extended_domain",new_result},{"rejections",2},{"cache_or_model_executed",false}}).dump()<<'\n';return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
