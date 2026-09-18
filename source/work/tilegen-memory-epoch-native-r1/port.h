#pragma once
#include "../tilegen-tiny-full-r1/source.h"
#include "ada_source.h"
#include <functional>
#include "../tilegen-packet-runtime-r1/runtime.h"
#include <array>
#include <climits>
#include <deque>
#include <map>
#include <set>
#include <span>
#include <unordered_map>

namespace tiny_full {
namespace rt=packet_runtime;

// Original memory objects, different explicitly stated frontend scheduling.
// Accepted is a logical instruction/subop dispatch, NOT a cache-line enqueue.
class FrontendPort final : public rt::MemoryPort {
    struct Operation {
        rt::IssueRequest request;
        MemoryDescriptor instruction;
        unsigned phase=0; // 0 ordinary, 1 async source, 2 async destination
        bool source_posted=false;
        rt::Cycle frontend_due=0;
        U unadmitted=0,pending=0,accepted=0,completed=0;
        bool frontend_done=false;
    };
    struct LineRetry {int id;std::size_t line;g::L1ReadMissMemo memo{};};
    struct Sp {
        std::array<rt::Cycle,2> pipeline_available{{0,0}};
        std::array<std::deque<int>,2> frontend;
        std::deque<int> shared_retry;
        std::deque<LineRetry> global_retry;
        rt::Cycle shared_read_issue=0;
        unsigned forwarded=0;
        bool epoch_previous_rejection=false;
    };
    struct SharedWait {rt::CtaId cta;unsigned group,sp;bool write;};
    struct Counts {
        U instructions_accepted=0,instructions_completed=0,dispatch_retries=0;
        U global_read_bytes=0,global_write_bytes=0,shared_read_bytes=0,shared_write_bytes=0;
        U global_lines_accepted=0,global_lines_completed=0,global_lines_forwarded=0;
        U global_line_attempts=0,global_line_rejections=0,shared_accepted=0,shared_completed=0;
        U shared_service_bytes=0,shared_extra_wavefronts=0,shared_admission_rejections=0;
        U peak_outstanding_instructions=0,errors=0,ticks=0;
        U fifo_peak_lines_per_sp=0,fifo_budget_stops=0,fifo_rejection_stops=0;
        U copy_dispatches=0,copy_sources_posted=0,copy_sources_returned=0,copy_destinations_completed=0;
        U zero_source_copies=0;
    } counts_;
    const KernelBinding& binding_;
    std::vector<AdaSource> ada_;
    g::L2Cache& l2_;
    const g::SimulatorConfig& cfg_;
    std::vector<g::Memory*> sram_;
    std::vector<Sp> sp_;
    std::vector<std::deque<int>> copy_shared_ready_;
    std::unordered_map<int,std::unique_ptr<Operation>> operations_;
    std::map<std::pair<rt::CtaId,unsigned>,SharedWait> shared_waiters_;
    rt::Runtime* runtime_=nullptr;
    std::optional<rt::Cycle> last_tick_;
    rt::Cycle issue_span_=0;
    unsigned epoch_quantum_=1;
    rt::Cycle epoch_cursor_=0,epoch_origin_=0;
    using EpochService=std::function<std::vector<int>(rt::Cycle,unsigned,g::L2Cache::EpochServiceStatistics&)>;
    EpochService epoch_l2_;
    g::L2Cache::EpochServiceStatistics l2_epoch_counts_;
    struct EpochCounts {
        U boundaries=0,sp_visits=0,sp_forwarded_slots=0;
        U sp_rejection_stops=0,ada_line_attempts=0,ada_line_rejections=0;
        U sp_blocked_epochs=0,sp_blocked_prefix_slots=0;
        U periodic_tick_skips=0,partial_boundaries=0;
    } epoch_counts_;

    void check(bool yes,const char* why) {
        if(!yes){++counts_.errors;throw std::logic_error(why);}
    }
    g::Cycle cycle(rt::Cycle at) {
        check(at<=U(std::numeric_limits<g::Cycle>::max()),"packet port cycle out of core range");
        return static_cast<g::Cycle>(at);
    }
    Operation& operation(int id) {
        auto i=operations_.find(id);check(i!=operations_.end(),"unowned memory completion/retry ID");return *i->second;
    }
    int completion_id(const rt::IssueRequest& r) {
        U id=p::add(binding_.first_node(r.cta),r.member);
        check(id<=INT32_MAX,"canonical memory completion ID overflow");return int(id);
    }
    void maybe_complete(int id,rt::Cycle at) {
        auto& op=operation(id);
        if(!op.frontend_done||op.unadmitted||op.pending)return;
        if(op.phase==1) {
            if(!op.source_posted)return;
            ++counts_.copy_sources_returned;op.phase=2;op.unadmitted=1;
            // Destination enters the same finite SRAM write queue, without
            // a second ST instruction, SP issue slot, or invented LS delay.
            copy_shared_ready_[op.request.sp].push_back(id);return;
        }
        check(op.accepted==op.completed,"instruction completion counter closure");
        check(runtime_!=nullptr,"packet runtime not attached");
        runtime_->complete(op.request.token,at);
        ++counts_.instructions_completed;if(op.phase==2)++counts_.copy_destinations_completed;
        operations_.erase(id);
    }
    void on_complete(int id,bool global,unsigned sm,rt::Cycle at) {
        auto& op=operation(id);
        check((global?(op.phase==1||op.instruction.path==Path::DirectGlobal):
            (op.phase==2||op.instruction.path==Path::Shared))&&op.pending>0&&op.frontend_done,"memory completion source/pending mismatch");
        if(!global)check(op.request.sp/4==sm,"SRAM completion from wrong SM");
        --op.pending;++op.completed;
        if(global)++counts_.global_lines_completed;else ++counts_.shared_completed;
        maybe_complete(id,at);
    }
    bool enqueue_global(int id,std::size_t line,rt::Cycle at,g::L1ReadMissMemo* memo,bool bulk=false) {
        auto& op=operation(id);auto& sp=sp_[op.request.sp];
        check((op.instruction.path!=Path::Shared)&&line<op.instruction.lines.size()&&op.unadmitted>0,"global line admission identity");
        g::L2LineRequest request;
        request.completion_token=id;
        request.native_key=request.cache_key=op.instruction.lines[line];
        request.is_write=op.instruction.write;request.cycle=cycle(at);
        request.sm_id=int(op.request.sp/4);request.subpartition_id=int(op.request.sp%4);
        request.bypass_l1=op.instruction.bypass_l1;
        request.allow_l2_forward=bulk||cfg_.l2_max_transactions_per_cycle_per_sp<=0||
            sp.forwarded<unsigned(cfg_.l2_max_transactions_per_cycle_per_sp);
        request.source_matrix_id=1;request.memory_coalesce_bytes=128;
        request.source_subops=std::span<const g::ExplicitMemorySubop>(op.instruction.global_subops.data(),op.instruction.global_subops.size());
        request.source_subop_index=0;
        bool forwarded=false;++counts_.global_line_attempts;
        // Caller uses the declared approximate FIFO policy: after the first
        // forwarded line it stops this cycle, including possible later L1 hits.
        // This individual call still uses the original L1/L2 acceptance path.
        if(!l2_.enqueue_line_request(request,&forwarded,memo)) {
            check(!forwarded,"rejected line reported forwarded");++counts_.global_line_rejections;return false;
        }
        if(forwarded){if(!bulk)++sp.forwarded;++counts_.global_lines_forwarded;}
        --op.unadmitted;++op.pending;++op.accepted;++counts_.global_lines_accepted;
        return true;
    }
    bool enqueue_shared(int id,rt::Cycle at) {
        auto& op=operation(id);auto& sp=sp_[op.request.sp];auto& memory=*sram_[op.request.sp/4];
        check((op.instruction.path==Path::Shared||op.phase==2)&&op.unadmitted==1,"shared admission identity");
        const bool write=op.phase==2||op.instruction.write;
        const auto& service=op.instruction.shared_service;
        check(service.service_bytes<=INT_MAX&&service.extra_wavefronts>=0,"shared service range");
        if(memory.is_full(write)||!memory.enqueue(id,int(service.service_bytes),cycle(at),service.extra_wavefronts,write)){
            ++counts_.shared_admission_rejections;return false;
        }
        --op.unadmitted;++op.pending;++op.accepted;++counts_.shared_accepted;
        counts_.shared_service_bytes=p::add(counts_.shared_service_bytes,service.service_bytes);
        counts_.shared_extra_wavefronts=p::add(counts_.shared_extra_wavefronts,U(service.extra_wavefronts));
        if(!write)sp.shared_read_issue=std::max(sp.shared_read_issue,rt::add(at,U(service.extra_wavefronts)));
        return true;
    }
    void frontend_due(int id,rt::Cycle at) {
        auto& op=operation(id);auto& sp=sp_[op.request.sp];
        check(!op.frontend_done&&op.frontend_due<=at,"duplicate/early LS frontend completion");
        op.frontend_done=true;
        if((op.instruction.path!=Path::Shared)){
            // Append once at the original LS completion cycle. Lines retain
            // their source first-touch order and never bypass earlier work.
            for(std::size_t line=0;line<op.instruction.lines.size();++line)
                sp.global_retry.push_back({id,line,{}});
            counts_.fifo_peak_lines_per_sp=std::max<U>(counts_.fifo_peak_lines_per_sp,sp.global_retry.size());
        }else if(!enqueue_shared(id,at))sp.shared_retry.push_back(id);
    }
    void service_sp(unsigned id,rt::Cycle at) {
        auto& sp=sp_[id];
        // Approximate FIFO policy: shared retries retain their original scan;
        // append newly due LD then ST, then service only the global FIFO head.
        // A reject or exhausted forwarded-line budget blocks all later lines,
        // deliberately including later lines that might currently hit in L1.
        const auto shared_count=sp.shared_retry.size();
        for(std::size_t i=0;i<shared_count;++i){int node=sp.shared_retry.front();sp.shared_retry.pop_front();if(!enqueue_shared(node,at))sp.shared_retry.push_back(node);}
        for(unsigned pipe=0;pipe<2;++pipe)while(!sp.frontend[pipe].empty()){
            int node=sp.frontend[pipe].front();if(operation(node).frontend_due>at)break;
            sp.frontend[pipe].pop_front();frontend_due(node,at);
        }
        while(!sp.global_retry.empty()) {
            if(cfg_.l2_max_transactions_per_cycle_per_sp>0&&
               sp.forwarded>=unsigned(cfg_.l2_max_transactions_per_cycle_per_sp)) {
                ++counts_.fifo_budget_stops;break;
            }
            auto& head=sp.global_retry.front();
            if(!enqueue_global(head.id,head.line,at,&head.memo)) {
                ++counts_.fifo_rejection_stops;break;
            }
            sp.global_retry.pop_front();
        }
        auto& copies=copy_shared_ready_[id];
        while(!copies.empty()) {
            if(!enqueue_shared(copies.front(),at))break;
            copies.pop_front();
        }
    }
    void service_sp_epoch(unsigned id,rt::Cycle previous,rt::Cycle at) {
        auto& sp=sp_[id];++epoch_counts_.sp_visits;
        const auto shared_count=sp.shared_retry.size();
        for(std::size_t i=0;i<shared_count;++i){int node=sp.shared_retry.front();sp.shared_retry.pop_front();if(!enqueue_shared(node,at))sp.shared_retry.push_back(node);}
        for(unsigned pipe=0;pipe<2;++pipe)while(!sp.frontend[pipe].empty()){
            int node=sp.frontend[pipe].front();if(operation(node).frontend_due>at)break;
            sp.frontend[pipe].pop_front();frontend_due(node,at);
        }
        const bool previously_rejected=sp.epoch_previous_rejection;
        sp.epoch_previous_rejection=false;
        if(previously_rejected){
            ++epoch_counts_.sp_blocked_epochs;
            epoch_counts_.sp_blocked_prefix_slots=p::add(epoch_counts_.sp_blocked_prefix_slots,at-previous-1);
        }
        rt::Cycle slot=previously_rejected?at:rt::add(previous,1);
        sp.forwarded=0;
        while(!sp.global_retry.empty()) {
            const auto& head=sp.global_retry.front();
            const auto eligible=operation(head.id).frontend_due;
            if(eligible>slot){slot=eligible;sp.forwarded=0;}
            // Exactly the old pre-attempt quota guard, per virtual slot.
            // A possible later L1 hit never licenses an extra head probe.
            if(sp.forwarded>=unsigned(cfg_.l2_max_transactions_per_cycle_per_sp)){
                slot=rt::add(slot,1);sp.forwarded=0;
            }
            if(slot>at){++counts_.fifo_budget_stops;break;}
            const auto before=sp.forwarded;
            auto& retry=sp.global_retry.front();
            if(!enqueue_global(retry.id,retry.line,at,&retry.memo)) {
                sp.epoch_previous_rejection=true;
                ++counts_.fifo_rejection_stops;++epoch_counts_.sp_rejection_stops;break;
            }
            if(sp.forwarded>before)++epoch_counts_.sp_forwarded_slots;
            sp.global_retry.pop_front();
        }
        auto& copies=copy_shared_ready_[id];
        while(!copies.empty()) {
            if(!enqueue_shared(copies.front(),at))break;
            copies.pop_front();
        }
    }
    void service_epoch(rt::Cycle at) {
        check(epoch_quantum_>1&&at>epoch_cursor_&&at-epoch_cursor_<=epoch_quantum_,"Port epoch interval");
        const auto span=unsigned(at-epoch_cursor_);const auto current=cycle(at);
        const auto previous=epoch_cursor_;
        ++epoch_counts_.boundaries;
        if(span<epoch_quantum_)++epoch_counts_.partial_boundaries;
        // The actual integration performs one MemoryPath old-return poll and
        // one L2 service. It never calls either for each virtual budget slot.
        const auto global_completed=epoch_l2_(at,span,l2_epoch_counts_);
        for(int id:global_completed)on_complete(id,true,0,at);
        for(auto& sp:sp_)sp.forwarded=0;
        for(unsigned sm=0;sm<sram_.size();++sm) {
            const auto completed=sram_[sm]->step(current);for(int id:completed)on_complete(id,false,sm,at);
            const auto before=ada_[sm].statistics();
            const auto posted=ada_[sm].step_epoch(at,span,[&](int id,std::size_t line){return enqueue_global(id,line,at,nullptr,true);});
            const auto after=ada_[sm].statistics();
            epoch_counts_.ada_line_attempts=p::add(epoch_counts_.ada_line_attempts,after.line_attempts-before.line_attempts);
            epoch_counts_.ada_line_rejections=p::add(epoch_counts_.ada_line_rejections,after.line_rejections-before.line_rejections);
            for(int id:posted){auto& op=operation(id);check(op.phase==1&&!op.source_posted&&op.unadmitted==0,"copy source posted milestone");
                op.source_posted=true;++counts_.copy_sources_posted;maybe_complete(id,at);}
            for(unsigned local=0;local<4;++local)service_sp_epoch(sm*4+local,previous,at);
        }
        for(auto it=shared_waiters_.begin();it!=shared_waiters_.end();){const auto w=it->second;
            if(!sram_[w.sp/4]->is_full(w.write)&&(w.write||at>=sp_[w.sp].shared_read_issue)){
                runtime_->wake_retry(w.cta,w.group,at);it=shared_waiters_.erase(it);
            }else ++it;
        }
        epoch_cursor_=at;
    }
    void tick_epoch(rt::Cycle at) {
        check(runtime_!=nullptr,"runtime must attach before ticking");
        check(last_tick_?at==rt::add(*last_tick_,1):at==rt::add(epoch_cursor_,1),"memory tick must be exactly once each consecutive cycle");
        last_tick_=at;++counts_.ticks;
        if(at-epoch_cursor_<epoch_quantum_){++epoch_counts_.periodic_tick_skips;return;}
        service_epoch(at);
    }


public:
    FrontendPort(const KernelBinding& binding,g::L2Cache& l2,const g::SimulatorConfig& cfg,
                 std::vector<g::Memory*> per_sm_sram,rt::Cycle initial_cycle=0,
                 unsigned memory_epoch=1,
                 EpochService epoch_l2={})
        :binding_(binding),l2_(l2),cfg_(cfg),sram_(std::move(per_sm_sram)),sp_(cfg.num_sms>0&&cfg.num_sms<=1024?U(cfg.num_sms)*4:0){
        check(memory_epoch==1||memory_epoch==4||memory_epoch==8,"memory epoch quantum must be 1/4/8");
        check(memory_epoch==1||bool(epoch_l2),"memory epoch requires the actual old-return/L2 service callback");
        epoch_quantum_=memory_epoch;epoch_origin_=epoch_cursor_=initial_cycle;epoch_l2_=std::move(epoch_l2);
        check(cfg.num_sms>0&&cfg.num_sms<=1024&&sram_.size()==U(cfg.num_sms),"one supplied SRAM per SM");
        check(cfg.ls_throughput_bytes>0&&cfg.ls_width_bytes>0&&cfg.ls_latency_cycles>=0,"LS pipeline profile");
        check(cfg.ls_throughput_bytes==128&&cfg.ls_width_bytes==128&&cfg.ls_latency_cycles==10&&
              cfg.sram_latency_cycles==21&&cfg.sram_bandwidth_bytes_per_cycle==63&&cfg.sram_queue_depth==64&&
              cfg.l2_max_transactions_per_cycle_per_sp==1,"frozen tiny memory frontend profile");
        issue_span_=std::max<U>(1,(U(cfg.ls_throughput_bytes)+U(cfg.ls_width_bytes)-1)/U(cfg.ls_width_bytes));
        check(!l2.uses_whole_tiles()&&l2.per_sm_l1_config().num_sms==unsigned(cfg.num_sms),"native line L2 geometry");
        for(auto* m:sram_)check(m&&m->memory_latency==cfg.sram_latency_cycles&&m->memory_bandwidth==cfg.sram_bandwidth_bytes_per_cycle&&
            m->memory_write_bandwidth==cfg.sram_bandwidth_bytes_per_cycle&&m->memory_queue_depth==cfg.sram_queue_depth&&m->memory_write_queue_depth==cfg.sram_queue_depth,"supplied SRAM profile mismatch");
        check(cfg.bulk_copy_mechanism==g::BulkCopyMechanism::ADA_LDGSTS&&cfg.tma_setup_latency_cycles==0&&
            cfg.tma_issue_interval_cycles==1,"frozen Ada copy command profile");
        copy_shared_ready_.resize(sp_.size());
        for(unsigned sm=0;sm<sram_.size();++sm)ada_.emplace_back(
            AdaSource::Rate{cfg.bulk_copy_per_source_rate_numerator_bytes,cfg.bulk_copy_per_source_rate_denominator_cycles},
            AdaSource::Rate{cfg.bulk_copy_aggregate_sm_rate_numerator_bytes,cfg.bulk_copy_aggregate_sm_rate_denominator_cycles},initial_cycle);
    }
    void attach_runtime(rt::Runtime& runtime){check(runtime_==nullptr,"runtime already attached");runtime_=&runtime;}
    rt::IssueReply try_issue(const rt::IssueRequest& r)override {
        check(runtime_&&last_tick_&&r.cycle==*last_tick_,"dispatch must follow same-cycle memory tick");
        check(r.sp<sp_.size()&&r.cta<binding_.ctas()&&r.token!=0&&r.subop==0,"one original explicit memory subop required");
        const auto nodes=binding_.nodes(r.cta);
        check(r.member==r.external_descriptor&&r.member<nodes.size(),"memory descriptor/member mismatch");
        const auto& node=nodes[r.member];
        const bool global=node.kind==Kind::Global,copy=node.kind==Kind::AsyncCopy,write=node.write;
        check(global||copy||node.kind==Kind::Shared,"external request is not memory");
        check(node.warp==r.warp&&r.sp/4==r.cta%U(cfg_.num_sms)&&
            r.sp%4==unsigned(binding_.warp_token(r.cta,r.warp,unsigned(cfg_.num_sms))%4),"source CTA/warp/SP placement");
        check(r.external_kind==(global?1u:copy?3u:2u),"memory kind/source mismatch");
        const unsigned pipe=write?1:0;auto& sp=sp_[r.sp];rt::IssueReply answer;
        if(!global&&!copy){
            if(!write&&r.cycle<sp.shared_read_issue){answer.retry_cycle=sp.shared_read_issue;++counts_.dispatch_retries;return answer;}
            if(sram_[r.sp/4]->is_full(write)){
                shared_waiters_[{r.cta,r.group}]={r.cta,r.group,r.sp,write};
                ++counts_.dispatch_retries;return answer;
            }
        }
        if(!copy&&r.cycle<sp.pipeline_available[pipe]){answer.retry_cycle=sp.pipeline_available[pipe];++counts_.dispatch_retries;return answer;}
        const int id=completion_id(r);check(!operations_.count(id),"memory instruction dispatched twice");
        auto op=std::make_unique<Operation>();op->request=r;op->instruction=binding_.memory(r.cta,r.member);
        auto& d=op->instruction;
        check(d.path==(global?Path::DirectGlobal:copy?Path::AsyncGlobalToShared:Path::Shared)&&d.write==write,"bound memory path");
        check((!global&&!copy)||(d.global_subops.size()==1),"one bound global instruction subop");
        check((global)||(d.shared_subops.size()==1&&d.shared_bytes>0),"one bound shared instruction subop");
        check(!global||(d.global_bytes>0&&!d.lines.empty()),"empty direct global requires explicit importer fallback");
        if(copy) {
            check(!write&&d.bypass_l1,"Ada copy source is a bypass read");
            op->phase=1;op->frontend_done=true;op->unadmitted=d.lines.size();
            ++counts_.copy_dispatches;if(d.lines.empty())++counts_.zero_source_copies;
            ada_[r.sp/4].enqueue(id,binding_.warp_token(r.cta,r.warp,unsigned(cfg_.num_sms)),
                AdaSource::ready_after_dispatch(r.cycle,U(cfg_.tma_setup_latency_cycles)),d.lines.size());
        }else {
            op->frontend_due=rt::add(rt::add(r.cycle,issue_span_),U(cfg_.ls_latency_cycles));
            op->unadmitted=global?d.lines.size():1;
            sp.frontend[pipe].push_back(id);sp.pipeline_available[pipe]=rt::add(r.cycle,issue_span_);
        }
        if(global||copy){auto& count=write?counts_.global_write_bytes:counts_.global_read_bytes;count=p::add(count,d.global_bytes);}
        if(!global){auto& count=(write||copy)?counts_.shared_write_bytes:counts_.shared_read_bytes;count=p::add(count,d.shared_bytes);}
        operations_.emplace(id,std::move(op));shared_waiters_.erase({r.cta,r.group});
        ++counts_.instructions_accepted;counts_.peak_outstanding_instructions=std::max<U>(counts_.peak_outstanding_instructions,operations_.size());
        answer.status=rt::IssueReply::Status::Accepted;answer.last_subop=true;return answer;
    }
    void tick(rt::Cycle at) {
        if(epoch_quantum_>1){tick_epoch(at);return;}
        check(runtime_!=nullptr,"runtime must attach before ticking");
        check(!last_tick_||at==rt::add(*last_tick_,1),"memory tick must be exactly once each consecutive cycle");
        last_tick_=at;++counts_.ticks;const auto current=cycle(at);
        // L2/HBFSIM remains the original per-cycle service implementation.
        const auto global_completed=l2_.step(current);
        for(int id:global_completed)on_complete(id,true,0,at);
        for(auto& sp:sp_)sp.forwarded=0;
        for(unsigned sm=0;sm<sram_.size();++sm) {
            const auto completed=sram_[sm]->step(current);for(int id:completed)on_complete(id,false,sm,at);
            const auto posted=ada_[sm].step(at,[&](int id,std::size_t line){return enqueue_global(id,line,at,nullptr,true);});
            for(int id:posted){auto& op=operation(id);check(op.phase==1&&!op.source_posted&&op.unadmitted==0,"copy source posted milestone");
                op.source_posted=true;++counts_.copy_sources_posted;maybe_complete(id,at);}
            for(unsigned local=0;local<4;++local)service_sp(sm*4+local,at);
        }
        // Readiness is based on the real queue and original conflict throttle,
        // after all old retries/new LS completions were considered this cycle.
        for(auto it=shared_waiters_.begin();it!=shared_waiters_.end();){const auto w=it->second;
            if(!sram_[w.sp/4]->is_full(w.write)&&(w.write||at>=sp_[w.sp].shared_read_issue)){
                runtime_->wake_retry(w.cta,w.group,at);it=shared_waiters_.erase(it);
            }else ++it;
        }
    }
    // Closing a kernel may expose a shorter actual interval; no fictitious
    // cycles or token credit are granted to complete a full q-sized window.
    void flush_epoch() {
        check(epoch_quantum_>1&&last_tick_&&quiescent(),"partial epoch flush requires quiescent frontend");
        if(*last_tick_>epoch_cursor_)service_epoch(*last_tick_);
    }
    J epoch_statistics()const {
        U ada_slots=0,ada_calls=0,ada_checks=0,ada_reject_stops=0,ada_blocked=0,ada_blocked_slots=0;
        for(const auto& a:ada_){const auto& s=a.epoch_statistics();
            ada_slots=p::add(ada_slots,s.virtual_slots);ada_calls=p::add(ada_calls,s.service_calls);
            ada_checks=p::add(ada_checks,s.posting_checks);ada_reject_stops=p::add(ada_reject_stops,s.rejection_stops);
            ada_blocked=p::add(ada_blocked,s.blocked_epochs);ada_blocked_slots=p::add(ada_blocked_slots,s.blocked_prefix_slots);}
        return {{"quantum",epoch_quantum_},{"origin_cycle",epoch_origin_},{"clock_ticks",counts_.ticks},
            {"actual_backend_polls",epoch_counts_.boundaries},{"actual_l2_service_calls",l2_epoch_counts_.service_calls},
            {"actual_shared_steps",p::multiply(epoch_counts_.boundaries,U(sram_.size()))},
            {"actual_ada_epoch_calls",ada_calls},{"actual_sp_service_visits",epoch_counts_.sp_visits},
            {"virtual_l2_budget_slots",l2_epoch_counts_.virtual_slots},{"virtual_ada_budget_slots",ada_slots},
            {"l2_blocked_epochs",l2_epoch_counts_.blocked_epochs},{"l2_blocked_prefix_slots",l2_epoch_counts_.blocked_prefix_slots},
            {"sp_forwarded_slots_spent",epoch_counts_.sp_forwarded_slots},
            {"sp_blocked_epochs",epoch_counts_.sp_blocked_epochs},{"sp_blocked_prefix_slots",epoch_counts_.sp_blocked_prefix_slots},
            {"ada_blocked_epochs",ada_blocked},{"ada_blocked_prefix_slots",ada_blocked_slots},
            {"sp_rejection_stops",epoch_counts_.sp_rejection_stops},{"ada_posting_checks",ada_checks},
            {"ada_rejection_stops",ada_reject_stops},{"ada_line_attempts",epoch_counts_.ada_line_attempts},
            {"ada_line_rejections",epoch_counts_.ada_line_rejections},
            {"global_line_attempts",counts_.global_line_attempts},{"global_lines_accepted",counts_.global_lines_accepted},
            {"global_lines_completed",counts_.global_lines_completed},{"shared_accepted",counts_.shared_accepted},
            {"shared_completed",counts_.shared_completed},{"periodic_tick_skips",epoch_counts_.periodic_tick_skips},
            {"partial_boundaries",epoch_counts_.partial_boundaries},{"serviced_through",epoch_cursor_},
            {"independently_measured_wall_seconds",nullptr}};
    }

    bool quiescent()const {
        if(!operations_.empty()||!shared_waiters_.empty())return false;
        for(const auto& sp:sp_)if(!sp.frontend[0].empty()||!sp.frontend[1].empty()||!sp.shared_retry.empty()||!sp.global_retry.empty())return false;
        for(const auto& a:ada_)if(a.has_work())return false;
        for(const auto& q:copy_shared_ready_)if(!q.empty())return false;
        return true; // frontend only: caller must separately drain/audit L2/HBF
    }
    U outstanding()const{return operations_.size();}
    J stats()const {
        U unadmitted=0,pending=0,global_pending=0,shared_pending=0,delayed=0,global_retry=0,shared_retry=0;
        for(const auto& [id,p]:operations_){(void)id;unadmitted+=p->unadmitted;pending+=p->pending;if(p->instruction.path==Path::DirectGlobal||p->phase==1)global_pending+=p->pending;else shared_pending+=p->pending;}
        for(const auto& sp:sp_){delayed+=sp.frontend[0].size()+sp.frontend[1].size();global_retry+=sp.global_retry.size();shared_retry+=sp.shared_retry.size();}
        return {{"instructions_accepted",counts_.instructions_accepted},{"instructions_completed",counts_.instructions_completed},
            {"dispatch_retries",counts_.dispatch_retries},{"logical_global_read_bytes",counts_.global_read_bytes},{"logical_global_write_bytes",counts_.global_write_bytes},
            {"logical_shared_read_bytes",counts_.shared_read_bytes},{"logical_shared_write_bytes",counts_.shared_write_bytes},
            {"global_lines_accepted",counts_.global_lines_accepted},{"global_lines_completed",counts_.global_lines_completed},{"global_lines_forwarded_to_L2",counts_.global_lines_forwarded},
            {"global_line_attempts",counts_.global_line_attempts},{"global_line_rejections",counts_.global_line_rejections},
            {"shared_service_accepted",counts_.shared_accepted},{"shared_service_completed",counts_.shared_completed},
            {"shared_service_bytes",counts_.shared_service_bytes},{"shared_extra_wavefronts",counts_.shared_extra_wavefronts},
            {"shared_admission_rejections",counts_.shared_admission_rejections},{"peak_outstanding_instructions",counts_.peak_outstanding_instructions},
            {"outstanding_instructions",operations_.size()},{"outstanding_responses",pending},{"outstanding_global_line_responses",global_pending},{"outstanding_shared_responses",shared_pending},{"unadmitted_requests",unadmitted},
            {"delayed_LS_completions",delayed},{"global_line_retry_entries",global_retry},{"shared_retry_entries",shared_retry},{"shared_dispatch_waiters",shared_waiters_.size()},
            {"errors",counts_.errors},{"ticks",counts_.ticks},{"frontend_quiescent",quiescent()},
            {"global_fifo_peak_lines_per_sp",counts_.fifo_peak_lines_per_sp},
            {"global_fifo_budget_stops",counts_.fifo_budget_stops},{"global_fifo_rejection_stops",counts_.fifo_rejection_stops},
            {"async_copy_dispatches",counts_.copy_dispatches},{"async_sources_posted",counts_.copy_sources_posted},
            {"async_sources_returned",counts_.copy_sources_returned},{"async_destinations_completed",counts_.copy_destinations_completed},
            {"zero_source_copies",counts_.zero_source_copies},
            {"async_copy_ledger_closed_at_quiescence",quiescent()&&counts_.copy_dispatches==counts_.copy_sources_posted&&
                counts_.copy_dispatches==counts_.copy_sources_returned&&counts_.copy_dispatches==counts_.copy_destinations_completed},
            {"instruction_ledger_closed",counts_.instructions_accepted==counts_.instructions_completed+operations_.size()},
            {"global_line_ledger_closed",counts_.global_lines_accepted>=counts_.global_lines_completed&&counts_.global_lines_accepted-counts_.global_lines_completed==global_pending},
            {"shared_service_ledger_closed",counts_.shared_accepted>=counts_.shared_completed&&counts_.shared_accepted-counts_.shared_completed==shared_pending},
            {"policy",epoch_quantum_==1?"approximate FIFO v1: per-cycle original L2/HBFSIM; per SM SRAM, Ada source, then ascending SP; prior shared retries, append new due LD then ST; global head only, stop on first rejection or forwarded budget; later L1 hits deliberately wait":"approximate memory epoch: per-cycle dispatch; boundary old physical returns then L2; per SM SRAM, Ada finite resource slots, ascending SP; actual callbacks and arrivals at boundary; previous rejection blocks historical spending"},
            {"approximation",epoch_quantum_==1?"global head-of-line blocking and changed L1/L2 arrival/interleaving; global_line_retry_entries includes never-attempted queued lines; no fitted latency or hardware backend modification":"bounded epoch arrival/return visibility, finite same-boundary bursts and changed cache/MSHR/SP/Ada arbitration; physical traffic and cycles may change; original requests, capacities, addresses and HBF commands retained; no latency fit"}};
    }
};
} // namespace tiny_full
