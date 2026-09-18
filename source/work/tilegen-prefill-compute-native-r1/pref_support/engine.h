#pragma once
// Isolated approximate Prefill engine; native driver integration is not present.
#include "compiled_program.h"
namespace prefill_compute_runtime {
class PrefillComputeEngine {
    struct State {
        std::uint32_t remaining = 0, next_subop = 0, pending = 0;
        Cycle ready = 0;
        Token retry_token = 0;
        std::uint64_t retry_generation = 0;
        bool started = false, issue_done = false, issue_released = false, completed = false, retry_blocked = false;
    };
    struct Cta {
        std::shared_ptr<const CompiledProgram> program;
        std::vector<State> states; // one per SEGMENT; no original-group mutable array
        std::vector<std::uint32_t> cursor, warp_sp;
        std::uint64_t completed = 0;
    };
    struct WarpRef { CtaId cta; unsigned warp; };
    struct Sp {
        Cycle next_issue = 0;
        std::vector<Cycle> pipeline_free;
        std::vector<WarpRef> warps;
        std::size_t rr = 0;
        std::optional<Cycle> queued_wake;
    };
    enum class EventKind : unsigned { CompletePart = 0, ReleaseIssue = 1, RetryWake = 2, Schedule = 3 };
    struct Event {
        Cycle cycle; EventKind kind; std::uint64_t sequence; CtaId cta;
        unsigned group, sp; Token token;
    };
    struct Later {
        bool operator()(const Event& a, const Event& b) const {
            return std::tie(a.cycle,a.kind,a.sequence) > std::tie(b.cycle,b.kind,b.sequence);
        }
    };
    struct PendingToken { CtaId cta; unsigned group; bool completion_queued = false; };
    std::unordered_map<CtaId, std::unique_ptr<Cta>> ctas_;
    std::unordered_map<Token, PendingToken> pending_tokens_;
    std::vector<Sp> sps_;
    std::priority_queue<Event,std::vector<Event>,Later> events_;
    MemoryPort* memory_;
    Cycle now_ = 0;
    std::uint64_t sequence_ = 0; Token next_token_ = 1;
    Stats stats_;
    ActualStats actual_stats_;
    // Per-kernel lifetime registry: independent SM refill may admit CTA IDs
    // out of order. Retired IDs remain reserved so old events cannot bind a
    // later CTA. One entry per admitted CTA, never per logical member.
    std::set<CtaId> used_cta_ids_;

    Cta& cta(CtaId id) { auto it=ctas_.find(id); require(it!=ctas_.end(),"unknown CTA"); return *it->second; }
    void queue(EventKind kind, Cycle at, CtaId id, unsigned group, unsigned sp=0, Token token=0) {
        require(at >= now_, "backdated event");
        require(sequence_ != std::numeric_limits<std::uint64_t>::max(), "event sequence overflow");
        events_.push({at,kind,sequence_++,id,group,sp,token});
    }
    void wake_sp(unsigned sp, Cycle at) {
        require(sp<sps_.size(),"SP range"); at=std::max(at,sps_[sp].next_issue);
        auto& queued=sps_[sp].queued_wake;
        if (!queued || at<*queued) { queued=at; queue(EventKind::Schedule,at,0,0,sp); }
    }
    void release(CtaId id, unsigned g, EdgeType type) {
        auto& c=cta(id); auto& state=c.states[g];
        if (type==EdgeType::Issue) {
            require(!state.issue_released,"duplicate issue release");state.issue_released=true;
            // Conservative original work accounting: all merged groups have
            // reached their issue milestones at this tail, never at entry.
            if(c.program->recipe(g))stats_.compute_subops_issued=add(stats_.compute_subops_issued,c.program->compute_subops(g));
        }
        const auto& internal=c.program->internal(g);const auto t=unsigned(type);
        // Original contribution counts are accounted once at their segment tail.
        // They are not actual dynamic internal counter updates or event pops.
        stats_.typed_group_events_released=add(stats_.typed_group_events_released,internal.typed[t]);
        stats_.logical_external_edges_released=add(stats_.logical_external_edges_released,internal.logical[t]);
        actual_stats_.internal_typed_group_contributions=add(actual_stats_.internal_typed_group_contributions,internal.typed[t]);
        actual_stats_.internal_logical_edge_contributions=add(actual_stats_.internal_logical_edge_contributions,internal.logical[t]);
        for (auto edge_index:c.program->outgoing(g)) {
            const auto& e=c.program->plan().gates[edge_index]; if(e.type!=type)continue;
            auto& dest=c.states[e.target]; require(dest.remaining>0,"dependency counter underflow");
            --dest.remaining;++actual_stats_.actual_gate_updates;
            stats_.typed_group_events_released=add(stats_.typed_group_events_released,e.original_edge_indices.size());
            stats_.logical_external_edges_released=add(stats_.logical_external_edges_released,e.logical_multiplicity);
            if (!dest.remaining) {dest.ready=add(now_,1);wake_sp(c.warp_sp[c.program->group(e.target).warp],dest.ready);}
        }
    }
    void finish(CtaId id, unsigned g) {
        auto& c=cta(id);auto& state=c.states[g];const auto& segment=c.program->plan().segments[g];
        require(state.issue_done && state.pending==0 && !state.completed,"segment completion closure");
        state.completed=true;++c.completed;++actual_stats_.segments_completed;
        if(segment.groups.size()>1)++actual_stats_.merged_segments_completed;
        stats_.groups_completed=add(stats_.groups_completed,segment.groups.size());
        stats_.logical_members_completed=add(stats_.logical_members_completed,segment.logical_members);
        stats_.compute_elements_completed=add(stats_.compute_elements_completed,segment.compute_elements);
        stats_.tensor_fma_completed=add(stats_.tensor_fma_completed,segment.tensor_fma);
        release(id,g,EdgeType::Completion);
        if(c.completed==c.states.size())++stats_.ctas_completed;
    }
    void advance_cursor(Cta& c,unsigned warp,unsigned g) {
        auto& cursor=c.cursor[warp];
        require(cursor<c.program->plan().warp_segments[warp].size() && c.program->plan().warp_segments[warp][cursor]==g,"warp cursor closure");
        ++cursor;
    }
    Token token() {
        require(next_token_ != std::numeric_limits<Token>::max(),"token exhaustion");return next_token_++;
    }
    void compute_issue(CtaId id,unsigned g,unsigned spid) {
        auto& c=cta(id);auto& st=c.states[g];const auto& gdesc=c.program->group(g);auto& sp=sps_[spid];
        if(c.program->recipe(g)) {
            require(!st.started&&!st.next_subop,"segment issued twice");
            recurrence::ResourceClocks clocks;clocks.sp_next=sp.next_issue;
            std::copy(sp.pipeline_free.begin(),sp.pipeline_free.end(),clocks.pipeline_free.begin());
            // All checked recurrence arithmetic precedes state/resource commit.
            const auto timing=recurrence::evaluate_recipe(*c.program->recipe(g),now_,clocks);
            require(timing.first_dispatch==now_,"segment entry must really issue now");
            st.started=true;st.next_subop=1;st.pending=1;st.issue_done=true;
            sp.next_issue=timing.cursor_ready;
            std::copy(timing.after.pipeline_free.begin(),timing.after.pipeline_free.end(),sp.pipeline_free.begin());
            ++actual_stats_.merged_segments_issued;
            queue(EventKind::ReleaseIssue,timing.issue_tail,id,g);
            queue(EventKind::CompletePart,timing.completion_tail,id,g);
            advance_cursor(c,gdesc.warp,g);wake_sp(spid,sp.next_issue);return;
        }
        st.started=true;
        if(gdesc.kind==GroupKind::ComputePacket) {
            require(!st.next_subop,"packet issued twice"); st.next_subop=1;st.pending=1;st.issue_done=true;
            sp.next_issue=add(now_,gdesc.reservation_end);sp.pipeline_free[gdesc.pipeline]=sp.next_issue;
            stats_.compute_subops_issued=add(stats_.compute_subops_issued,gdesc.members.size());
            queue(EventKind::ReleaseIssue,add(now_,gdesc.issue_release),id,g);
            queue(EventKind::CompletePart,add(now_,gdesc.completion),id,g);
            advance_cursor(c,gdesc.warp,g);
        } else {
            ++st.next_subop;++st.pending;++stats_.compute_subops_issued;
            sp.next_issue=add(now_,1);sp.pipeline_free[gdesc.pipeline]=add(now_,gdesc.issue_span);
            queue(EventKind::CompletePart,add(add(now_,gdesc.issue_span),gdesc.latency),id,g);
            if(st.next_subop==gdesc.subops) {st.issue_done=true;queue(EventKind::ReleaseIssue,now_,id,g);advance_cursor(c,gdesc.warp,g);}
            else st.ready=add(now_,1);
        }
        wake_sp(spid,sp.next_issue);
    }
    void external_issue(CtaId id,unsigned g,unsigned spid) {
        require(memory_!=nullptr,"external singleton needs memory port");
        auto& c=cta(id);auto& st=c.states[g];const auto& group=c.program->group(g);
        if(!st.retry_token)st.retry_token=token();
        const IssueRequest request{now_,id,c.program->original_group(g),group.members[0].id,group.warp,spid,st.next_subop,st.retry_token,group.external_kind,group.external_descriptor};
        // Instruction/subop dispatch, never one call per cache line. The port
        // owns LD/ST pipeline readiness, delayed line injection and all line
        // retry/completion aggregation. It must not synchronously reenter Runtime.
        ++stats_.external_attempts;const auto reply=memory_->try_issue(request);
        if(reply.status==IssueReply::Status::Retry) {
            require(!reply.completed_now && !reply.last_subop,"retry cannot declare issue/completion");
            ++stats_.external_retries;st.retry_blocked=true;
            require(st.retry_generation!=std::numeric_limits<std::uint64_t>::max(),"retry generation overflow");++st.retry_generation;
            if(reply.retry_cycle) {require(*reply.retry_cycle>now_,"retry deadline must advance");queue(EventKind::RetryWake,*reply.retry_cycle,id,g,0,st.retry_generation);}
            return; // retry consumes no SP issue slot, other warps may proceed
        }
        require(!reply.retry_cycle,"accepted cannot also retry");
        if(!reply.last_subop)require(reply.next_issue_cycle>now_,"next subop readiness must advance");
        st.started=true;st.retry_blocked=false;++st.next_subop;++st.pending;++stats_.external_subops_accepted;
        require(pending_tokens_.emplace(st.retry_token,PendingToken{id,g,false}).second,"duplicate completion token");
        const Token accepted_token=st.retry_token;st.retry_token=0;
        sps_[spid].next_issue=add(now_,1);
        if(reply.last_subop) {st.issue_done=true;queue(EventKind::ReleaseIssue,now_,id,g);advance_cursor(c,group.warp,g);}
        else st.ready=reply.next_issue_cycle;
        if(reply.completed_now)complete(accepted_token,now_);
        wake_sp(spid,sps_[spid].next_issue);
    }
    void schedule(unsigned spid) {
        ++stats_.schedule_invocations;
        auto& sp=sps_[spid];const auto count=sp.warps.size();if(!count)return;
        std::optional<Cycle> earliest;
        for(std::size_t n=0;n<count;++n) {
            ++stats_.warp_candidate_visits;
            const auto index=(sp.rr+n)%count;const auto ref=sp.warps[index];auto& c=cta(ref.cta);
            const auto& warp=c.program->plan().warp_segments[ref.warp];if(c.cursor[ref.warp]==warp.size())continue;
            const auto g=warp[c.cursor[ref.warp]];auto& st=c.states[g];const auto& group=c.program->group(g);
            if(st.remaining || st.retry_blocked)continue;
            Cycle due=std::max(st.ready,sp.next_issue);
            if(is_compute(group.kind))due=std::max(due,sp.pipeline_free[group.pipeline]);
            if(due>now_) {if(!earliest || due<*earliest)earliest=due;continue;}
            if(is_external(group.kind)) {
                external_issue(ref.cta,g,spid);
                if(st.retry_blocked)continue;
            } else if(group.kind==GroupKind::BarrierSingleton) {
                st.started=true;st.issue_done=true;st.pending=1;sp.next_issue=add(now_,1);
                queue(EventKind::ReleaseIssue,now_,ref.cta,g);
                queue(EventKind::CompletePart,add(now_,group.barrier_latency),ref.cta,g);
                advance_cursor(c,ref.warp,g);wake_sp(spid,sp.next_issue);
            } else compute_issue(ref.cta,g,spid);
            sp.rr=(index+1)%count;return;
        }
        if(earliest)wake_sp(spid,*earliest);
    }
public:
    enum class Stop { Complete, WaitingForPort, Budget };
    explicit PrefillComputeEngine(unsigned sp_count,unsigned pipeline_count,MemoryPort* memory=nullptr,Cycle initial_cycle=0):sps_(sp_count),memory_(memory),now_(initial_cycle) {
        require(sp_count>0 && sp_count%4==0 && pipeline_count==6,"four-SP/six-pipeline Prefill resources");
        for(auto& sp:sps_)sp.pipeline_free.resize(pipeline_count);
    }
    Cycle now() const {return now_;}
    const Stats& stats() const {return stats_;}
    const ActualStats& actual_stats() const {return actual_stats_;}
    std::size_t pending_tokens() const {return pending_tokens_.size();}
    std::size_t resident_ctas() const {return ctas_.size();}
    std::optional<Cycle> next_event_cycle() const {return events_.empty()?std::nullopt:std::optional<Cycle>(events_.top().cycle);}
    void add_cta(CtaId id,std::shared_ptr<const CompiledProgram> program,const std::vector<unsigned>& warp_sp,Cycle ready=0) {
        require(program&&program->source().warp_groups.size()==4,"compiled four-warp program");
        require(!used_cta_ids_.count(id) && !ctas_.count(id) && warp_sp.size()==4 && ready>=now_,"CTA admission");
        for(unsigned w=0;w<4;++w)require(warp_sp[w]<sps_.size()&&warp_sp[w]%4==w&&warp_sp[w]/4==warp_sp[0]/4&&sps_[warp_sp[w]].warps.empty(),"one resident CTA and one warp per SP");
        auto c=std::make_unique<Cta>();c->program=std::move(program);c->warp_sp=warp_sp;c->cursor.resize(4);c->states.resize(c->program->plan().segments.size());
        for(std::size_t g=0;g<c->states.size();++g){c->states[g].remaining=c->program->indegree(g);c->states[g].ready=ready;}
        const auto slots=c->states.size();ctas_.emplace(id,std::move(c));used_cta_ids_.insert(id);++stats_.ctas_added;
        actual_stats_.segment_state_slots_current=add(actual_stats_.segment_state_slots_current,slots);
        actual_stats_.segment_state_slots_peak=std::max(actual_stats_.segment_state_slots_peak,actual_stats_.segment_state_slots_current);
        for(unsigned w=0;w<warp_sp.size();++w){sps_[warp_sp[w]].warps.push_back({id,w});wake_sp(warp_sp[w],ready);}
    }
    // Queue notifications before step_once if they coincide with its next
    // cycle, so completion/retry wake precedes same-cycle scheduling.
    void complete(Token token,Cycle at) {
        auto it=pending_tokens_.find(token);require(it!=pending_tokens_.end() && !it->second.completion_queued,"unknown/duplicate external completion");
        require(at>=now_,"backdated external completion");it->second.completion_queued=true;
        queue(EventKind::CompletePart,at,it->second.cta,it->second.group,0,token);
    }
    void wake_retry(CtaId id,unsigned group,Cycle at) {
        auto& c=cta(id);require(group<c.program->source().groups.size(),"original retry group range");
        const auto segment=c.program->plan().group_to_segment[group];
        require(c.program->plan().segments[segment].groups.size()==1&&c.program->original_group(segment)==group&&c.states[segment].retry_blocked,"retry wake requires original blocked singleton");
        queue(EventKind::RetryWake,at,id,segment,0,c.states[segment].retry_generation);
    }
    // One timestamp per call. External driver must bound stepping by its own
    // next event/poll frontier; this class cannot know future memory arrivals.
    bool step_once() {
        if(events_.empty())return false;
        now_=events_.top().cycle;
        while(!events_.empty() && events_.top().cycle==now_) {
            const auto e=events_.top();events_.pop();++stats_.events_processed;
            if(e.kind==EventKind::Schedule) {
                auto& pending=sps_[e.sp].queued_wake;if(!pending || *pending!=now_)continue;
                pending.reset();schedule(e.sp);continue;
            }
            if(e.kind==EventKind::RetryWake) {
                auto found=ctas_.find(e.cta);if(found==ctas_.end())continue; // stale hint after retirement
                auto& c=*found->second;auto& st=c.states[e.group];
                if(st.retry_blocked && e.token==st.retry_generation){st.retry_blocked=false;st.ready=now_;wake_sp(c.warp_sp[c.program->group(e.group).warp],now_);}continue;
            }
            auto& c=cta(e.cta);auto& st=c.states[e.group];
            if(e.kind==EventKind::ReleaseIssue) {release(e.cta,e.group,EdgeType::Issue);continue;}
            require(st.pending>0,"completion count underflow");
            if(e.token){auto it=pending_tokens_.find(e.token);require(it!=pending_tokens_.end() && it->second.completion_queued,"token closure");pending_tokens_.erase(it);}
            --st.pending;if(st.pending==0 && st.issue_done)finish(e.cta,e.group);
        }
        return true;
    }
    // Root driver order: device/L2.step(cycle), notify completions/retries,
    // advance_to(cycle), then L2.end_cycle. No crossing an unprocessed event.
    void advance_to(Cycle cycle) {
        require(cycle>=now_,"backdated runtime advance");
        if(memory_)require(cycle==now_ || cycle==add(now_,1),"memory-attached runtime requires consecutive externally pumped cycles");
        require(events_.empty() || events_.top().cycle>=cycle,"advance crossed an unprocessed runtime event");
        if(!events_.empty() && events_.top().cycle==cycle)step_once();
        else now_=cycle;
    }
    Stop run_known_events(std::uint64_t max_timestamps) {
        require(memory_==nullptr,"memory driver must pump its device then call advance_to");
        for(std::uint64_t n=0;n<max_timestamps && !events_.empty() && stats_.ctas_completed<stats_.ctas_added;++n)step_once();
        if(stats_.ctas_completed==stats_.ctas_added)return Stop::Complete;
        if(!events_.empty())return Stop::Budget;
        return stats_.ctas_completed==stats_.ctas_added?Stop::Complete:Stop::WaitingForPort;
    }
    bool cta_complete(CtaId id) const {
        auto it=ctas_.find(id);require(it!=ctas_.end(),"unknown CTA");return it->second->completed==it->second->states.size();
    }
    void retire_cta(CtaId id) {
        require(cta_complete(id),"cannot retire unfinished CTA");
        for(auto& sp:sps_) {
            sp.warps.erase(std::remove_if(sp.warps.begin(),sp.warps.end(),[id](const WarpRef& r){return r.cta==id;}),sp.warps.end());
            sp.rr=sp.warps.empty()?0:sp.rr%sp.warps.size();
        }
        actual_stats_.segment_state_slots_current-=cta(id).states.size();
        ctas_.erase(id);
    }
};
} // namespace prefill_compute_runtime
