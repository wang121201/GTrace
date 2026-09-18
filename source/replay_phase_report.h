#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace replay_phase_report {
using J=nlohmann::json;using U=std::uint64_t;
inline void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline U natural(const J& value){
    need(value.is_number_integer()&&(value.is_number_unsigned()||value.get<std::int64_t>()>=0),"phase report expects a nonnegative integer");
    return value.get<U>();
}
inline U add(U a,U b){need(b<=std::numeric_limits<U>::max()-a,"phase report counter overflow");return a+b;}
inline void annotate(J& out,const J& source){
    const U numerator=natural(out.at("clock").at("period_ps_numerator"));
    const U denominator=natural(out.at("clock").at("period_ps_denominator"));
    need(numerator&&denominator,"phase report clock must be positive");
    const long double cycle_ns=static_cast<long double>(numerator)/denominator/1000.0L;
    const double mhz=source.at("phase_profile").at("clock_frequency_MHz").get<double>();
    const long double expected_mhz=1000.0L/cycle_ns;
    need(std::isfinite(mhz)&&mhz>0&&std::abs(mhz-expected_mhz)<=expected_mhz*1e-9L,"phase report source/replay clock differs");
    auto ns=[&](U cycles){return double(cycles*cycle_ns);};
    auto span=[&](J& row){
        const U begin=natural(row.at("open_cycle")),end=natural(row.at("compute_finish_cycle"));
        need(end>=begin,"phase report reversed makespan");
        const U cycles=end-begin;const double duration=ns(cycles);
        const U r=natural(row.at("read_bytes")),w=natural(row.at("write_bytes"));
        row["makespan_cycles"]=cycles;row["makespan_ns"]=duration;
        need(natural(row.at("compute_cycles"))<=cycles,"phase report compute exceeds makespan");
        row["compute_ns"]=ns(natural(row.at("compute_cycles")));
        row["read_bandwidth_GBps"]=duration>0?J(double(r)/duration):J(nullptr);
        row["write_bandwidth_GBps"]=duration>0?J(double(w)/duration):J(nullptr);
        row["aggregate_bandwidth_GBps"]=duration>0?J(double(add(r,w))/duration):J(nullptr);
        const auto& first=row.at("first_admission_cycle");const auto& last=row.at("last_completion_cycle");
        const U requests=natural(row.at("requests"));
        if(!requests){need(first.is_null()&&last.is_null()&&!r&&!w,"phase report empty memory ledger differs");row["memory_window_ns"]=nullptr;}
        else{need(!first.is_null()&&!last.is_null(),"phase report missing memory endpoints");
            const U a=natural(first),b=natural(last);need(begin<=a&&a<=b&&b<=end,"phase report memory endpoints outside makespan");
            row["memory_window_ns"]=ns(b-a);}
    };
    auto& calls=out.at("calls");auto& stages=out.at("stages");const auto& pipeline=source.at("pipeline");
    need(calls.is_array()&&stages.is_array()&&pipeline.is_array()&&calls.size()==pipeline.size(),"phase report call coverage differs");
    constexpr const char* counters[]={"requests","read_requests","write_requests","read_bytes","write_bytes","compute_cycles"};
    std::vector<J> totals(calls.size(),J::object());std::vector<U> counts(calls.size(),0),last_finish(calls.size(),0);
    for(auto& total:totals)for(const auto* key:counters)total[key]=U(0);
    U previous_call=0,next_stage=0;
    for(auto& stage:stages){
        const U call=natural(stage.at("call_index"));
        need(call<calls.size()&&call>=previous_call&&natural(stage.at("id"))==next_stage++,"phase report stage order differs");previous_call=call;
        const U start=natural(stage.at("compute_start_cycle")),finish=natural(stage.at("compute_finish_cycle"));
        need(start>=natural(stage.at("memory_ready_cycle"))&&start>=natural(stage.at("open_cycle"))&&finish>=start&&finish-start==natural(stage.at("compute_cycles")),"phase report stage compute interval differs");
        const U call_open=natural(calls.at(call).at("open_cycle"));
        need(natural(stage.at("open_cycle"))>=call_open&&finish<=natural(calls.at(call).at("compute_finish_cycle"))&&
            (counts[call]||natural(stage.at("open_cycle"))==call_open),"phase report stage lies outside call window");
        need(!counts[call]||start>=last_finish[call],"phase report compute lanes overlap");last_finish[call]=finish;++counts[call];
        need(natural(stage.at("requests"))==add(natural(stage.at("read_requests")),natural(stage.at("write_requests"))),"phase report stage request ledger differs");
        span(stage);stage["phase"]=pipeline.at(call).at("phase");
        stage["bandwidth_denominator"]="local open-to-compute-finish residence; stages may overlap and these spans must not be summed";
        for(const auto* key:counters)totals[call][key]=add(natural(totals[call][key]),natural(stage.at(key)));
    }
    J phases=J::array();U end=0,compute=0,r=0,w=0,requests=0,rr=0,wr=0;
    for(U i=0;i<calls.size();++i){
        auto& call=calls.at(i);const auto& origin=pipeline.at(i);
        need(natural(call.at("call_index"))==i&&natural(call.at("open_cycle"))==end&&counts[i]>0,"phase report serial call barrier differs");
        need(origin.at("phase").is_string()&&!origin.at("phase").get<std::string>().empty()&&call.at("phase")==origin.at("phase"),"phase report source phase differs");
        need(call.at("read_bytes")==origin.at("DRAM_read_bytes")&&call.at("write_bytes")==origin.at("DRAM_write_bytes"),"phase report source call bytes differ");
        for(const auto* key:counters)need(natural(call.at(key))==natural(totals[i][key]),"phase report stage/call counters differ");
        need(natural(call.at("stages"))==counts[i]&&natural(call.at("compute_finish_cycle"))==last_finish[i],"phase report stage/call endpoint differs");
        span(call);end=natural(call.at("compute_finish_cycle"));
        if(phases.empty()||phases.back().at("phase")!=origin.at("phase")){
            J group={{"phase",origin.at("phase")},{"call_begin",i},{"call_end",i},{"kernel_count",U(0)},
                {"open_cycle",call.at("open_cycle")},{"first_admission_cycle",nullptr},{"last_completion_cycle",nullptr}};
            for(const auto* key:counters)group[key]=U(0);phases.push_back(std::move(group));
        }
        auto& group=phases.back();group["call_end"]=i+1;group["kernel_count"]=add(natural(group["kernel_count"]),1);
        group["compute_finish_cycle"]=end;
        for(const auto* key:counters)group[key]=add(natural(group[key]),natural(call.at(key)));
        if(!call.at("first_admission_cycle").is_null()){
            if(group.at("first_admission_cycle").is_null())group["first_admission_cycle"]=call.at("first_admission_cycle");
            group["last_completion_cycle"]=call.at("last_completion_cycle");
        }
        r=add(r,natural(call.at("read_bytes")));w=add(w,natural(call.at("write_bytes")));
        requests=add(requests,natural(call.at("requests")));rr=add(rr,natural(call.at("read_requests")));wr=add(wr,natural(call.at("write_requests")));
        compute=add(compute,natural(call.at("compute_cycles")));
    }
    need(r==natural(out.at("read_bytes"))&&w==natural(out.at("write_bytes"))&&requests==natural(out.at("requests"))&&rr==natural(out.at("read_requests"))&&wr==natural(out.at("write_requests")),"phase report total traffic differs");
    const U full=natural(out.at("makespan_cycles"));
    const U serving=out.contains("stage_makespan_cycles")?natural(out.at("stage_makespan_cycles")):full;
    const U tail=out.contains("persistence_tail_cycles")?natural(out.at("persistence_tail_cycles")):0;
    need(end==serving&&add(serving,tail)==full&&compute==natural(out.at("compute_busy_cycles")),"phase report serving/persistence/full timing differs");
    for(const auto& item:std::vector<std::pair<const char*,U>>{{"makespan_ns",full},{"stage_makespan_ns",serving},{"persistence_tail_ns",tail}}){
        if(!out.contains(item.first))continue;
        const double actual=out.at(item.first).get<double>(),expected=ns(item.second);
        need(std::isfinite(actual)&&std::abs(actual-expected)<=std::max(1.0,expected)*1e-9,"phase report nanoseconds differ from clock");
    }
    for(auto& group:phases)span(group);
    out["semantic_phases"]=std::move(phases);
    out["phase_report"]={{"schema","TILEGEN_STAGE_REPLAY_PHASE_REPORT_V1"},{"ledger_closed",true},
        {"grouping","consecutive exact source.pipeline phase labels; half-open call ranges; selected source scope only"},
        {"bandwidth_unit","decimal GB/s = bytes/ns"},{"phase_bandwidth_denominator","first call open through last call compute finish, including compute tail but excluding EOF persistence; ratio of sums, not mean bandwidth"},
        {"memory_window_definition","first observed admission through last observed completion; includes gaps; not bus busy time or physical per-stage service time"},
        {"serving_makespan_cycles",serving},{"serving_makespan_ns",ns(serving)},
        {"persistence_tail_cycles",tail},{"persistence_tail_ns",ns(tail)},
        {"full_makespan_cycles",full},{"full_makespan_ns",ns(full)},
        {"EOF_persistence_attributed_to_semantic_phases",false},
        {"persistence_scope","EOF device drain/maintenance is separate from kernel and phase serving windows; full durable bandwidth uses full makespan"},
        {"stage_span_additive",false},{"per_stage_overlap_measured",false},{"hardware_accuracy_qualified",false}};
}
} // namespace replay_phase_report
