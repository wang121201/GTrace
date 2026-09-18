#include "replay_phase_report.h"
#include <iostream>
using J=nlohmann::json;using U=std::uint64_t;
static unsigned checks=0;
void check(bool ok){++checks;if(!ok)throw std::runtime_error("phase report assertion failed");}
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
J row(U call,U open,U start,U finish,U r,U w,J first,J last){
    return {{"call_index",call},{"open_cycle",open},{"compute_start_cycle",start},{"compute_finish_cycle",finish},
        {"memory_ready_cycle",start},{"compute_cycles",finish-start},{"read_bytes",r},{"write_bytes",w},
        {"requests",r/128+w/32},{"read_requests",r/128},{"write_requests",w/32},
        {"first_admission_cycle",first},{"last_completion_cycle",last}};
}
std::pair<J,J> fixture(){
    // 0.5 ns/cycle: two overlapping stages in call 0, then two serial kernels.
    J a=row(0,0,10,20,128,0,0,10),b=row(0,0,20,30,0,32,1,15);
    J c=row(1,30,35,50,128,0,30,35),d=row(2,50,50,60,0,0,nullptr,nullptr);
    J stages=J::array({a,b,c,d});for(U i=0;i<stages.size();++i)stages[i]["id"]=i;
    J call0=row(0,0,10,30,128,32,0,15);call0["compute_cycles"]=20;
    J calls=J::array({call0,c,d});J pipeline=J::array();
    for(U i=0;i<calls.size();++i){calls[i]["stages"]=i==0?2:1;calls[i]["phase"]=i<2?"Prefill":"Decode1";
        pipeline.push_back({{"phase",calls[i]["phase"]},{"DRAM_read_bytes",calls[i]["read_bytes"]},{"DRAM_write_bytes",calls[i]["write_bytes"]}});}
    J out={{"clock",{{"period_ps_numerator",500},{"period_ps_denominator",1}}},{"calls",calls},{"stages",stages},
        {"requests",3},{"read_requests",2},{"write_requests",1},{"read_bytes",256},{"write_bytes",32},
        {"makespan_cycles",60},{"compute_busy_cycles",45}};
    J source={{"phase_profile",{{"clock_frequency_MHz",2000.0}}},{"pipeline",pipeline}};return {out,source};
}
int main(){try{
    auto [out,source]=fixture();replay_phase_report::annotate(out,source);
    check(out["phase_report"]["ledger_closed"]==true);check(out["semantic_phases"].size()==2);
    const auto& p=out["semantic_phases"][0];check(p["phase"]=="Prefill"&&p["kernel_count"]==2&&p["call_begin"]==0&&p["call_end"]==2);
    check(p["makespan_ns"]==25.0&&p["compute_ns"]==17.5&&p["memory_window_ns"]==17.5);
    check(p["read_bandwidth_GBps"]==256.0/25.0&&p["write_bandwidth_GBps"]==32.0/25.0&&p["aggregate_bandwidth_GBps"]==288.0/25.0);
    check(out["calls"][0]["makespan_ns"]==15.0);check(out["stages"][0]["makespan_ns"]==10.0&&out["stages"][1]["makespan_ns"]==15.0);
    check(out["semantic_phases"][1]["makespan_ns"]==5.0&&out["semantic_phases"][1]["memory_window_ns"].is_null());
    check(out["semantic_phases"][1]["aggregate_bandwidth_GBps"]==0.0);
    auto bad=[&](auto mutate){auto [a,b]=fixture();mutate(a,b);rejects([&]{replay_phase_report::annotate(a,b);});};
    bad([](J&a,J&){a["calls"][1]["open_cycle"]=31;});
    bad([](J&,J&b){b["phase_profile"]["clock_frequency_MHz"]=1000.;});
    bad([](J&a,J&){a["read_bytes"]=128;});bad([](J&a,J&){a["stages"][1]["read_bytes"]=128;});
    bad([](J&a,J&){a["stages"][0]["compute_cycles"]=-1;});
    bad([](J&a,J&){a["stages"][0]["last_completion_cycle"]=21;});
    bad([](J&a,J&){a["calls"][2]["phase"]="Decode2";});
    bad([](J&a,J&){a["makespan_cycles"]=59;});
    bad([](J&a,J&){a["makespan_cycles"]=80;a["stage_makespan_cycles"]=60;a["persistence_tail_cycles"]=19;});
    bad([](J&a,J&){a["makespan_cycles"]=80;a["stage_makespan_cycles"]=60;});
    bad([](J&a,J&){a["persistence_tail_cycles"]=-1;});
    bad([](J&a,J&){a["stage_makespan_ns"]=31.0;});
    bad([](J&a,J&){a["stage_makespan_cycles"]=60;a["persistence_tail_cycles"]=std::numeric_limits<U>::max();});
    // EOF durability is charged only to Full, never to the last inference phase.
    auto [durable,durable_source]=fixture();durable["stage_makespan_cycles"]=60;durable["stage_makespan_ns"]=30.0;
    durable["persistence_tail_cycles"]=20;durable["persistence_tail_ns"]=10.0;
    durable["makespan_cycles"]=80;durable["makespan_ns"]=40.0;durable["aggregate_bandwidth_GBps"]=288.0/40.0;
    replay_phase_report::annotate(durable,durable_source);
    check(durable["semantic_phases"]==out["semantic_phases"]);
    check(durable["aggregate_bandwidth_GBps"]==288.0/40.0&&durable["phase_report"]["serving_makespan_ns"]==30.0);
    check(durable["phase_report"]["persistence_tail_ns"]==10.0&&durable["phase_report"]["EOF_persistence_attributed_to_semantic_phases"]==false);
    // Identical labels separated by another phase remain separate groups.
    auto [a,b]=fixture();a["calls"][1]["phase"]=b["pipeline"][1]["phase"]="Decode1";
    a["calls"][2]["phase"]=b["pipeline"][2]["phase"]="Prefill";
    replay_phase_report::annotate(a,b);check(a["semantic_phases"].size()==3);
    // Zero-duration/zero-byte stage has no artificial infinity bandwidth.
    auto [z,s]=fixture();auto& last=z["calls"][2];last["compute_finish_cycle"]=50;last["compute_cycles"]=0;
    z["stages"][3]["compute_finish_cycle"]=50;z["stages"][3]["compute_cycles"]=0;
    z["makespan_cycles"]=50;z["compute_busy_cycles"]=35;replay_phase_report::annotate(z,s);
    check(z["semantic_phases"][1]["aggregate_bandwidth_GBps"].is_null());
    std::cout<<J({{"status","PASS"},{"checks",checks}}).dump()<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
