#pragma once
// Diagnostic outer GPU memory path. The selected inner backend owns native
// command service and burst credits; this layer retains original GPU delivery.
#include "../../tilegen-hbf-drain-native-r1/native_backend.h"
#include "../../../native_trace.h"
#include <array>
#include <deque>

namespace sg_hbf {
inline U checked_add(U a,U b){need(b<=UINT64_MAX-a,"memory path time/counter overflow");return a+b;}
inline U checked_difference(U later,U earlier){need(later>=earlier,"memory path segment time regressed");return later-earlier;}
inline constexpr std::array<const char*,11> kDeliveredSegmentNames={
    "outer_admission_wait","request_queue_wait","request_service","request_fixed",
    "native_admission_wait","physical_core_ready_latency","physical_observation",
    "response_queue_wait","response_service","response_fixed","delivery_poll_lag"};
struct PathRate {
    bool limited=false;
    U numerator=1,denominator=1;
    U duration(U units)const{
        need(numerator>0&&denominator>0,"positive memory path rate required");
        return limited?ratio(units,denominator,numerator,true):0;
    }
};
struct PathConfig {
    U request_fixed_ps=0,response_fixed_ps=0,max_outer_live=4096;
    PathRate request_rate,response_rate;
    void validate()const{
        need(max_outer_live>0&&max_outer_live<=4096,"bounded memory path outer capacity required");
        need(request_fixed_ps<=1000000000&&response_fixed_ps<=1000000000,"diagnostic path delay exceeds 1ms bound");
        need(request_rate.numerator>0&&request_rate.denominator>0&&response_rate.numerator>0&&response_rate.denominator>0,
             "positive explicit path service rate required");
        need(request_rate.duration(1)<=1000000000&&response_rate.duration(128)<=1000000000,
             "diagnostic path service quantum exceeds 1ms bound");
    }
    bool direct_request()const{return request_fixed_ps==0&&!request_rate.limited;}
};
struct PathStatistics {
    U outer_accepted=0,outer_delivered=0,outer_rejected=0,physical_admitted=0,physical_completed=0;
    U peak_outer_live=0,peak_ingress=0,peak_responses=0,peak_physical_live=0;
    U request_service_busy_ps=0,response_service_busy_ps=0,response_payload_bytes=0;
    U outer_admission_wait_ps=0,request_queue_wait_ps=0,request_fixed_work_ps=0;
    U request_service_work_ps=0,native_admission_wait_ps=0;
    U physical_core_ready_latency_ps=0,physical_observation_lag_ps=0;
    U physical_observation_segment_ps=0;
    U response_queue_wait_ps=0,response_fixed_work_ps=0,response_service_work_ps=0;
    U delivery_poll_lag_ps=0,end_to_end_observed_latency_ps=0;
    U first_issue_ps=UINT64_MAX,first_native_arrival_ps=UINT64_MAX;
    U last_native_arrival_ps=0,last_physical_core_ready_ps=0,last_response_ready_ps=0,last_delivery_ps=0;
    U physical_release_while_response_pending=0,zero_path_direct_completions=0,step_calls=0;
    U outer_identity_fnv1a64=14695981039346656037ULL,inner_binding_fnv1a64=14695981039346656037ULL;
    std::array<U,11> delivered_segments_ps{};
    U delivered_segment_sum_ps=0,delivered_parent_segment_closures=0;
};

class MemoryPathBackend final:public g::L2DramCompletionBackend {
    enum class Phase {INGRESS,PHYSICAL,RESPONSE};
    struct Live {
        g::L2DramRequest original;
        Phase phase=Phase::INGRESS;
        U accepted_ps=0,request_ready_ps=0,inner_id=UINT64_MAX,native_arrival_ps=0;
        U physical_ready_ps=0,response_ready_ps=0,physical_observed_ps=0;
    };
    Clock clock_;
    PathConfig path_;
    HbfCreditBackend inner_;
    std::map<U,Live> live_;
    std::deque<U> ingress_;
    std::map<U,U> inner_to_outer_;
    std::map<std::pair<U,U>,U> response_due_;
    g::L2DramRuntimeStatistics stats_;
    PathStatistics path_stats_;
    U next_inner_=0,last_issue_=0,last_call_=0,request_available_ps_=0,response_available_ps_=0;
    U native_retry_delay_ps_=0;
    bool called_=false,failed_=false;
    native_trace::Writer* trace_sink_=nullptr;
    U trace_context_=native_trace::unknown;
    static bool write(const g::L2DramRequest& r){
        need(r.cause==g::L2DramRequestCause::FILL_READ||r.cause==g::L2DramRequestCause::DIRTY_WRITEBACK,"unknown outer memory cause");
        return r.cause==g::L2DramRequestCause::DIRTY_WRITEBACK;
    }
    static void add(U& total,U value){total=checked_add(total,value);}
    static void hash(U& h,U value){for(int n=0;n<8;++n){h^=(value>>(n*8))&255;h*=1099511628211ULL;}}
    void time_call(U cycle){
        need(!failed_&&cycle<=INT64_MAX,"failed path or invalid native cycle");
        need(!called_||cycle>=last_call_,"memory path call time regressed");
        called_=true;last_call_=cycle;
    }
    void validate_request(const g::L2DramRequest& r,U cycle)const{
        need(path_stats_.outer_accepted<UINT64_MAX&&r.request_id==path_stats_.outer_accepted&&r.source_sequence==r.request_id,
             "noncontiguous outer identity or duplicate request");
        need(r.issue_cycle<=cycle&&r.issue_time_ps==clock_.issue_ps(r.issue_cycle),"outer issue time differs");
        need(path_stats_.outer_accepted==0||r.issue_cycle>=last_issue_,"outer source issue order regressed");
        (void)write(r);
        const auto& cfg=inner_.native_config();
        need(HbfCreditBackend::valid_native_request_shape(r)&&r.address<cfg.device.capacity_bytes&&r.bytes<=cfg.device.capacity_bytes-r.address,
             "outer request must be one in-capacity read128B or aligned dirty-sector run");
    }
    g::L2DramRequest inner_request(const g::L2DramRequest& original,U cycle)const{
        auto r=original;r.request_id=next_inner_;r.source_sequence=next_inner_;
        r.issue_cycle=cycle;r.issue_time_ps=clock_.issue_ps(cycle);return r;
    }
    void bind(Live& x,U cycle){
        x.phase=Phase::PHYSICAL;x.inner_id=next_inner_;x.native_arrival_ps=clock_.issue_ps(cycle);
        need(x.native_arrival_ps>=x.request_ready_ps,"native admission precedes request readiness");
        need(inner_to_outer_.emplace(next_inner_,x.original.request_id).second,"duplicate inner binding");
        hash(path_stats_.inner_binding_fnv1a64,next_inner_);hash(path_stats_.inner_binding_fnv1a64,x.original.request_id);
        hash(path_stats_.inner_binding_fnv1a64,x.original.issue_time_ps);hash(path_stats_.inner_binding_fnv1a64,x.native_arrival_ps);
        ++next_inner_;++path_stats_.physical_admitted;
        path_stats_.first_native_arrival_ps=std::min(path_stats_.first_native_arrival_ps,x.native_arrival_ps);
        path_stats_.last_native_arrival_ps=x.native_arrival_ps;
        add(path_stats_.native_admission_wait_ps,x.native_arrival_ps-x.request_ready_ps);
        add(native_retry_delay_ps_,x.native_arrival_ps-x.original.issue_time_ps);
        path_stats_.peak_physical_live=std::max<U>(path_stats_.peak_physical_live,inner_to_outer_.size());
        if(trace_sink_){
            // This point is reached exactly once after successful physical
            // admission. inner_request() rewrites issue time, so retain original.
            const auto& r=x.original;native_trace::Record record;
            record.request_id=r.request_id;record.source_sequence=r.source_sequence;
            record.issue_cycle=r.issue_cycle;record.issue_ps=r.issue_time_ps;
            record.admission_cycle=cycle;record.admission_ps=x.native_arrival_ps;
            record.source_matrix_id=static_cast<U>(r.key.matrix_id);record.source_line_address=r.key.line_addr;
            record.service_address=r.address;record.bytes=r.bytes;
            record.node_id=static_cast<U>(r.node_id);record.sm_id=static_cast<U>(r.sm_id);
            record.l2_subpartition_id=static_cast<U>(r.l2_subpartition_id);
            record.cause=write(r)?native_trace::Cause::DirtyWriteback:native_trace::Cause::ReadFillOrRfo;
            record.call_index=trace_context_;trace_sink_->append(record);
        }
    }
    void admit_ingress(U cycle){
        const U through=clock_.poll_ps(cycle);
        while(!ingress_.empty()){
            auto& x=live_.at(ingress_.front());need(x.phase==Phase::INGRESS,"ingress phase differs");
            if(x.request_ready_ps>through)break;
            if(!inner_.try_enqueue(inner_request(x.original,cycle),cycle))break;
            bind(x,cycle);ingress_.pop_front();
        }
    }
    g::L2DramCompletion deliver(const Live& x,U ready_cycle,U current_cycle){
        const auto& r=x.original;
        need(ready_cycle<=current_cycle&&ready_cycle>=r.issue_cycle,"outer delivery before readiness");
        const U observed=clock_.poll_ps(current_cycle);
        need(observed>=x.response_ready_ps&&observed>=r.issue_time_ps,"outer completion past poll boundary");
        const U request_service=path_.request_rate.duration(1);
        const U request_start=checked_difference(x.request_ready_ps,checked_add(request_service,path_.request_fixed_ps));
        const bool direct_response=path_.response_fixed_ps==0&&!path_.response_rate.limited;
        const U response_service=path_.response_rate.duration(write(r)?0:r.bytes);
        const U response_start=checked_difference(x.response_ready_ps,checked_add(response_service,path_.response_fixed_ps));
        // Delivered-only disjoint accounting: in zero-return mode the delayed
        // observation is owned by delivery_poll_lag, never counted twice.
        const std::array<U,11> segments={
            checked_difference(x.accepted_ps,r.issue_time_ps),
            checked_difference(request_start,x.accepted_ps),request_service,path_.request_fixed_ps,
            checked_difference(x.native_arrival_ps,x.request_ready_ps),
            checked_difference(x.physical_ready_ps,x.native_arrival_ps),
            direct_response?0:checked_difference(x.physical_observed_ps,x.physical_ready_ps),
            direct_response?0:checked_difference(response_start,x.physical_observed_ps),
            response_service,path_.response_fixed_ps,checked_difference(observed,x.response_ready_ps)};
        U total=0;for(std::size_t i=0;i<segments.size();++i){
            total=checked_add(total,segments[i]);add(path_stats_.delivered_segments_ps[i],segments[i]);
        }
        need(total==observed-r.issue_time_ps,"per-parent additive end-to-end segments do not close");
        add(path_stats_.delivered_segment_sum_ps,total);++path_stats_.delivered_parent_segment_closures;
        add(path_stats_.delivery_poll_lag_ps,observed-x.response_ready_ps);
        add(path_stats_.end_to_end_observed_latency_ps,observed-r.issue_time_ps);
        need(path_stats_.delivered_segment_sum_ps==path_stats_.end_to_end_observed_latency_ps,
             "delivered aggregate end-to-end segments do not close");
        path_stats_.last_delivery_ps=observed;++path_stats_.outer_delivered;
        if(write(r)) { ++stats_.writeback_completions; add(stats_.writeback_completed_bytes,r.bytes); }
        else { ++stats_.fill_completions; add(stats_.fill_completed_bytes,r.bytes); }
        return {r.request_id,r.source_sequence,r.issue_cycle,r.issue_time_ps,ready_cycle,r.key,write(r)};
    }
    void consume_physical(const g::L2DramCompletion& c,U cycle,std::vector<g::L2DramCompletion>& out){
        const auto binding=inner_to_outer_.find(c.request_id);need(binding!=inner_to_outer_.end(),"unknown physical inner completion");
        auto it=live_.find(binding->second);need(it!=live_.end(),"physical parent missing outer lifetime");
        auto& x=it->second;const auto& r=x.original;
        need(x.phase==Phase::PHYSICAL&&c.source_sequence==x.inner_id&&c.key==r.key&&c.is_writeback==write(r)&&
             c.issue_time_ps==x.native_arrival_ps&&c.completion_cycle<=cycle,"inner completion/binding differs");
        // Inner already released its native credits at this completion. It
        // reports a safe core-ready cycle, not a per-parent raw device ns.
        x.physical_ready_ps=clock_.poll_ps(c.completion_cycle);
        const U observed=clock_.poll_ps(cycle);
        need(x.physical_ready_ps>=x.native_arrival_ps&&observed>=x.physical_ready_ps,"physical clock causality differs");
        add(path_stats_.physical_core_ready_latency_ps,x.physical_ready_ps-x.native_arrival_ps);
        add(path_stats_.physical_observation_lag_ps,observed-x.physical_ready_ps);
        x.physical_observed_ps=observed;
        path_stats_.last_physical_core_ready_ps=std::max(path_stats_.last_physical_core_ready_ps,x.physical_ready_ps);
        ++path_stats_.physical_completed;inner_to_outer_.erase(binding);
        const U payload=write(r)?0:r.bytes;
        add(path_stats_.response_payload_bytes,payload);
        if(path_.response_fixed_ps==0&&!path_.response_rate.limited){
            x.response_ready_ps=x.physical_ready_ps;
            path_stats_.last_response_ready_ps=std::max(path_stats_.last_response_ready_ps,x.response_ready_ps);
            ++path_stats_.zero_path_direct_completions;
            out.push_back(deliver(x,c.completion_cycle,cycle));live_.erase(it);return;
        }
        // No future-resource reservation before physical finish is observed.
        // Writeback ACK has no invented payload or payload-serializer demand.
        const U start=payload?std::max(observed,response_available_ps_):observed;
        add(path_stats_.physical_observation_segment_ps,observed-x.physical_ready_ps);
        const U service=path_.response_rate.duration(payload);
        const U end=checked_add(start,service);
        if(payload)response_available_ps_=end;
        x.response_ready_ps=checked_add(end,path_.response_fixed_ps);x.phase=Phase::RESPONSE;
        add(path_stats_.response_queue_wait_ps,start-observed);add(path_stats_.response_service_work_ps,service);
        add(path_stats_.response_service_busy_ps,service);add(path_stats_.response_fixed_work_ps,path_.response_fixed_ps);
        path_stats_.last_response_ready_ps=std::max(path_stats_.last_response_ready_ps,x.response_ready_ps);
        need(response_due_.emplace(std::make_pair(x.response_ready_ps,r.source_sequence),r.request_id).second,"duplicate response event");
        if(x.response_ready_ps>observed)++path_stats_.physical_release_while_response_pending;
        path_stats_.peak_responses=std::max<U>(path_stats_.peak_responses,response_due_.size());
    }
public:
    MemoryPathBackend(Clock clock,p::hbm::HbmConfig config,PathConfig path,U physical_max_live=4096,U credits_per_pc=32,NativeServicePolicy service_policy={}):
        clock_(clock),path_(path),inner_(clock,config,physical_max_live,credits_per_pc,service_policy.quantum,service_policy.drain){path_.validate();}
    void set_trace_sink(native_trace::Writer* sink){
        need(!failed_&&path_stats_.outer_accepted==0,"trace sink must be selected before first request");
        need(!sink||(sink->mode()==native_trace::Mode::NativeCosim&&sink->count()==0),"native backend requires fresh native trace sink");
        trace_sink_=sink;
    }
    void set_trace_context(U call_index){
        need(!failed_&&call_index!=native_trace::unknown,"valid trace kernel context required");
        need(live_.empty()&&ingress_.empty()&&inner_to_outer_.empty()&&response_due_.empty()&&inner_.queue_depth()==0,
             "trace context requires drained kernel boundary");
        need(trace_context_==native_trace::unknown||call_index>=trace_context_,"trace kernel context regressed");
        trace_context_=call_index;
    }
    U issue_cycle_to_ps(U cycle)const override{return clock_.issue_ps(cycle);}
    void enqueue(const g::L2DramRequest& r)override{need(try_enqueue(r,r.issue_cycle),"outer legacy enqueue requires retries");}
    bool try_enqueue(const g::L2DramRequest& r,U cycle)override{
        try{
            // A rejected call must not advance even the wrapper's call frontier.
            need(!failed_&&cycle<=INT64_MAX&&(!called_||cycle>=last_call_),"failed path or regressed enqueue time");
            validate_request(r,cycle);
            if(live_.size()>=path_.max_outer_live){++path_stats_.outer_rejected;return false;}
            Live x;x.original=r;x.accepted_ps=clock_.issue_ps(cycle);
            U service=0,start=x.accepted_ps,end=start;
            if(path_.direct_request()){
                x.request_ready_ps=x.accepted_ps;
                // Preserve frozen zero-request-path physical backpressure.
                if(!inner_.try_enqueue(inner_request(r,cycle),cycle)){++path_stats_.outer_rejected;return false;}
            }else{
                start=std::max(x.accepted_ps,request_available_ps_);service=path_.request_rate.duration(1);
                end=checked_add(start,service);x.request_ready_ps=checked_add(end,path_.request_fixed_ps);
            }
            time_call(cycle);last_issue_=r.issue_cycle;
            auto inserted=live_.emplace(r.request_id,std::move(x));need(inserted.second,"outer duplicate lifetime");
            ++path_stats_.outer_accepted;
            if(write(r)) { ++stats_.writeback_requests; add(stats_.writeback_bytes,r.bytes); }
            else { ++stats_.fill_requests; add(stats_.fill_bytes,r.bytes); }
            hash(path_stats_.outer_identity_fnv1a64,r.request_id);hash(path_stats_.outer_identity_fnv1a64,r.issue_time_ps);
            hash(path_stats_.outer_identity_fnv1a64,r.address);hash(path_stats_.outer_identity_fnv1a64,write(r));
            auto& accepted=inserted.first->second;
            path_stats_.first_issue_ps=std::min(path_stats_.first_issue_ps,r.issue_time_ps);
            add(path_stats_.outer_admission_wait_ps,accepted.accepted_ps-r.issue_time_ps);
            add(path_stats_.request_queue_wait_ps,start-accepted.accepted_ps);
            add(path_stats_.request_service_work_ps,service);add(path_stats_.request_service_busy_ps,service);
            add(path_stats_.request_fixed_work_ps,path_.request_fixed_ps);
            if(path_.direct_request())bind(accepted,cycle);
            else{request_available_ps_=end;ingress_.push_back(r.request_id);path_stats_.peak_ingress=std::max<U>(path_stats_.peak_ingress,ingress_.size());}
            path_stats_.peak_outer_live=std::max<U>(path_stats_.peak_outer_live,live_.size());
            stats_.peak_queue_depth=path_stats_.peak_outer_live;return true;
        }catch(...){failed_=true;throw;}
    }
    std::vector<g::L2DramCompletion> step(U cycle)override{
        try{
            time_call(cycle);++path_stats_.step_calls;
            // Native admissions at the current cycle cannot execute before
            // strict current frontier. This order also preserves zero path.
            admit_ingress(cycle);
            std::vector<g::L2DramCompletion> out;
            for(const auto& c:inner_.step(cycle))consume_physical(c,cycle,out);
            admit_ingress(cycle); // use just-released native credits, no service
            const U through=clock_.poll_ps(cycle);
            while(!response_due_.empty()&&response_due_.begin()->first.first<=through){
                const auto event=response_due_.begin();auto it=live_.find(event->second);
                need(it!=live_.end()&&it->second.phase==Phase::RESPONSE,"response lifetime differs");
                out.push_back(deliver(it->second,clock_.completion_cycle(it->second.response_ready_ps),cycle));
                live_.erase(it);response_due_.erase(event);
            }
            return out;
        }catch(...){failed_=true;throw;}
    }
    void require_epoch_policy() const {
        const auto policy=inner_.service_diagnostics();
        need(policy.drain==DrainMode::Independent && policy.cadence.quantum==1,
             "memory epoch requires explicitly selected common independent/1 HBF policy");
    }
    // One old physical batch only. Neither later fills/dirty writebacks nor
    // newly admitted ingress are serviced again at this boundary.
    std::vector<g::L2DramCompletion> step_epoch(U cycle) {
        try {
            require_epoch_policy();time_call(cycle);++path_stats_.step_calls;
            std::vector<g::L2DramCompletion> out;
            for(const auto& c:inner_.step(cycle))consume_physical(c,cycle,out);
            const U through=clock_.poll_ps(cycle);
            while(!response_due_.empty()&&response_due_.begin()->first.first<=through){
                const auto event=response_due_.begin();auto it=live_.find(event->second);
                need(it!=live_.end()&&it->second.phase==Phase::RESPONSE,"response lifetime differs");
                out.push_back(deliver(it->second,clock_.completion_cycle(it->second.response_ready_ps),cycle));
                live_.erase(it);response_due_.erase(event);
            }
            admit_ingress(cycle); // current arrival, no second completion pump
            return out;
        }catch(...){failed_=true;throw;}
    }

    const g::L2DramRuntimeStatistics& statistics()const override{return stats_;}
    std::size_t queue_depth()const override{return live_.size();}
    std::size_t physical_queue_depth()const{return inner_.queue_depth();}
    std::size_t ingress_queue_depth()const{return ingress_.size();}
    std::size_t response_queue_depth()const{return response_due_.size();}
    U max_live()const{return path_.max_outer_live;}
    std::size_t admission_capacity()const override{return path_.max_outer_live;}
    U credits_per_pc()const{return inner_.credits_per_pc();}
    const std::vector<U>& occupied_credits()const{return inner_.occupied_credits();}
    const p::hbm::HbmConfig& native_config()const{return inner_.native_config();}
    NativeServiceDiagnostics service_diagnostics()const{return inner_.service_diagnostics();}
    const PathConfig& path_config()const{return path_;}
    const PathStatistics& path_statistics()const{return path_stats_;}
    const AdmissionStatistics& physical_admission_statistics()const{return inner_.admission_statistics();}
    AdmissionStatistics admission_statistics()const{
        auto a=inner_.admission_statistics();a.accepted=path_stats_.outer_accepted;a.completed=path_stats_.outer_delivered;
        a.blocked=path_stats_.outer_rejected;a.peak_live=path_stats_.peak_outer_live;a.total_admission_delay_ps=native_retry_delay_ps_;
        return a;
    }
    p::hbm::HbmStats physical_statistics()const{return inner_.physical_statistics();}
    void finalize()const{
        need(!failed_&&live_.empty()&&ingress_.empty()&&response_due_.empty()&&inner_to_outer_.empty(),"unclosed outer memory path");
        need(path_stats_.outer_accepted==path_stats_.physical_admitted&&path_stats_.physical_admitted==path_stats_.physical_completed&&
             path_stats_.physical_completed==path_stats_.outer_delivered,"outer/inner completion conservation differs");
        need(stats_.fill_requests==stats_.fill_completions&&stats_.writeback_requests==stats_.writeback_completions,
             "outer read/write delivery conservation differs");
        need(path_stats_.delivered_parent_segment_closures==path_stats_.outer_delivered&&
             path_stats_.delivered_segment_sum_ps==path_stats_.end_to_end_observed_latency_ps,
             "final delivered segment ledger differs");
        need(stats_.fill_bytes==stats_.fill_completed_bytes&&stats_.writeback_bytes==stats_.writeback_completed_bytes,
             "outer accepted/completed actual-byte conservation differs");
        inner_.finalize();
        need(inner_.statistics().fill_bytes==stats_.fill_bytes&&inner_.statistics().writeback_bytes==stats_.writeback_bytes,
             "outer/inner actual-byte census differs");
        need(inner_.statistics().fill_requests==stats_.fill_requests&&inner_.statistics().writeback_requests==stats_.writeback_requests,
             "outer/inner operation census differs");
    }
};
} // namespace sg_hbf
