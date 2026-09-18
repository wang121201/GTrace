#include "stage_replay.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace stage_test {
using U=std::uint64_t;using J=nlohmann::json;
namespace nt=native_trace;namespace g=GTSim;
namespace hbm=hbfsim::physical::hbm;
static U checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F&& function){bool rejected=false;try{function();}catch(const std::exception&){rejected=true;}check(rejected,"expected stage replay rejection");}
using Completion=std::array<U,9>;
using Admission=std::array<U,8>;
using Event=std::array<U,4>; // 0=open, 1=compute-start, 2=compute-finish; stage, call, cycle

J physical(const hbm::HbmStats& s){
    J j=J::object();
#define F(x) j[#x]=s.x
    F(read_bytes);F(write_bytes);F(controller_buffer_read_bytes);F(controller_buffer_write_bytes);
    F(controller_buffer_transfers);F(controller_buffer_bus_busy_ns);F(row_hits);F(row_misses);
    F(row_conflicts);F(activations);F(precharges);F(refresh_count);F(bus_busy_ns);F(finish_ns);
    F(pseudo_channels);F(active_pseudo_channels);F(max_pseudo_channel_accesses);F(max_queue_occupancy);
    F(max_pseudo_channel_busy_ns);F(avg_active_pseudo_channel_busy_ns);F(replicated_requests);F(replicated_bursts);
#undef F
    j["first_arrival_ns"]=std::isfinite(s.first_arrival_ns)?J(s.first_arrival_ns):J(nullptr);J work=J::object();
#define F(x) work[#x]=s.stage_work.x
    F(ingress_queue_wait_ns);F(scheduler_queue_wait_ns);F(address_mapping_ns);F(translation_ns);
    F(mapping_dram_ns);F(write_buffer_dram_ns);F(refresh_stall_ns);F(precharge_ns);F(activation_ns);
    F(command_ns);F(array_read_ns);F(array_program_ns);F(array_erase_ns);F(media_lane_transfer_ns);
    F(page_buffer_ns);F(sram_staging_ns);F(channel_transfer_ns);F(tsv_transfer_ns);F(hb_io_transfer_ns);
    F(transport_latency_ns);F(ecc_queue_wait_ns);F(ecc_latency_ns);F(maintenance_ns);
#undef F
    j["overlapping_stage_work_ns"]=work;return j;
}

hbm::HbmConfig config(){
    hbm::HbmConfig c;c.device.capacity_bytes=128ULL<<20;c.device.stacks=1;
    c.device.channels_per_stack=2;c.device.pseudo_channels_per_channel=1;
    c.device.bank_groups_per_pseudo_channel=2;c.device.banks_per_group=2;
    c.device.channel_row_size_bytes=1024;c.device.channel_width_bits=32;c.device.burst_length=8;
    c.controller.interleave_bytes=256;c.controller.queue_depth=4;
    c.controller.refresh_enabled=false;c.controller.same_bank_refresh=false;
    c.controller.replicate_symmetric_pseudo_channels=false;return c;
}

struct Stage {
    U call=0,begin=0,end=0,cta_begin=0,cta_end=1,compute=0;
};
J profile(const std::vector<Stage>& stages){
    J rows=J::array();for(U i=0;i<stages.size();++i){const auto&s=stages[i];rows.push_back({
        {"id",i},{"call_index",s.call},{"record_begin",s.begin},{"record_end",s.end},
        {"cta_begin",s.cta_begin},{"cta_end",s.cta_end},{"compute_cycles",s.compute}});}
    return {{"stages",rows},{"record_count",stages.empty()?0:stages.back().end}};
}

nt::Record record(U id,U call,U cta,U address,bool write=false){
    nt::Record r;r.request_id=r.source_sequence=id;r.call_index=call;r.cta=cta;
    r.source_matrix_id=1;r.source_line_address=0x100000000ULL+(address/128)*128;
    r.service_address=address;r.bytes=write?32:128;
    r.cause=write?nt::Cause::DirtyWriteback:nt::Cause::ReadFillOrRfo;
    r.node_id=id+10;r.sm_id=cta%2;r.l2_subpartition_id=id%4;return r;
}
U address(hbm::HbmDevice& layout,unsigned channel,U row,U offset=0){
    hbm::HbmAddress a;a.channel=channel;a.row=row;a.offset=offset;return layout.encode(a);
}
g::L2DramRequest request(const nt::Record&r,U issue,const sg_hbf::Clock&clock){
    g::L2DramRequest x;x.request_id=r.request_id;x.source_sequence=r.source_sequence;
    x.issue_cycle=issue;x.issue_time_ps=clock.issue_ps(issue);
    x.key={int(r.source_matrix_id),r.source_line_address};x.address=r.service_address;x.bytes=std::uint32_t(r.bytes);
    x.node_id=std::int64_t(r.node_id);x.sm_id=std::int32_t(r.sm_id);x.l2_subpartition_id=std::int32_t(r.l2_subpartition_id);
    x.cause=r.cause==nt::Cause::DirtyWriteback?g::L2DramRequestCause::DIRTY_WRITEBACK:g::L2DramRequestCause::FILL_READ;return x;
}
Completion completion(const g::L2DramCompletion&c,U at){return {c.request_id,c.source_sequence,c.issue_cycle,c.issue_time_ps,c.completion_cycle,U(c.key.matrix_id),c.key.line_addr,U(c.is_writeback),at};}
Admission admission(const nt::Record&r,U issue,U at){return {r.request_id,r.call_index,r.cta,r.service_address,r.bytes,U(r.cause),issue,at};}

struct State {bool open=false,done=false;U opened=0,ready=nt::unknown,start=nt::unknown,end=nt::unknown,completed=0;};
struct Run {J result;std::vector<Completion> completions;std::vector<Admission> admissions;std::vector<Event> events;};

// Independent, intentionally slow reference: all fixture records are known;
// time advances exactly one native tick, never using the new scheduler or its
// next-event calculation. A simple prefix state machine is enough for the
// tiny fixtures and gives a transparent oracle for all same-tick closures.
Run reference(const std::vector<Stage>& stages,const std::vector<nt::Record>& records,
              const hbm::HbmConfig& cfg,U window,U max_live,U credits,sg_hbf::DrainMode drain){
    sg_hbf::Clock clock(500,1);sg_hbf::HbfCreditBackend backend(clock,cfg,max_live,credits,1,drain);
    Run out;std::vector<State> states(stages.size());std::vector<U> owner(records.size());
    for(U i=0;i<stages.size();++i)for(U id=stages[i].begin;id<stages[i].end;++id)owner.at(id)=i;
    U cycle=0,opened=0,done=0,next=0,peak=0,compute_busy=0,memory_busy=0,both=0;
    std::optional<U> computing;
    auto settle=[&]{
        for(;;){
            if(computing&&states[*computing].end<=cycle){
                auto&id=*computing;check(states[id].end==cycle,"reference skipped compute completion");
                states[id].done=true;out.events.push_back({2,id,stages[id].call,cycle});++done;computing.reset();
            }
            while(opened<stages.size()&&opened-done<window){
                if(opened&&stages[opened].call!=stages[opened-1].call&&done!=opened)break;
                auto&s=states[opened];s.open=true;s.opened=cycle;
                if(stages[opened].begin==stages[opened].end)s.ready=cycle;
                out.events.push_back({0,opened,stages[opened].call,cycle});++opened;peak=std::max(peak,opened-done);
            }
            if(!computing&&done<opened&&states[done].completed==stages[done].end-stages[done].begin){
                auto&s=states[done];check(s.ready!=nt::unknown,"reference compute started without ready memory");
                s.start=cycle;s.end=cycle+stages[done].compute;computing=done;
                out.events.push_back({1,done,stages[done].call,cycle});
                if(stages[done].compute==0)continue;
            }
            break;
        }
    };
    settle();
    while(done<stages.size()||next<records.size()||backend.queue_depth()){
        while(next<records.size()&&states.at(owner[next]).open){
            const U id=owner[next];const auto r=request(records[next],states[id].opened,clock);
            if(!backend.try_enqueue(r,cycle))break;
            out.admissions.push_back(admission(records[next],states[id].opened,cycle));++next;
        }
        if(done==stages.size()&&next==records.size()&&backend.queue_depth()==0)break;
        check(cycle<1000000,"reference bounded tick count");
        compute_busy+=computing.has_value();memory_busy+=backend.queue_depth()!=0;
        both+=computing.has_value()&&backend.queue_depth()!=0;++cycle;
        for(const auto& c:backend.step(cycle)){
            out.completions.push_back(completion(c,cycle));const U id=owner.at(c.request_id);
            auto&s=states[id];++s.completed;
            if(s.completed==stages[id].end-stages[id].begin)s.ready=cycle;
        }
        settle();
    }
    backend.finalize();const auto&a=backend.admission_statistics();const auto&t=backend.statistics();J rows=J::array();
    for(U i=0;i<stages.size();++i){const auto&s=stages[i];const auto&x=states[i];rows.push_back({
        {"id",i},{"stage_id",i},{"call_index",s.call},{"record_begin",s.begin},{"record_end",s.end},
        {"cta_begin",s.cta_begin},{"cta_end",s.cta_end},{"compute_cycles",s.compute},
        {"open_cycle",x.opened},{"memory_ready_cycle",x.ready},{"compute_start_cycle",x.start},
        {"compute_finish_cycle",x.end},{"requests",s.end-s.begin},{"completed",x.completed}});}
    out.result={{"requests",records.size()},{"read_requests",t.fill_requests},{"write_requests",t.writeback_requests},
        {"read_bytes",t.fill_bytes},{"write_bytes",t.writeback_bytes},{"request_payload_fnv1a64",a.actual_request_shape_fnv1a64},
        {"native_request_payload_fnv1a64",a.actual_request_shape_fnv1a64},{"replay_cycles",cycle},{"makespan_cycles",cycle},
        {"physical",physical(backend.physical_statistics())},{"stages",rows},{"peak_active_stages",peak},
        {"compute_busy_cycles",compute_busy},{"native_outstanding_cycles",memory_busy},
        {"compute_native_outstanding_overlap_cycles",both}};
    check(a.accepted==records.size()&&a.completed==records.size()&&a.reserved_bursts==0,
          "reference complete native ledger");return out;
}

Run candidate(const J& plan,const std::vector<nt::Record>& records,const hbm::HbmConfig&cfg,
              U window,U max_live,U credits,sg_hbf::DrainMode drain){
    stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,plan,window,max_live,credits,drain,1000000);Run out;
    replay.set_completion_observer([&](const g::L2DramCompletion&c,U at){out.completions.push_back(completion(c,at));});
    replay.set_admission_observer([&](const nt::Record&r,U issue,U at){out.admissions.push_back(admission(r,issue,at));});
    replay.set_event_observer([&](const stage_replay::Event&e){
        U kind=nt::unknown;
        if(e.kind==stage_replay::Event::Kind::StageOpened)kind=0;
        if(e.kind==stage_replay::Event::Kind::ComputeStarted)kind=1;
        if(e.kind==stage_replay::Event::Kind::ComputeFinished)kind=2;
        check(kind!=nt::unknown,"unknown observed stage event");out.events.push_back({kind,e.stage_id,e.call_index,e.cycle});
    });
    for(const auto&r:records)replay.accept(r);out.result=replay.finish();
    rejects([&]{replay.finish();});rejects([&]{replay.accept(record(records.size(),0,0,0));});return out;
}

void compare(const Run& actual,const Run& expected){
    check(actual.result.at("status")=="PASS_CLOSED_STAGE_OVERLAP_REPLAY","candidate did not finish stage replay");
    for(const auto& field:expected.result.items()){
        if(field.key()=="stages")continue;
        check(actual.result.at(field.key())==field.value(),"stage replay differs from independent tick oracle");
    }
    check(actual.result.at("stages").size()==expected.result.at("stages").size(),"stage count changed");
    for(U i=0;i<expected.result.at("stages").size();++i)
        for(const auto& field:expected.result.at("stages").at(i).items())
            check(actual.result.at("stages").at(i).at(field.key())==field.value(),"stage interval or census changed");
    check(actual.events==expected.events,"same-tick stage open/compute event order differs");
    check(actual.admissions==expected.admissions,"stage release/admission order or request payload differs");
    check(actual.completions==expected.completions,"native completion sequence or observation time differs");
}

J fixture(const std::string&name,const std::vector<Stage>&stages,const std::vector<nt::Record>&records,
          const hbm::HbmConfig&cfg,U window=2,U max_live=2,sg_hbf::DrainMode drain=sg_hbf::DrainMode::Global){
    auto expected=reference(stages,records,cfg,window,max_live,4,drain);
    auto actual=candidate(profile(stages),records,cfg,window,max_live,4,drain);compare(actual,expected);
    // A payload-only ledger is independent of both replay schedulers.
    nt::Ledger ledger;ledger.value.mode=nt::Mode::FunctionalDirect;for(const auto&r:records)ledger.accept(r);
    check(actual.result.at("request_payload_fnv1a64")==ledger.value.request_payload_fnv1a64&&
        actual.result.at("read_bytes")==ledger.value.read_bytes&&actual.result.at("write_bytes")==ledger.value.write_bytes,
        "stage scheduler reordered, repeated, dropped or re-cached the direct trace");
    return {{"name",name},{"status","PASS_EXACT_TICK_REFERENCE"},{"window_stages",window},
        {"max_live",max_live},{"drain",drain==sg_hbf::DrainMode::Global?"global":"independent"},
        {"reference",expected.result},{"events",actual.events},{"admissions",actual.admissions},
        {"completions",actual.completions},{"safe_completion_jumps",actual.result.value("safe_completion_jumps",U(0))}};
}

void negatives(const hbm::HbmConfig&cfg){
    const auto valid=profile({{0,0,1,0,1,1},{0,1,2,1,2,2}});
    for(unsigned variant=0;variant<12;++variant){
        J bad=valid;auto&stages=bad["stages"];
        if(variant==0)stages[0]["id"]=1;
        if(variant==1)stages[0]["record_begin"]=1;
        if(variant==2)stages[1]["record_begin"]=0;
        if(variant==3)stages[1]["record_begin"]=2;
        if(variant==4)stages[0]["record_end"]=0,stages[0]["record_begin"]=1;
        if(variant==5)stages[0]["cta_end"]=0;
        if(variant==6)stages[1]["cta_begin"]=2;
        if(variant==7)stages[0]["call_index"]=1,stages[1]["call_index"]=0;
        if(variant==8)stages[0]["compute_cycles"]=-1;
        if(variant==9)stages[0]["compute_cycles"]=true;
        if(variant==10)bad["record_count"]=3;
        if(variant==11)stages[0]["cta_begin"]=nt::unknown,stages[0]["cta_end"]=0;
        rejects([&]{stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,bad,2,2,4);});
    }
    for(unsigned variant=0;variant<9;++variant){
        stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,valid,2,2,4);auto bad=record(0,0,0,0);
        if(variant==0)bad.call_index=1;if(variant==1)bad.cta=1;if(variant==2)bad.cta=nt::unknown;
        if(variant==3)bad.request_id=1;if(variant==4)bad.source_sequence=1;
        if(variant==5)bad.bytes=32;if(variant==6)bad.service_address=1;
        if(variant==7)bad.service_address=cfg.device.capacity_bytes;if(variant==8)bad.issue_cycle=0;
        rejects([&]{replay.accept(bad);});rejects([&]{replay.finish();});
    }
    {stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,valid,2,2,4);
        replay.accept(record(0,0,0,0));rejects([&]{replay.finish();});}
    {stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,profile({{0,0,1,0,1,0}}),2,2,4);
        replay.accept(record(0,0,0,0));rejects([&]{replay.accept(record(1,0,0,0));});}
    {stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,profile({{0,0,2,0,2,0}}),2,2,4);
        replay.accept(record(0,0,1,0));rejects([&]{replay.accept(record(1,0,0,128));});}
    {stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,profile({{0,0,1,0,1,0}}),2,2,4,sg_hbf::DrainMode::Global,1);
        replay.accept(record(0,0,0,0));rejects([&]{replay.finish();});}
    rejects([&]{stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,profile({{0,0,0,0,1,100}}),2,2,4,sg_hbf::DrainMode::Global,99);
        (void)replay.finish();});
    // Overflow / signed-native-time guard may reject during construction or
    // finish, but must never wrap and publish a successful result.
    rejects([&]{stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,
        profile({{0,0,0,0,1,U(INT64_MAX)},{0,0,0,1,2,U(INT64_MAX)}}),2,2,4,
        sg_hbf::DrainMode::Global,U(INT64_MAX));(void)replay.finish();});
    rejects([&]{stage_replay::Replay replay(sg_hbf::Clock(500,1),cfg,valid,0,2,4);});
}

J run(){
    static_assert(g::kTilegenDirtySectorMode==2,"stage tests require individual dirty32 build");
    const auto cfg=config();hbm::HbmDevice layout(cfg);const U a=address(layout,0,0),b=address(layout,1,0),c=address(layout,0,1);
    J results=J::array();
    results.push_back(fixture("empty-profile",{},{},cfg));
    results.push_back(fixture("zero-DRAM-zero-compute",{{0,0,0,0,1,0},{0,0,0,1,2,0},{1,0,0,0,1,0}},{},cfg));
    auto tail=fixture("compute-only-tail",{{0,0,0,0,1,7},{0,0,0,1,2,11},{1,0,0,0,1,13}},{},cfg);
    check(tail.at("reference").at("makespan_cycles")==31,"empty-memory compute tail lost");results.push_back(tail);
    const std::vector<Stage> mixed{{0,0,2,0,1,40},{0,2,4,1,2,160},{0,4,4,2,3,19},
        {0,4,6,3,4,0},{1,6,8,0,1,23},{1,8,8,1,2,17}};
    const std::vector<nt::Record> memory{record(0,0,0,a),record(1,0,0,b),
        record(2,0,1,a+32,true),record(3,0,1,c),record(4,0,3,b+64,true),record(5,0,3,c),
        record(6,1,0,c),record(7,1,0,b+96,true)};
    for(auto drain:{sg_hbf::DrainMode::Global,sg_hbf::DrainMode::Independent})
        for(U window:{U(1),U(2)}){
            auto row=fixture("mixed-stage-flow",mixed,memory,cfg,window,2,drain);
            const U overlap=row.at("reference").at("compute_native_outstanding_overlap_cycles");
            check(window==1?overlap==0:overlap>0,"fixture does not distinguish serial from prefetched execution");
            check(row.at("reference").at("compute_busy_cycles")==259,"serial compute work changed with prefetch window");
            results.push_back(row);
        }
    // A compute-only head starts at t=0 while prefetched memory is live. Its
    // completion at 55 lies after this fixture's RD command issue frontier
    // and before its CAS/data completion. A known native completion must not
    // let the implementation jump over that earlier compute deadline.
    const auto jump=fixture("compute-deadline-before-native-completion",
        {{0,0,0,0,1,55},{0,0,1,1,2,0},{0,1,1,2,3,9}}, {record(0,0,1,a)},cfg);
    check(jump.at("reference").at("stages")[0].at("compute_finish_cycle")==55,
        "compute end crossed by native completion jump");results.push_back(jump);
    auto memory_tail=fixture("memory-then-long-compute-tail",{{0,0,1,0,1,1000}}, {record(0,0,0,a)},cfg);
    const auto&ts=memory_tail.at("reference").at("stages")[0];
    check(ts.at("compute_finish_cycle").get<U>()-ts.at("memory_ready_cycle").get<U>()==1000,
        "compute tail missing from total latency");results.push_back(memory_tail);
    auto boundary=fixture("cross-call-row-retention",{{0,0,1,0,1,7},{1,1,2,0,1,11}},
        {record(0,0,0,a),record(1,1,0,a)},cfg);
    check(boundary.at("reference").at("physical").at("activations")==1&&
        boundary.at("reference").at("physical").at("row_hits")==7,
        "call barrier reset row state or duplicate source was cached");
    check(boundary.at("reference").at("stages")[1].at("open_cycle")==
        boundary.at("reference").at("stages")[0].at("compute_finish_cycle"),"cross-call barrier missing");
    results.push_back(boundary);
    negatives(cfg);
    return {{"status","PASS_STAGE_REPLAY_EXACT_TICK_REFERENCE"},{"checks",checks},{"CPU_only",true},
        {"GPU_or_cache_simulated",false},{"native_HBFSIM_microfixtures",true},
        {"reference","Independent finite native-backend driver; one tick per advance, no production stage scheduler or jump logic"},
        {"fixtures",results}};
}
} // namespace stage_test
int main(){try{std::cout<<stage_test::run().dump(2)<<'\n';return 0;}
catch(const std::exception&e){std::cerr<<nlohmann::json({{"status","FAIL"},{"reason",e.what()}}).dump()<<'\n';return 1;}}
