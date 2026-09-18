#include "trace_replay.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace replay_test {
using U=std::uint64_t;
using J=nlohmann::json;
namespace nt=native_trace;
namespace g=GTSim;
namespace hbm=hbfsim::physical::hbm;
namespace fs=std::filesystem;
static U checks=0;

void check(bool value,const char* why) {
    ++checks;if(!value)throw std::runtime_error(why);
}
template<class F> void rejects(F&& operation) {
    bool rejected=false;
    try {operation();}catch(const std::exception&){rejected=true;}
    check(rejected,"expected replay rejection");
}
using Completion=std::array<U,9>;
Completion completion_row(const g::L2DramCompletion& value,U observed_cycle) {
    return {value.request_id,value.source_sequence,value.issue_cycle,value.issue_time_ps,
        value.completion_cycle,U(value.key.matrix_id),value.key.line_addr,
        U(value.is_writeback),observed_cycle};
}

// Keep this serializer independent of replay's output helper. Include every
// actual HbmStats counter and all additive stage-work fields, not just bytes.
J physical_stats(const hbm::HbmStats& stats) {
    J result=J::object();
#define FIELD(name) result[#name]=stats.name
    FIELD(read_bytes);FIELD(write_bytes);FIELD(controller_buffer_read_bytes);
    FIELD(controller_buffer_write_bytes);FIELD(controller_buffer_transfers);
    FIELD(controller_buffer_bus_busy_ns);FIELD(row_hits);FIELD(row_misses);
    FIELD(row_conflicts);FIELD(activations);FIELD(precharges);FIELD(refresh_count);
    FIELD(bus_busy_ns);FIELD(finish_ns);FIELD(pseudo_channels);FIELD(active_pseudo_channels);
    FIELD(max_pseudo_channel_accesses);FIELD(max_queue_occupancy);
    FIELD(max_pseudo_channel_busy_ns);FIELD(avg_active_pseudo_channel_busy_ns);
    FIELD(replicated_requests);FIELD(replicated_bursts);
#undef FIELD
    result["first_arrival_ns"]=std::isfinite(stats.first_arrival_ns)?J(stats.first_arrival_ns):J(nullptr);
    J work=J::object();
#define WORK(name) work[#name]=stats.stage_work.name
    WORK(ingress_queue_wait_ns);WORK(scheduler_queue_wait_ns);WORK(address_mapping_ns);
    WORK(translation_ns);WORK(mapping_dram_ns);WORK(write_buffer_dram_ns);
    WORK(refresh_stall_ns);WORK(precharge_ns);WORK(activation_ns);WORK(command_ns);
    WORK(array_read_ns);WORK(array_program_ns);WORK(array_erase_ns);
    WORK(media_lane_transfer_ns);WORK(page_buffer_ns);WORK(sram_staging_ns);
    WORK(channel_transfer_ns);WORK(tsv_transfer_ns);WORK(hb_io_transfer_ns);
    WORK(transport_latency_ns);WORK(ecc_queue_wait_ns);WORK(ecc_latency_ns);WORK(maintenance_ns);
#undef WORK
    result["overlapping_stage_work_ns"]=work;return result;
}

hbm::HbmConfig config() {
    hbm::HbmConfig result;
    result.device.capacity_bytes=128ULL<<20;
    result.device.stacks=1;result.device.channels_per_stack=2;
    result.device.pseudo_channels_per_channel=1;
    result.device.bank_groups_per_pseudo_channel=2;result.device.banks_per_group=2;
    result.device.channel_row_size_bytes=1024;result.device.channel_width_bits=32;
    result.device.burst_length=8;result.controller.interleave_bytes=256;
    result.controller.queue_depth=4;
    result.controller.refresh_enabled=false;result.controller.same_bank_refresh=false;
    result.controller.replicate_symmetric_pseudo_channels=false;
    return result;
}

nt::Record record(U id,U address,bool write,U call) {
    nt::Record result;result.request_id=result.source_sequence=id;
    result.call_index=call;result.source_matrix_id=1;
    result.source_line_address=0x100000000ULL+(address/128)*128;
    result.service_address=address;result.bytes=write?32:128;
    result.cause=write?nt::Cause::DirtyWriteback:nt::Cause::ReadFillOrRfo;
    result.node_id=id+10;result.sm_id=id%2;result.l2_subpartition_id=id%4;
    return result;
}

U address(hbm::HbmDevice& layout,unsigned channel,U row,U offset=0) {
    hbm::HbmAddress wanted;wanted.channel=channel;wanted.row=row;wanted.offset=offset;
    const U encoded=layout.encode(wanted);const auto decoded=layout.decode(encoded);
    check(decoded.channel==channel&&decoded.row==row&&decoded.offset==offset&&
        decoded.bank==0&&decoded.bank_group==0,"fixture address geometry");
    return encoded;
}

std::vector<nt::Record> mixed_records(const hbm::HbmConfig& cfg) {
    hbm::HbmDevice layout(cfg);std::vector<nt::Record> result;
    auto append=[&](unsigned channel,U row,U offset,bool write) {
        const U id=result.size();result.push_back(record(id,address(layout,channel,row,offset),write,id/3));
    };
    // First two reads can occupy distinct channels at once. Later same-bank
    // row changes force ACT/PRE. Exact repeats must remain separate requests.
    for(unsigned repeat=0;repeat<3;++repeat) {
        append(0,0,0,false);append(1,0,0,false);
        append(0,0,32,true);append(0,1,0,false);
        append(1,0,64,true);append(0,1,0,false);
        append(1,1,0,false);append(0,0,0,false);
        append(0,0,96,true);append(0,0,0,false);
    }
    return result;
}

g::L2DramRequest original_request(const nt::Record& source) {
    g::L2DramRequest result;
    result.request_id=source.request_id;result.source_sequence=source.source_sequence;
    // Original native backend receives every replay request ready at t=0.
    // Trace timing is unknown and never becomes a synthetic GPU issue time.
    result.issue_cycle=result.issue_time_ps=0;
    result.key={int(source.source_matrix_id),source.source_line_address};
    result.address=source.service_address;result.bytes=std::uint32_t(source.bytes);
    result.node_id=std::int64_t(source.node_id);result.sm_id=std::int32_t(source.sm_id);
    result.l2_subpartition_id=std::int32_t(source.l2_subpartition_id);
    result.cause=source.cause==nt::Cause::DirtyWriteback?
        g::L2DramRequestCause::DIRTY_WRITEBACK:g::L2DramRequestCause::FILL_READ;
    return result;
}

struct Reference {
    J summary;
    std::vector<Completion> completions;
    std::map<U,J> calls;
    U blocked=0,peak_live=0,peak_credits=0,cycles=0;
};

Reference tick_reference(const std::vector<nt::Record>& records,const hbm::HbmConfig& cfg,
                         U max_live,U credits,sg_hbf::DrainMode drain) {
    sg_hbf::HbfCreditBackend backend(sg_hbf::Clock(500,1),cfg,max_live,credits,1,drain);
    Reference result;U reads=0,writes=0,read_bytes=0,write_bytes=0;
    auto advance=[&] {
        // Deliberately never consult next_completion_cycle or any replay code.
        check(result.cycles<1000000,"reference cycle budget");++result.cycles;
        for(const auto& completion:backend.step(result.cycles)) {
            check(completion.request_id<records.size(),"reference completion identity");
            result.completions.push_back(completion_row(completion,result.cycles));
            const U call=records.at(completion.request_id).call_index;
            auto& row=result.calls.at(call);
            if(row["first_completion_cycle"].is_null())row["first_completion_cycle"]=completion.completion_cycle;
            row["last_completion_cycle"]=completion.completion_cycle;
        }
    };
    for(const auto& item:records) {
        const auto request=original_request(item);
        while(!backend.try_enqueue(request,result.cycles))advance();
        if(!result.calls.count(item.call_index))result.calls[item.call_index]={
            {"call_index",item.call_index},{"requests",0},{"read_requests",0},{"write_requests",0},
            {"read_bytes",0},{"write_bytes",0},{"first_admission_cycle",result.cycles},
            {"last_admission_cycle",result.cycles},{"first_completion_cycle",nullptr},
            {"last_completion_cycle",nullptr}};
        auto& row=result.calls.at(item.call_index);
        row["requests"]=row.at("requests").get<U>()+1;
        row["last_admission_cycle"]=result.cycles;
        const bool write=item.cause==nt::Cause::DirtyWriteback;
        const char* request_key=write?"write_requests":"read_requests";
        const char* byte_key=write?"write_bytes":"read_bytes";
        row[request_key]=row.at(request_key).get<U>()+1;
        row[byte_key]=row.at(byte_key).get<U>()+item.bytes;
        if(write){++writes;write_bytes+=item.bytes;}else{++reads;read_bytes+=item.bytes;}
    }
    while(backend.queue_depth())advance();backend.finalize();
    const auto& stats=backend.statistics();const auto& admission=backend.admission_statistics();
    check(admission.accepted==records.size()&&admission.completed==records.size()&&
        admission.reserved_bursts==0&&backend.queue_depth()==0,"reference complete admission ledger");
    check(stats.fill_requests==reads&&stats.writeback_requests==writes&&
        stats.fill_completions==reads&&stats.writeback_completions==writes&&
        stats.fill_bytes==read_bytes&&stats.writeback_bytes==write_bytes&&
        stats.fill_completed_bytes==read_bytes&&stats.writeback_completed_bytes==write_bytes,
        "reference accepted/completed traffic closure");
    check(std::all_of(backend.occupied_credits().begin(),backend.occupied_credits().end(),
        [](U count){return count==0;}),"reference all credits returned");
    result.blocked=admission.blocked;result.peak_live=admission.peak_live;
    result.peak_credits=admission.peak_pseudo_channel_credits;
    result.summary={{"requests",records.size()},{"read_requests",reads},{"write_requests",writes},
        {"read_bytes",read_bytes},{"write_bytes",write_bytes},{"replay_cycles",result.cycles},
        {"request_payload_fnv1a64",admission.actual_request_shape_fnv1a64},
        {"physical",physical_stats(backend.physical_statistics())}};
    return result;
}

struct Run {
    J result;std::vector<Completion> completions;
};
Run replay(const std::vector<nt::Record>& records,const hbm::HbmConfig& cfg,
           U max_live,U credits,sg_hbf::DrainMode drain) {
    trace_replay::Replay backend(sg_hbf::Clock(500,1),cfg,max_live,credits,drain,1000000);
    Run result;
    backend.set_completion_observer([&](const g::L2DramCompletion& value,U cycle) {
        result.completions.push_back(completion_row(value,cycle));
    });
    for(const auto& item:records)backend.accept(item);
    result.result=backend.finish();
    check(result.result.at("replay_cycles")==backend.cycle(),"reported final replay frontier");
    rejects([&]{backend.finish();});rejects([&]{backend.accept(record(records.size(),0,false,999));});
    return result;
}

void same_result(const Run& actual,const Reference& expected) {
    check(actual.result.at("status")=="PASS_CLOSED_MEMORY_ONLY_REPLAY"&&
        actual.result.at("cache_replayed")==false&&
        actual.result.at("GPU_compute_executed")==false&&
        actual.result.at("GPU_stall_model_executed")==false,
        "replay status or memory-only qualification changed");
    for(const auto& field:expected.summary.items())
        check(actual.result.at(field.key())==field.value(),"replay differs from original tick reference");
    const auto count=expected.summary.at("requests");
    check(actual.result.at("accepted")==count&&actual.result.at("completed")==count&&
        actual.result.at("native_completions_posted")==count&&
        actual.result.at("native_enqueue_execution_checks")==count&&
        actual.result.at("native_enqueue_service_violations")==0&&
        actual.result.at("final_live_requests")==0&&actual.result.at("final_reserved_bursts")==0&&
        actual.result.at("byte_ledger_closed")==true&&actual.result.at("request_ledger_closed")==true,
        "replay final admission/byte/credit ledger did not close");
    check(actual.result.at("native_request_payload_fnv1a64")==
        expected.summary.at("request_payload_fnv1a64"),"native payload hash differs");
    check(actual.result.at("peak_live_requests")==expected.peak_live&&
        actual.result.at("peak_channel_burst_credits")==expected.peak_credits,
        "native live-request or credit high-water mark differs");
    check(actual.completions==expected.completions,"completion identity, order, clock or observation differs");
    check(actual.result.at("calls").size()==expected.calls.size(),"per-call group census differs");
    for(const auto& call:actual.result.at("calls")) {
        const auto& original=expected.calls.at(call.at("call_index").get<U>());
        for(const auto& field:original.items())
            check(call.at(field.key())==field.value(),"per-call traffic or timing differs");
    }
}

J fixture(const std::string& name,const std::vector<nt::Record>& records,
          const hbm::HbmConfig& cfg,U max_live,U credits,sg_hbf::DrainMode drain) {
    const auto original=tick_reference(records,cfg,max_live,credits,drain);
    const auto actual=replay(records,cfg,max_live,credits,drain);same_result(actual,original);
    check(original.peak_live<=max_live&&original.peak_credits<=credits,"finite capacity exceeded");
    return {{"name",name},{"status","PASS_EXACT_TICK_REFERENCE"},{"max_live",max_live},
        {"credits",credits},{"drain",drain==sg_hbf::DrainMode::Global?"global":"independent"},
        {"reference_blocked_attempts",original.blocked},{"peak_live",original.peak_live},
        {"peak_channel_credits",original.peak_credits},{"completion_count",actual.completions.size()},
        {"reference",original.summary},{"completion_rows",actual.completions}};
}

void invalid_tests(const hbm::HbmConfig& cfg) {
    for(unsigned variant=0;variant<11;++variant) {
        trace_replay::Replay backend(sg_hbf::Clock(500,1),cfg,2,4,sg_hbf::DrainMode::Global,1000000);
        auto bad=record(0,0,false,0);
        if(variant==0)bad.bytes=32;
        if(variant==1){bad.cause=nt::Cause::DirtyWriteback;bad.bytes=64;}
        if(variant==2)bad.service_address=1;
        if(variant==3)bad.service_address=cfg.device.capacity_bytes;
        if(variant==4)bad.source_line_address=32;
        if(variant==5)bad.request_id=1;
        if(variant==6)bad.source_sequence=1;
        if(variant==7)bad.cause=static_cast<nt::Cause>(2);
        if(variant==8)bad.issue_cycle=0;
        if(variant==9)bad.call_index=nt::unknown;
        if(variant==10)bad.service_address=UINT64_MAX-31;
        rejects([&]{backend.accept(bad);});rejects([&]{backend.finish();});
        rejects([&]{backend.accept(record(0,0,false,0));});
    }
    {
        trace_replay::Replay backend(sg_hbf::Clock(500,1),cfg,2,4);
        backend.accept(record(0,0,false,0));
        rejects([&]{backend.accept(record(0,0,false,0));});rejects([&]{backend.finish();});
    }
    {
        trace_replay::Replay backend(sg_hbf::Clock(500,1),cfg,2,4);
        backend.accept(record(0,0,false,1));
        rejects([&]{backend.accept(record(1,0,false,0));});rejects([&]{backend.finish();});
    }
    {
        trace_replay::Replay backend(sg_hbf::Clock(500,1),cfg,2,4,sg_hbf::DrainMode::Global,1);
        backend.accept(record(0,0,false,0));rejects([&]{backend.finish();});
        rejects([&]{backend.accept(record(1,0,false,0));});
    }
    {
        trace_replay::Replay backend(sg_hbf::Clock(500,1),cfg,2,4,sg_hbf::DrainMode::Global,1000000,1);
        backend.accept(record(0,0,false,0));
        rejects([&]{backend.accept(record(1,0,false,1));});rejects([&]{backend.finish();});
    }
    rejects([&]{trace_replay::Replay invalid(sg_hbf::Clock(500,1),cfg,0,4);});
    rejects([&]{trace_replay::Replay invalid(sg_hbf::Clock(500,1),cfg,2,3);});
}

J file_roundtrip(const fs::path& directory,const std::vector<nt::Record>& records,
                 const hbm::HbmConfig& cfg) {
    const auto path=(directory/"memory-only.tgn").string();
    nt::Writer writer(path,nt::Mode::FunctionalDirect,tiny_sha::sha256("trace replay CPU fixture"));
    for(const auto& item:records)writer.append(item);
    const auto receipt=writer.finish();
    trace_replay::Replay backend(sg_hbf::Clock(500,1),cfg,2,4,sg_hbf::DrainMode::Global,1000000);
    Run actual;backend.set_completion_observer([&](const g::L2DramCompletion& value,U cycle) {
        actual.completions.push_back(completion_row(value,cycle));
    });
    const auto checked=nt::validate(path,receipt.file_sha256,nt::default_max_bytes,
        [&](const nt::Record& value){backend.accept(value);});
    actual.result=backend.finish();same_result(actual,tick_reference(records,cfg,2,4,sg_hbf::DrainMode::Global));
    check(checked.records==records.size()&&actual.result.at("request_payload_fnv1a64")==
        checked.request_payload_fnv1a64,"closed trace payload not replayed exactly once");
    return receipt.to_json();
}

J run(const fs::path& directory) {
    static_assert(g::kTilegenDirtySectorMode==2,"replay tests require individual dirty32 build");
    const auto cfg=config();const auto records=mixed_records(cfg);J results=J::array();
    results.push_back(fixture("empty",{},cfg,2,4,sg_hbf::DrainMode::Global));
    hbm::HbmDevice layout(cfg);const U same=address(layout,0,0);
    std::vector<nt::Record> repeated{record(0,same,false,0),record(1,same,false,1)};
    auto cross=fixture("repeated-source-cross-call",repeated,cfg,2,4,sg_hbf::DrainMode::Global);
    const auto& physical=cross.at("reference").at("physical");
    check(physical.at("read_bytes")==256&&physical.at("activations")==1&&
        physical.at("row_hits")==7&&physical.at("row_misses")==1,
        "duplicate reads were cached/dropped or cross-call row state was reset");
    repeated[1].call_index=0;
    const auto single_call=tick_reference(repeated,cfg,2,4,sg_hbf::DrainMode::Global);
    check(cross.at("reference")==single_call.summary,"call boundary changed memory service");
    results.push_back(cross);
    for(const auto drain:{sg_hbf::DrainMode::Global,sg_hbf::DrainMode::Independent}) {
        for(U max_live:{U(1),U(2),U(8)}) {
            auto row=fixture("mixed-read128-write32",records,cfg,max_live,4,drain);
            check(row.at("reference_blocked_attempts").get<U>()>0,"fixture failed to exercise backpressure");
            check(row.at("reference").at("physical").at("row_conflicts").get<U>()>0,
                "fixture failed to exercise same-bank conflict");
            check(row.at("reference").at("physical").at("active_pseudo_channels")==2,
                "fixture failed to exercise multiple service channels");
            if(max_live==2)check(row.at("peak_live")==2,"fixture failed to fill parent capacity");
            check(row.at("peak_channel_credits")==4,"fixture failed to fill channel credits");
            results.push_back(row);
        }
    }
    invalid_tests(cfg);
    const auto closed_file=file_roundtrip(directory,records,cfg);
    return {{"status","PASS_TRACE_REPLAY_EXACT_TICK_REFERENCE"},{"checks",checks},
        {"CPU_only",true},{"GPU_or_cache_simulated",false},{"HBFSIM_microfixtures_executed",true},
        {"scope","read128/write32 ordered memory-only replay; native tick reference, no L1/L2 filtering or GPU scheduling"},
        {"test_directory",directory.string()},{"fixtures",results},
        {"closed_file_roundtrip",closed_file}};
}
} // namespace replay_test

int main(int argc,char** argv) {
    try {
        std::string pattern=(argc>1?argv[1]:"/tmp/tilegen-trace-replay-test")+std::string("-XXXXXX");
        std::vector<char> path(pattern.begin(),pattern.end());path.push_back(0);
        replay_test::check(::mkdtemp(path.data())!=nullptr,"create replay test output directory");
        std::cout<<replay_test::run(path.data()).dump(2)<<'\n';return 0;
    }catch(const std::exception& error) {
        std::cerr<<nlohmann::json({{"status","FAIL"},{"reason",error.what()}}).dump()<<'\n';return 1;
    }
}
