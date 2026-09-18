#pragma once
// The input is already post-cache. Replay never constructs a cache, GPU
// scheduler, source program, or outer GPU request/response path.
#include "native_trace.h"
#include "work/tilegen-hbf-drain-native-r1/native_backend.h"
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <type_traits>

namespace trace_replay {
using U=std::uint64_t;
using J=nlohmann::json;
namespace g=GTSim;
namespace p=hbfsim::physical;
inline void require(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}
template<class Signed> inline Signed source_id(U value) {
    static_assert(std::is_integral_v<Signed>&&std::is_signed_v<Signed>);
    if(value==native_trace::unknown)return Signed(-1);
    require(value<=U(std::numeric_limits<Signed>::max()),"replay source ID exceeds signed native field");
    return static_cast<Signed>(value);
}
inline J physical_json(const p::hbm::HbmStats& stats) {
    J out;
#define FIELD(x) out[#x]=stats.x
    FIELD(read_bytes);FIELD(write_bytes);FIELD(row_hits);FIELD(row_misses);FIELD(row_conflicts);
    FIELD(controller_buffer_read_bytes);FIELD(controller_buffer_write_bytes);
    FIELD(controller_buffer_transfers);FIELD(controller_buffer_bus_busy_ns);
    FIELD(activations);FIELD(precharges);FIELD(refresh_count);FIELD(bus_busy_ns);FIELD(finish_ns);
    FIELD(pseudo_channels);FIELD(active_pseudo_channels);FIELD(max_pseudo_channel_accesses);
    FIELD(max_queue_occupancy);FIELD(max_pseudo_channel_busy_ns);FIELD(avg_active_pseudo_channel_busy_ns);
    FIELD(replicated_requests);FIELD(replicated_bursts);
#undef FIELD
    out["first_arrival_ns"]=std::isfinite(stats.first_arrival_ns)?J(stats.first_arrival_ns):J(nullptr);
    J work=J::object();
#define WORK(x) work[#x]=stats.stage_work.x
    WORK(ingress_queue_wait_ns);WORK(scheduler_queue_wait_ns);WORK(address_mapping_ns);WORK(translation_ns);
    WORK(mapping_dram_ns);WORK(write_buffer_dram_ns);WORK(refresh_stall_ns);WORK(precharge_ns);WORK(activation_ns);
    WORK(command_ns);WORK(array_read_ns);WORK(array_program_ns);WORK(array_erase_ns);WORK(media_lane_transfer_ns);
    WORK(page_buffer_ns);WORK(sram_staging_ns);WORK(channel_transfer_ns);WORK(tsv_transfer_ns);WORK(hb_io_transfer_ns);
    WORK(transport_latency_ns);WORK(ecc_queue_wait_ns);WORK(ecc_latency_ns);WORK(maintenance_ns);
#undef WORK
    out["overlapping_stage_work_ns"]=std::move(work);
    return out;
}
class Replay {
    struct Live { U call,bytes;bool write;g::CacheLineKey key; };
    struct Call {
        U requests=0,read_requests=0,write_requests=0,read_bytes=0,write_bytes=0,completed=0;
        U first_admission=native_trace::unknown,last_admission=0;
        U first_completion=native_trace::unknown,last_completion=0;
    };
    sg_hbf::Clock clock_;
    sg_hbf::HbfCreditBackend backend_;
    U max_cycles_,max_call_groups_,cycle_=0,advance_calls_=0,safe_completion_jumps_=0;
    U peak_metadata_=0;
    bool finished_=false,failed_=false;
    native_trace::Ledger source_;
    std::map<U,Live> live_;
    std::map<U,Call> calls_;
    std::function<void(const g::L2DramCompletion&,U)> completion_observer_;
    const std::chrono::steady_clock::time_point began_=std::chrono::steady_clock::now();

    void completions(const std::vector<g::L2DramCompletion>& values) {
        for(const auto& completion:values) {
            const auto it=live_.find(completion.request_id);
            require(it!=live_.end(),"replay received unknown or duplicate completion");
            const auto& pending=it->second;
            require(completion.source_sequence==completion.request_id&&
                completion.issue_cycle==0&&completion.issue_time_ps==0&&
                completion.completion_cycle<=cycle_&&completion.key==pending.key&&
                completion.is_writeback==pending.write,"replay completion identity/timing mismatch");
            auto& call=calls_.at(pending.call);
            ++call.completed;
            call.first_completion=std::min(call.first_completion,completion.completion_cycle);
            call.last_completion=std::max(call.last_completion,completion.completion_cycle);
            if(completion_observer_)completion_observer_(completion,cycle_);
            live_.erase(it);
        }
        require(live_.size()==backend_.queue_depth(),"replay bounded metadata/backend lifetime mismatch");
    }
    void advance() {
        require(cycle_<max_cycles_,"memory-only replay cycle budget exhausted");
        U next=cycle_+1;
        const auto& admission=backend_.admission_statistics();
        // A known due completion alone is NOT a safe jump target: an unposted
        // parent may issue commands and free a credit before it. Jump only
        // after every admitted parent has a fixed native completion schedule.
        const U due=backend_.next_completion_cycle();
        if(admission.accepted==admission.native_completions_posted&&
           due!=native_trace::unknown&&due>next) {
            next=due;++safe_completion_jumps_;
        }
        require(next<=max_cycles_,"memory-only replay completion exceeds cycle budget");
        cycle_=next;++advance_calls_;completions(backend_.step(cycle_));
    }

public:
    Replay(sg_hbf::Clock clock,const p::hbm::HbmConfig& config,
           U max_live=4096,U credits=32,sg_hbf::DrainMode drain=sg_hbf::DrainMode::Global,
           U max_cycles=2000000000ULL,U max_call_groups=65536)
        :clock_(clock),backend_(clock,config,max_live,credits,1,drain),
         max_cycles_(max_cycles),max_call_groups_(max_call_groups) {
        require(max_cycles>0&&max_cycles<=U(INT64_MAX),"replay requires positive signed cycle budget");
        require(max_call_groups>0&&max_call_groups<=65536,"replay call metadata cap must be1..65536");
        source_.value.mode=native_trace::Mode::FunctionalDirect;
    }
    Replay(const Replay&)=delete;
    Replay& operator=(const Replay&)=delete;
    U cycle() const {return cycle_;}
    void set_completion_observer(std::function<void(const g::L2DramCompletion&,U)> observer) {
        require(!finished_&&!failed_&&source_.value.records==0,"replay observer must be set before first input");
        completion_observer_=std::move(observer);
    }
    void accept(const native_trace::Record& record) {
        require(!finished_&&!failed_,"replay cannot accept after finish/failure");
        try {
            // Retain source IDs and bytes exactly. Source timestamps are
            // intentionally absent: all trace entries are offered at t=0.
            source_.accept(record);
            g::L2DramRequest request;
            request.request_id=record.request_id;request.source_sequence=record.source_sequence;
            request.issue_cycle=0;request.issue_time_ps=0;
            request.key={source_id<int>(record.source_matrix_id),record.source_line_address};
            request.address=record.service_address;request.bytes=std::uint32_t(record.bytes);
            request.node_id=source_id<std::int64_t>(record.node_id);
            request.sm_id=source_id<std::int32_t>(record.sm_id);
            request.l2_subpartition_id=source_id<std::int32_t>(record.l2_subpartition_id);
            const bool write=record.cause==native_trace::Cause::DirtyWriteback;
            request.cause=write?g::L2DramRequestCause::DIRTY_WRITEBACK:g::L2DramRequestCause::FILL_READ;
            if(!calls_.contains(record.call_index)) {
                require(calls_.size()<max_call_groups_,"replay per-call metadata limit exceeded");
                calls_.emplace(record.call_index,Call{});
            }
            // Head-of-line admission is intentional: a blocked trace record
            // cannot be bypassed by later records targeting another channel.
            while(!backend_.try_enqueue(request,cycle_))advance();
            require(live_.emplace(record.request_id,Live{record.call_index,record.bytes,write,request.key}).second,
                    "duplicate replay live request ID");
            require(live_.size()<=backend_.max_live(),"replay metadata exceeded native parent credits");
            peak_metadata_=std::max(peak_metadata_,U(live_.size()));
            auto& call=calls_.at(record.call_index);++call.requests;
            if(write){++call.write_requests;call.write_bytes=native_trace::add(call.write_bytes,record.bytes);}
            else{++call.read_requests;call.read_bytes=native_trace::add(call.read_bytes,record.bytes);}
            call.first_admission=std::min(call.first_admission,cycle_);call.last_admission=cycle_;
        } catch(...) {failed_=true;throw;}
    }
    J finish() {
        require(!finished_&&!failed_,"replay cannot finish twice or after failure");
        try {
            const U final_admission_cycle=cycle_;
            while(backend_.queue_depth())advance();
            require(live_.empty(),"replay metadata not drained");backend_.finalize();
            const auto& source=source_.value;const auto& admission=backend_.admission_statistics();
            const auto& stats=backend_.statistics();const auto physical=backend_.physical_statistics();
            require(admission.accepted==source.records&&admission.completed==source.records&&
                admission.actual_request_shape_fnv1a64==source.request_payload_fnv1a64,
                "trace/native ordered request identity/hash differs");
            require(stats.fill_requests==source.read_requests&&stats.writeback_requests==source.write_requests&&
                stats.fill_bytes==source.read_bytes&&stats.writeback_bytes==source.write_bytes&&
                physical.read_bytes==source.read_bytes&&physical.write_bytes==source.write_bytes,
                "trace/native/physical request byte conservation differs");
            const U bytes=native_trace::add(source.read_bytes,source.write_bytes);
            const double duration=source.records?physical.finish_ns-physical.first_arrival_ns:0.0;
            require(std::isfinite(duration)&&duration>=0&&(!source.records||duration>0),
                    "replay native physical time span invalid");
            J rows=J::array();U call_requests=0,call_read=0,call_write=0;
            for(const auto& [index,call]:calls_) {
                require(call.requests==call.completed&&call.read_requests+call.write_requests==call.requests,
                        "replay per-call request completion did not close");
                call_requests=native_trace::add(call_requests,call.requests);
                call_read=native_trace::add(call_read,call.read_bytes);call_write=native_trace::add(call_write,call.write_bytes);
                rows.push_back({{"call_index",index},{"requests",call.requests},{"completed",call.completed},
                    {"read_requests",call.read_requests},{"write_requests",call.write_requests},
                    {"read_bytes",call.read_bytes},{"write_bytes",call.write_bytes},
                    {"first_admission_cycle",call.first_admission},{"last_admission_cycle",call.last_admission},
                    {"first_completion_cycle",call.first_completion},{"last_completion_cycle",call.last_completion},
                    {"first_admission_ps",clock_.issue_ps(call.first_admission)},
                    {"last_admission_ps",clock_.issue_ps(call.last_admission)},
                    {"first_completion_poll_ps",clock_.poll_ps(call.first_completion)},
                    {"last_completion_poll_ps",clock_.poll_ps(call.last_completion)}});
            }
            require(call_requests==source.records&&call_read==source.read_bytes&&call_write==source.write_bytes,
                    "replay per-call aggregate byte/request conservation differs");
            const auto service=backend_.service_diagnostics();
            J out={{"schema","HBFSIM_POST_CACHE_MEMORY_ONLY_REPLAY_V1"},{"status","PASS_CLOSED_MEMORY_ONLY_REPLAY"},
                {"mode","memory-only-replay"},{"input_mode","FUNCTIONAL_DIRECT_DETERMINISTIC_ORDER"},
                {"requests",source.records},{"read_requests",source.read_requests},{"write_requests",source.write_requests},
                {"read_bytes",source.read_bytes},{"write_bytes",source.write_bytes},
                {"request_payload_fnv1a64",source.request_payload_fnv1a64},
                {"native_request_payload_fnv1a64",admission.actual_request_shape_fnv1a64},
                {"GPU_compute_executed",false},{"GPU_stall_model_executed",false},{"cache_replayed",false},
                {"kernel_barriers_applied",false},{"source_timestamps_applied",false},
                {"injection_policy","all records ready at synthetic t0; trace-order ASAP refill with head-of-line native credits"},
                {"HBFSIM_command_waits_preserved",true},{"native_target_timing_qualified",false},
                {"outer_GPU_memory_path_applied",false},{"writeback_record_bytes",32},{"read_record_bytes",128},
                {"clock",{{"period_ps_numerator",clock_.ps_numerator},{"period_ps_denominator",clock_.cycle_denominator}}},
                {"replay_cycles",cycle_},{"max_cycles",max_cycles_},{"last_admission_cycle",final_admission_cycle},
                {"drain_cycles_after_last_admission",cycle_-final_admission_cycle},
                {"completion_poll_time_ps",clock_.poll_ps(cycle_)},
                {"native_last_completion_ps",admission.last_completion_ps},
                {"physical_time_ns",duration},{"physical",physical_json(physical)},
                {"bandwidth_denominator","native physical finish_ns minus first_arrival_ns; includes final drain"},
                {"bandwidth_unit","decimal GB/s = bytes / ns"},
                {"aggregate_bandwidth_GBps",duration>0?J(double(bytes)/duration):J(nullptr)},
                {"read_bandwidth_GBps",duration>0?J(double(source.read_bytes)/duration):J(nullptr)},
                {"write_bandwidth_GBps",duration>0?J(double(source.write_bytes)/duration):J(nullptr)},
                {"accepted",admission.accepted},{"completed",admission.completed},
                {"native_completions_posted",admission.native_completions_posted},
                {"native_enqueue_execution_checks",admission.native_enqueue_execution_checks},
                {"native_enqueue_service_violations",admission.native_enqueue_service_violations},
                {"max_live",backend_.max_live()},{"credits_per_channel_bursts",backend_.credits_per_pc()},
                {"peak_live_requests",admission.peak_live},{"peak_live_metadata",peak_metadata_},
                {"peak_reserved_bursts",admission.peak_reserved_bursts},
                {"peak_channel_burst_credits",admission.peak_pseudo_channel_credits},
                {"blocked_admission_attempts",admission.blocked},
                {"advance_calls",advance_calls_},{"safe_completion_jumps",safe_completion_jumps_},
                {"service_quantum",1},{"drain_mode",service.drain==sg_hbf::DrainMode::Global?"global":"independent"},
                {"independent_fast_drains",service.independent_fast_drains},
                {"fallback_global_drains",service.fallback_global_drains},
                {"calls",rows},{"call_time_scope","synthetic memory admission/completion only; overlapping groups, not kernel latency"},
                {"final_live_requests",0},{"final_reserved_bursts",0},{"byte_ledger_closed",true},
                {"request_ledger_closed",true},{"host_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-began_).count()}};
            finished_=true;return out;
        } catch(...) {failed_=true;throw;}
    }
};
} // namespace trace_replay
