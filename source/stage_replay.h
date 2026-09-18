#pragma once
// Explicit stage approximation over an unchanged, already post-cache trace.
// This is not a GPU instruction/DAG simulator and never reconstructs a cache.
#include "trace_replay.h"
#include <vector>

namespace stage_replay {
using U=std::uint64_t;
using J=nlohmann::json;
namespace g=GTSim;
namespace p=hbfsim::physical;
inline constexpr U unknown=native_trace::unknown;
inline void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Stage {
    U id,call_index,record_begin,record_end,cta_begin,cta_end,compute_cycles;
};
struct Event {
    enum class Kind { StageOpened, ComputeStarted, ComputeFinished };
    Kind kind;U stage_id,call_index,cycle;
};

class Replay {
    struct State {
        Stage plan;
        U open=unknown,memory_ready=unknown,compute_start=unknown,compute_finish=unknown;
        U admitted=0,completed=0,read_requests=0,write_requests=0,read_bytes=0,write_bytes=0;
        U first_admission=unknown,last_admission=0,first_completion=unknown,last_completion=0;
    };
    struct Live { U stage,issue;bool write;g::CacheLineKey key; };
    struct Call {
        U stages=0,compute_cycles=0,requests=0,completed=0;
        U read_requests=0,write_requests=0,read_bytes=0,write_bytes=0;
        U open=unknown,compute_start=unknown,compute_finish=0;
        U first_admission=unknown,last_admission=0,first_completion=unknown,last_completion=0;
    };
    sg_hbf::Clock clock_;
    sg_hbf::HbfCreditBackend backend_;
    U window_,max_cycles_,cycle_=0,expected_records_=0;
    U next_open_=0,next_compute_=0,input_stage_=0,running_=unknown;
    U peak_active_=0,peak_metadata_=0,advance_calls_=0,safe_completion_jumps_=0,compute_event_jumps_=0;
    U compute_busy_=0,native_outstanding_=0,overlap_=0,planned_compute_=0,last_admission_=0;
    U last_record_call_=unknown,last_record_cta_=0;
    bool started_=false,finished_=false,failed_=false;
    native_trace::Ledger source_;
    std::vector<State> stages_;
    std::map<U,Live> live_;
    std::map<U,Call> calls_;
    std::function<void(const Event&)> event_observer_;
    std::function<void(const g::L2DramCompletion&,U)> completion_observer_;
    std::function<void(const native_trace::Record&,U,U)> admission_observer_;
    const std::chrono::steady_clock::time_point began_=std::chrono::steady_clock::now();

    static U integer(const J& object,const char* key) {
        require(object.contains(key),"stage profile required integer is missing");
        const auto& value=object.at(key);
        require(value.is_number_integer(),"stage profile field must be an integer");
        if(value.is_number_unsigned())return value.get<U>();
        const auto signed_value=value.get<std::int64_t>();
        require(signed_value>=0,"stage profile integer must be nonnegative");
        return U(signed_value);
    }
    static J optional_cycle(U value){return value==unknown?J(nullptr):J(value);}
    double cycles_ns(U cycles)const {
        return double(static_cast<long double>(cycles)*clock_.ps_numerator/clock_.cycle_denominator/1000.0L);
    }
    void event(Event::Kind kind,U index) {
        if(event_observer_)event_observer_({kind,index,stages_.at(index).plan.call_index,cycle_});
    }
    // Reach a fixed point at the current cycle. Memory completions are
    // delivered by advance() before this routine. Opening precedes compute
    // start, and zero-cycle compute closes without advancing the clock.
    void settle() {
        for(;;) {
            if(running_!=unknown) {
                auto& stage=stages_.at(running_);
                require(stage.compute_finish>=cycle_,"stage compute deadline was skipped");
                if(stage.compute_finish==cycle_) {
                    require(running_==next_compute_,"stage compute order changed");
                    auto& call=calls_.at(stage.plan.call_index);
                    call.compute_finish=cycle_;
                    event(Event::Kind::ComputeFinished,running_);
                    ++next_compute_;running_=unknown;
                }
            }
            while(next_open_<stages_.size()&&next_open_-next_compute_<window_) {
                auto& stage=stages_.at(next_open_);
                // A call boundary waits for every earlier stage, including
                // its compute tail. The native device is never reconstructed.
                if(next_open_&&stage.plan.call_index!=stages_.at(next_open_-1).plan.call_index&&
                   next_compute_!=next_open_)break;
                stage.open=cycle_;
                if(stage.plan.record_begin==stage.plan.record_end)stage.memory_ready=cycle_;
                auto& call=calls_.at(stage.plan.call_index);
                call.open=std::min(call.open,cycle_);
                const U index=next_open_++;
                peak_active_=std::max(peak_active_,next_open_-next_compute_);
                event(Event::Kind::StageOpened,index);
            }
            require(next_open_-next_compute_<=window_,"stage prefetch window exceeded");
            if(running_!=unknown)return;
            if(next_compute_>=next_open_)return;
            auto& stage=stages_.at(next_compute_);
            if(stage.memory_ready==unknown)return;
            require(stage.admitted==stage.plan.record_end-stage.plan.record_begin&&
                    stage.completed==stage.admitted,"stage compute started before memory completion");
            stage.compute_start=cycle_;
            stage.compute_finish=native_trace::add(cycle_,stage.plan.compute_cycles);
            require(stage.compute_finish<=max_cycles_,"stage compute exceeds cycle budget");
            auto& call=calls_.at(stage.plan.call_index);
            call.compute_start=std::min(call.compute_start,cycle_);
            running_=next_compute_;
            event(Event::Kind::ComputeStarted,running_);
            if(stage.compute_finish>cycle_)return;
        }
    }
    void start() {if(!started_){started_=true;settle();}}
    void completions(const std::vector<g::L2DramCompletion>& values) {
        for(const auto& completion:values) {
            const auto it=live_.find(completion.request_id);
            require(it!=live_.end(),"stage replay received unknown or duplicate completion");
            const auto& pending=it->second;
            require(completion.source_sequence==completion.request_id&&
                completion.issue_cycle==pending.issue&&completion.issue_time_ps==clock_.issue_ps(pending.issue)&&
                completion.completion_cycle<=cycle_&&completion.key==pending.key&&
                completion.is_writeback==pending.write,"stage replay completion identity/timing mismatch");
            auto& stage=stages_.at(pending.stage);auto& call=calls_.at(stage.plan.call_index);
            ++stage.completed;++call.completed;
            stage.first_completion=std::min(stage.first_completion,completion.completion_cycle);
            stage.last_completion=std::max(stage.last_completion,completion.completion_cycle);
            call.first_completion=std::min(call.first_completion,completion.completion_cycle);
            call.last_completion=std::max(call.last_completion,completion.completion_cycle);
            require(stage.completed<=stage.admitted,"stage completion exceeds admitted requests");
            if(stage.completed==stage.plan.record_end-stage.plan.record_begin) {
                require(stage.memory_ready==unknown,"stage memory completed twice");stage.memory_ready=cycle_;
            }
            if(completion_observer_)completion_observer_(completion,cycle_);
            live_.erase(it);
        }
        require(live_.size()==backend_.queue_depth(),"stage bounded metadata/backend lifetime mismatch");
    }
    void advance() {
        require(cycle_<max_cycles_,"stage replay cycle budget exhausted");
        U next=unknown;
        const U compute_end=running_==unknown?unknown:stages_.at(running_).compute_finish;
        const auto& admission=backend_.admission_statistics();
        const U due=backend_.next_completion_cycle();
        if(backend_.queue_depth()) {
            // Unknown native schedules may produce an earlier command or
            // credit return. Never jump merely because one completion is known.
            if(admission.accepted!=admission.native_completions_posted)next=cycle_+1;
            else {require(due!=unknown,"posted native requests lack a completion event");next=due;}
        }
        next=std::min(next,compute_end);
        require(next!=unknown&&next>cycle_,"stage replay cannot advance: missing event or deadlock");
        require(next<=max_cycles_,"stage replay event exceeds cycle budget");
        const U elapsed=next-cycle_;
        if(elapsed>1) {
            if(next==due)++safe_completion_jumps_;
            if(next==compute_end)++compute_event_jumps_;
        }
        if(running_!=unknown)compute_busy_=native_trace::add(compute_busy_,elapsed);
        if(!live_.empty())native_outstanding_=native_trace::add(native_outstanding_,elapsed);
        if(running_!=unknown&&!live_.empty())overlap_=native_trace::add(overlap_,elapsed);
        cycle_=next;++advance_calls_;
        // When empty this advances the shared frontier across compute only.
        // HbfCreditBackend requires refresh off; no hidden idle work is lost.
        completions(backend_.step(cycle_));settle();
    }

public:
    Replay(sg_hbf::Clock clock,const p::hbm::HbmConfig& config,const J& phase_profile,
           U window=2,U max_live=4096,U credits=32,sg_hbf::DrainMode drain=sg_hbf::DrainMode::Global,
           U max_cycles=2000000000ULL)
        :clock_(clock),backend_(clock,config,max_live,credits,1,drain),window_(window),max_cycles_(max_cycles) {
        require(window==1||window==2||window==4||window==8,"stage window must be 1,2,4,8");
        require(max_cycles>0&&max_cycles<=U(INT64_MAX),"stage replay requires a positive signed cycle budget");
        require(phase_profile.is_object()&&phase_profile.contains("stages")&&phase_profile.at("stages").is_array(),
                "stage profile requires a stages array");
        const auto& plans=phase_profile.at("stages");
        require(plans.size()<=65536,"stage profile exceeds 65536 stages");stages_.reserve(plans.size());
        U previous_call=0,previous_cta_end=0;
        for(const auto& row:plans) {
            require(row.is_object(),"stage row must be an object");
            Stage plan{integer(row,"id"),integer(row,"call_index"),integer(row,"record_begin"),
                integer(row,"record_end"),integer(row,"cta_begin"),integer(row,"cta_end"),integer(row,"compute_cycles")};
            require(plan.id==stages_.size(),"stage IDs must be contiguous from zero");
            require(plan.call_index!=unknown&&(stages_.empty()||plan.call_index>=previous_call),
                    "stage call identity missing or regressed");
            require(plan.record_begin==expected_records_&&plan.record_end>=plan.record_begin&&plan.record_end!=unknown,
                    "stage record ranges must be contiguous nonoverlapping half-open ranges");
            require(plan.cta_begin<plan.cta_end,"stage CTA range must be nonempty and half-open");
            require(stages_.empty()||plan.call_index!=previous_call||plan.cta_begin==previous_cta_end,
                    "same-call stage CTA ranges must be contiguous");
            require(plan.compute_cycles<=max_cycles_,"stage compute exceeds cycle budget");
            planned_compute_=native_trace::add(planned_compute_,plan.compute_cycles);
            require(planned_compute_<=max_cycles_,"serial stage compute exceeds cycle budget");
            auto& call=calls_[plan.call_index];++call.stages;
            call.compute_cycles=native_trace::add(call.compute_cycles,plan.compute_cycles);
            stages_.push_back(State{plan});expected_records_=plan.record_end;
            previous_call=plan.call_index;previous_cta_end=plan.cta_end;
        }
        if(phase_profile.contains("record_count"))
            require(integer(phase_profile,"record_count")==expected_records_,"stage profile record_count differs from ranges");
        source_.value.mode=native_trace::Mode::FunctionalDirect;
    }
    Replay(const Replay&)=delete;Replay& operator=(const Replay&)=delete;
    U cycle()const{return cycle_;}
    void set_event_observer(std::function<void(const Event&)> observer) {
        require(!started_&&!finished_&&!failed_,"stage observer must be set before input");event_observer_=std::move(observer);
    }
    void set_completion_observer(std::function<void(const g::L2DramCompletion&,U)> observer) {
        require(!started_&&!finished_&&!failed_,"stage observer must be set before input");completion_observer_=std::move(observer);
    }
    void set_admission_observer(std::function<void(const native_trace::Record&,U,U)> observer) {
        require(!started_&&!finished_&&!failed_,"stage observer must be set before input");admission_observer_=std::move(observer);
    }
    void accept(const native_trace::Record& record) {
        require(!finished_&&!failed_,"stage replay cannot accept after finish/failure");
        try {
            require(record.request_id==source_.value.records&&record.request_id<expected_records_,
                    "stage replay request ID missing, duplicated, or outside plan");
            while(input_stage_<stages_.size()&&record.request_id>=stages_.at(input_stage_).plan.record_end)++input_stage_;
            require(input_stage_<stages_.size(),"trace record has no stage");
            auto& stage=stages_.at(input_stage_);const auto& plan=stage.plan;
            require(record.request_id>=plan.record_begin&&record.call_index==plan.call_index,
                    "trace record call or range does not match stage");
            require(record.cta!=unknown&&record.cta>=plan.cta_begin&&record.cta<plan.cta_end,
                    "trace CTA missing or outside stage range");
            require(last_record_call_!=record.call_index||record.cta>=last_record_cta_,"trace CTA order regressed");
            source_.accept(record);start();
            while(input_stage_>=next_open_)advance();
            g::L2DramRequest request;
            request.request_id=record.request_id;request.source_sequence=record.source_sequence;
            request.issue_cycle=stage.open;request.issue_time_ps=clock_.issue_ps(stage.open);
            request.key={trace_replay::source_id<int>(record.source_matrix_id),record.source_line_address};
            request.address=record.service_address;request.bytes=std::uint32_t(record.bytes);
            request.node_id=trace_replay::source_id<std::int64_t>(record.node_id);
            request.sm_id=trace_replay::source_id<std::int32_t>(record.sm_id);
            request.l2_subpartition_id=trace_replay::source_id<std::int32_t>(record.l2_subpartition_id);
            const bool write=record.cause==native_trace::Cause::DirtyWriteback;
            request.cause=write?g::L2DramRequestCause::DIRTY_WRITEBACK:g::L2DramRequestCause::FILL_READ;
            while(!backend_.try_enqueue(request,cycle_))advance();
            require(live_.emplace(record.request_id,Live{input_stage_,stage.open,write,request.key}).second,
                    "duplicate stage replay live request");
            require(live_.size()<=backend_.max_live(),"stage metadata exceeded native parent credits");
            peak_metadata_=std::max(peak_metadata_,U(live_.size()));
            auto& call=calls_.at(plan.call_index);++stage.admitted;++call.requests;
            if(write){++stage.write_requests;++call.write_requests;
                stage.write_bytes=native_trace::add(stage.write_bytes,record.bytes);call.write_bytes=native_trace::add(call.write_bytes,record.bytes);}
            else{++stage.read_requests;++call.read_requests;
                stage.read_bytes=native_trace::add(stage.read_bytes,record.bytes);call.read_bytes=native_trace::add(call.read_bytes,record.bytes);}
            stage.first_admission=std::min(stage.first_admission,cycle_);stage.last_admission=cycle_;
            call.first_admission=std::min(call.first_admission,cycle_);call.last_admission=cycle_;last_admission_=cycle_;
            last_record_call_=record.call_index;last_record_cta_=record.cta;
            if(admission_observer_)admission_observer_(record,stage.open,cycle_);
        }catch(...){failed_=true;throw;}
    }
    J finish() {
        require(!finished_&&!failed_,"stage replay cannot finish twice or after failure");
        try {
            require(source_.value.records==expected_records_,"trace ended before all stage requests arrived");
            start();
            while(next_compute_<stages_.size()||backend_.queue_depth())advance();
            require(next_open_==stages_.size()&&running_==unknown&&live_.empty(),"stage replay did not close");
            backend_.finalize();
            const auto& source=source_.value;const auto& admission=backend_.admission_statistics();
            const auto& stats=backend_.statistics();const auto physical=backend_.physical_statistics();
            require(admission.accepted==source.records&&admission.completed==source.records&&
                admission.actual_request_shape_fnv1a64==source.request_payload_fnv1a64,
                "stage trace/native ordered request identity/hash differs");
            require(stats.fill_requests==source.read_requests&&stats.writeback_requests==source.write_requests&&
                stats.fill_bytes==source.read_bytes&&stats.writeback_bytes==source.write_bytes&&
                physical.read_bytes==source.read_bytes&&physical.write_bytes==source.write_bytes,
                "stage trace/native/physical byte conservation differs");
            require(compute_busy_==planned_compute_&&overlap_<=compute_busy_&&overlap_<=native_outstanding_&&
                    compute_busy_<=cycle_&&native_outstanding_<=cycle_,"stage interval accounting did not close");
            const U occupied_union=native_trace::add(compute_busy_-overlap_,native_outstanding_);
            require(occupied_union<=cycle_,"stage occupancy union exceeds makespan");
            J rows=J::array(),call_rows=J::array();U stage_requests=0,stage_read=0,stage_write=0;
            for(const auto& stage:stages_) {
                const auto& plan=stage.plan;
                require(stage.admitted==plan.record_end-plan.record_begin&&stage.completed==stage.admitted&&
                    stage.read_requests+stage.write_requests==stage.admitted&&stage.open!=unknown&&
                    stage.memory_ready!=unknown&&stage.compute_start>=stage.memory_ready&&
                    stage.compute_finish-stage.compute_start==plan.compute_cycles,"stage ledger did not close");
                stage_requests=native_trace::add(stage_requests,stage.admitted);
                stage_read=native_trace::add(stage_read,stage.read_bytes);stage_write=native_trace::add(stage_write,stage.write_bytes);
                rows.push_back({{"id",plan.id},{"stage_id",plan.id},{"call_index",plan.call_index},
                    {"record_begin",plan.record_begin},{"record_end",plan.record_end},{"cta_begin",plan.cta_begin},{"cta_end",plan.cta_end},
                    {"compute_cycles",plan.compute_cycles},{"open_cycle",stage.open},{"memory_ready_cycle",stage.memory_ready},
                    {"compute_start_cycle",stage.compute_start},{"compute_finish_cycle",stage.compute_finish},
                    {"requests",stage.admitted},{"completed",stage.completed},{"read_requests",stage.read_requests},
                    {"write_requests",stage.write_requests},{"read_bytes",stage.read_bytes},{"write_bytes",stage.write_bytes},
                    {"first_admission_cycle",optional_cycle(stage.first_admission)},
                    {"last_admission_cycle",stage.admitted?J(stage.last_admission):J(nullptr)},
                    {"first_completion_cycle",optional_cycle(stage.first_completion)},
                    {"last_completion_cycle",stage.completed?J(stage.last_completion):J(nullptr)}});
            }
            U call_requests=0,call_read=0,call_write=0;
            for(const auto& [index,call]:calls_) {
                require(call.requests==call.completed&&call.read_requests+call.write_requests==call.requests&&
                        call.open!=unknown&&call.compute_start!=unknown,"stage per-call ledger did not close");
                call_requests=native_trace::add(call_requests,call.requests);
                call_read=native_trace::add(call_read,call.read_bytes);call_write=native_trace::add(call_write,call.write_bytes);
                call_rows.push_back({{"call_index",index},{"stages",call.stages},{"compute_cycles",call.compute_cycles},
                    {"requests",call.requests},{"completed",call.completed},{"read_requests",call.read_requests},
                    {"write_requests",call.write_requests},{"read_bytes",call.read_bytes},{"write_bytes",call.write_bytes},
                    {"open_cycle",call.open},{"compute_start_cycle",call.compute_start},{"compute_finish_cycle",call.compute_finish},
                    {"makespan_cycles",call.compute_finish-call.open},{"makespan_ns",cycles_ns(call.compute_finish-call.open)},
                    {"first_admission_cycle",optional_cycle(call.first_admission)},
                    {"last_admission_cycle",call.requests?J(call.last_admission):J(nullptr)},
                    {"first_completion_cycle",optional_cycle(call.first_completion)},
                    {"last_completion_cycle",call.completed?J(call.last_completion):J(nullptr)}});
            }
            require(stage_requests==source.records&&call_requests==source.records&&stage_read==source.read_bytes&&
                call_read==source.read_bytes&&stage_write==source.write_bytes&&call_write==source.write_bytes,
                "stage/call/source aggregate ledger differs");
            const U bytes=native_trace::add(source.read_bytes,source.write_bytes);
            const double memory_span=source.records?physical.finish_ns-physical.first_arrival_ns:0.0;
            const double makespan=cycles_ns(cycle_);
            require(std::isfinite(memory_span)&&memory_span>=0&&(!source.records||memory_span>0),"stage native memory span invalid");
            const auto service=backend_.service_diagnostics();
            J out={{"schema","HBFSIM_POST_CACHE_STAGE_OVERLAP_REPLAY_V1"},{"status","PASS_CLOSED_STAGE_OVERLAP_REPLAY"},
                {"mode","stage-overlap-replay"},{"qualification","EXPLICIT_UNCALIBRATED_STAGE_APPROXIMATION"},
                {"input_mode","FUNCTIONAL_DIRECT_DETERMINISTIC_ORDER"},{"requests",source.records},
                {"read_requests",source.read_requests},{"write_requests",source.write_requests},
                {"read_bytes",source.read_bytes},{"write_bytes",source.write_bytes},
                {"request_payload_fnv1a64",source.request_payload_fnv1a64},
                {"native_request_payload_fnv1a64",admission.actual_request_shape_fnv1a64},
                {"GPU_compute_executed",false},{"GPU_stall_model_executed",false},{"stage_compute_model_applied",true},
                {"cache_replayed",false},{"fixed_direct_cache",true},{"dependency_collapsed",true},
                {"writeback_attribution","eviction trigger context, not last writer or output-store completion"},
                {"dependency_scope","all stage requests complete before compute; one compute lane in stage order; bounded prefetch stages"},
                {"kernel_barriers_enforced",true},{"kernel_barriers_applied",true},{"source_timestamps_applied",false},
                {"synthetic_issue_scope","stage opening cycle; all stage records eligible then; trace-order native credit admission"},
                {"HBFSIM_command_waits_preserved",true},{"native_target_timing_qualified",false},{"outer_GPU_memory_path_applied",false},
                {"writeback_record_bytes",32},{"read_record_bytes",128},{"window_stages",window_},{"peak_active_stages",peak_active_},
                {"clock",{{"period_ps_numerator",clock_.ps_numerator},{"period_ps_denominator",clock_.cycle_denominator}}},
                {"replay_cycles",cycle_},{"makespan_cycles",cycle_},{"makespan_ns",makespan},{"max_cycles",max_cycles_},
                {"last_admission_cycle",source.records?J(last_admission_):J(nullptr)},
                {"tail_cycles_after_last_admission",source.records?J(cycle_-last_admission_):J(nullptr)},
                {"completion_poll_time_ps",clock_.poll_ps(cycle_)},{"native_last_completion_ps",admission.last_completion_ps},
                {"physical_time_ns",memory_span},{"memory_active_span_ns",memory_span},{"physical",trace_replay::physical_json(physical)},
                {"memory_span_definition","native physical finish_ns minus first_arrival_ns; includes gaps between requests, excludes final compute tail"},
                {"bandwidth_denominator","stage makespan from cycle zero through all native completions and final compute"},
                {"bandwidth_unit","decimal GB/s = bytes / ns"},
                {"aggregate_bandwidth_GBps",makespan>0?J(double(bytes)/makespan):J(nullptr)},
                {"read_bandwidth_GBps",makespan>0?J(double(source.read_bytes)/makespan):J(nullptr)},
                {"write_bandwidth_GBps",makespan>0?J(double(source.write_bytes)/makespan):J(nullptr)},
                {"memory_span_bandwidth_GBps",memory_span>0?J(double(bytes)/memory_span):J(nullptr)},
                {"compute_busy_cycles",compute_busy_},{"native_outstanding_cycles",native_outstanding_},
                {"compute_native_outstanding_overlap_cycles",overlap_},{"compute_busy_ns",cycles_ns(compute_busy_)},
                {"native_outstanding_ns",cycles_ns(native_outstanding_)},{"compute_native_outstanding_overlap_ns",cycles_ns(overlap_)},
                {"compute_only_cycles",compute_busy_-overlap_},{"memory_only_cycles",native_outstanding_-overlap_},
                {"both_idle_cycles",cycle_-occupied_union},
                {"overlap_definition","union of half-open replay-cycle intervals with compute active and at least one admitted native request not yet delivered; proxy, not physical bus overlap"},
                {"accepted",admission.accepted},{"completed",admission.completed},
                {"native_completions_posted",admission.native_completions_posted},
                {"native_enqueue_execution_checks",admission.native_enqueue_execution_checks},
                {"native_enqueue_service_violations",admission.native_enqueue_service_violations},
                {"max_live",backend_.max_live()},{"credits_per_channel_bursts",backend_.credits_per_pc()},
                {"peak_live_requests",admission.peak_live},{"peak_live_metadata",peak_metadata_},
                {"peak_reserved_bursts",admission.peak_reserved_bursts},{"peak_channel_burst_credits",admission.peak_pseudo_channel_credits},
                {"blocked_admission_attempts",admission.blocked},{"advance_calls",advance_calls_},
                {"safe_completion_jumps",safe_completion_jumps_},{"compute_event_jumps",compute_event_jumps_},
                {"service_quantum",1},{"drain_mode",service.drain==sg_hbf::DrainMode::Global?"global":"independent"},
                {"independent_fast_drains",service.independent_fast_drains},{"fallback_global_drains",service.fallback_global_drains},
                {"stages",std::move(rows)},{"calls",std::move(call_rows)},
                {"call_time_scope","explicit stage approximation with serial cross-call barriers; not measured GPU kernel latency"},
                {"final_live_requests",0},{"final_reserved_bursts",0},{"final_active_stages",0},
                {"byte_ledger_closed",true},{"request_ledger_closed",true},{"stage_ledger_closed",true},
                {"host_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-began_).count()}};
            finished_=true;return out;
        }catch(...){failed_=true;throw;}
    }
};
} // namespace stage_replay
