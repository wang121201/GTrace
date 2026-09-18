#pragma once
// Independent approximate timing component: shared program, compact CTA state.
// Does not include the old core, model memory latency, or attach HBFSIM.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace packet_runtime {
using Cycle = std::uint64_t;
using Token = std::uint64_t;
using CtaId = std::uint64_t;
inline void require(bool ok, const char* why) { if (!ok) throw std::invalid_argument(why); }
inline Cycle add(Cycle a, Cycle b) {
    if (b > std::numeric_limits<Cycle>::max() - a) throw std::overflow_error("cycle overflow");
    return a + b;
}
enum class EdgeType : unsigned { Completion = 0, Issue = 1 };
enum class GroupKind { ComputePacket, ComputeSingleton, ControlSingleton, GlobalSingleton, SharedSingleton, ExternalSingleton, BarrierSingleton };
inline bool is_compute(GroupKind k) { return k==GroupKind::ComputePacket || k==GroupKind::ComputeSingleton || k==GroupKind::ControlSingleton; }
inline bool is_external(GroupKind k) { return k==GroupKind::GlobalSingleton || k==GroupKind::SharedSingleton || k==GroupKind::ExternalSingleton; }
struct Member {
    std::uint32_t id = 0;
    Cycle dispatch = 0, reservation_end = 0, completion = 0;
};
struct Group {
    GroupKind kind = GroupKind::ComputeSingleton;
    std::uint32_t warp = 0, pipeline = 0;
    std::vector<Member> members;  // immutable, shared across all CTA instances
    std::uint64_t modeled_compute_elements = 0;
    std::uint64_t modeled_tensor_fma = 0;
    Cycle issue_release = 0, reservation_end = 0, completion = 0;
    std::uint32_t subops = 1;  // compute singleton only
    Cycle issue_span = 1, latency = 0;
    Cycle barrier_latency = 0; // explicit importer parameter; no default memory latency
    std::uint32_t external_kind = 0; // opaque original global/shared/other descriptor tag
    std::uint64_t external_descriptor = 0; // stable index into shared original descriptors
};
struct Edge {
    std::uint32_t source = 0, target = 0;
    EdgeType type = EdgeType::Completion;
    std::uint64_t logical_multiplicity = 1;
};
struct Program {
    std::vector<Group> groups;
    std::vector<Edge> edges;
    std::vector<std::vector<std::uint32_t>> warp_groups;
    std::vector<std::vector<std::uint32_t>> outgoing; // edge indices, shared once
    std::vector<std::uint32_t> indegree; // unique typed group-event count
    std::uint64_t logical_members = 0, logical_edges = 0; // logical_edges counts external original typed edges
    std::uint64_t internal_logical_edges = 0; // compiler-certified edges handled by packet offsets
    std::uint32_t pipeline_count = 0;

    // Call once before publishing as shared_ptr<const Program>. Dense source
    // member IDs and typed edge multiplicities close source coverage.
    void finalize() {
        require(!groups.empty() && !warp_groups.empty() && pipeline_count > 0, "nonempty shared program");
        require(groups.size() <= std::numeric_limits<std::uint32_t>::max() && edges.size() <= std::numeric_limits<std::uint32_t>::max() && warp_groups.size() <= std::numeric_limits<std::uint32_t>::max(), "group/edge/warp ID range");
        outgoing.assign(groups.size(), {}); indegree.assign(groups.size(), 0);
        std::vector<unsigned> seen(groups.size(), 0);
        for (std::size_t w = 0; w < warp_groups.size(); ++w) {
            require(!warp_groups[w].empty() && warp_groups[w].size() <= std::numeric_limits<std::uint32_t>::max(), "warp program range");
            for (auto id : warp_groups[w]) {
                require(id < groups.size() && groups[id].warp == w && seen[id]++ == 0, "warp/group identity");
            }
        }
        std::set<std::uint32_t> members;
        std::uint64_t total = 0;
        for (std::size_t id = 0; id < groups.size(); ++id) {
            const auto& g = groups[id];
            require(is_compute(g.kind) || is_external(g.kind) || g.kind==GroupKind::BarrierSingleton, "unknown group kind");
            require(seen[id] == 1 && !g.members.empty(), "group coverage");
            require(g.members.size() <= std::numeric_limits<std::uint32_t>::max(), "member count");
            for (const auto& m : g.members) require(members.insert(m.id).second, "duplicate logical member");
            total = add(total, g.members.size());
            if (g.kind != GroupKind::ComputePacket) require(g.members.size() == 1, "singleton membership");
            if (g.kind == GroupKind::ComputePacket) {
                require(g.members.size() > 1 && g.pipeline < pipeline_count, "packet shape");
                Cycle release = 0, reserve = 0, complete = 0, prior = 0, prior_reserve = 0;
                bool first = true;
                for (const auto& m : g.members) {
                    require(m.reservation_end > m.dispatch && m.completion >= m.reservation_end, "packet member times");
                    require(first || (m.dispatch > prior && m.dispatch >= prior_reserve), "packet issue/pipeline occupancy");
                    first = false; prior = m.dispatch; prior_reserve = m.reservation_end;
                    release = std::max(release, m.dispatch); reserve = std::max(reserve, m.reservation_end); complete = std::max(complete, m.completion);
                }
                require(g.members.front().dispatch == 0 && g.issue_release == release && g.reservation_end == reserve && g.completion == complete, "packet tail closure");
            } else if ((g.kind == GroupKind::ComputeSingleton || g.kind == GroupKind::ControlSingleton)) {
                require(g.pipeline < pipeline_count && g.subops > 0 && g.issue_span > 0, "compute singleton profile");
            }
        }
        require(total == logical_members && members.size() == logical_members, "logical member closure");
        require(*members.begin() == 0 && *members.rbegin() == logical_members - 1, "dense source member IDs");
        std::set<std::tuple<unsigned, unsigned, unsigned>> unique;
        std::uint64_t edge_total = 0;
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto& e = edges[i];
            require(e.source < groups.size() && e.target < groups.size() && e.source != e.target && e.logical_multiplicity > 0, "typed group edge");
            require(e.type == EdgeType::Completion || e.type == EdgeType::Issue, "typed edge kind");
            require(unique.emplace(e.source, e.target, unsigned(e.type)).second, "duplicate typed group event");
            outgoing[e.source].push_back(static_cast<unsigned>(i));
            require(indegree[e.target] != std::numeric_limits<std::uint32_t>::max(), "event indegree overflow");
            ++indegree[e.target];
            edge_total = add(edge_total, e.logical_multiplicity);
        }
        require(edge_total == logical_edges, "external typed edge multiplicity closure");
        std::vector<std::vector<unsigned>> check_children(groups.size());
        auto degree = indegree;
        for(const auto& e:edges)check_children[e.source].push_back(e.target);
        for(const auto& warp:warp_groups)for(std::size_t j=1;j<warp.size();++j){check_children[warp[j-1]].push_back(warp[j]);require(degree[warp[j]]!=std::numeric_limits<std::uint32_t>::max(),"warp indegree overflow");++degree[warp[j]];}
        std::queue<unsigned> ready;
        for (unsigned i = 0; i < degree.size(); ++i) if (!degree[i]) ready.push(i);
        std::size_t visited = 0;
        while (!ready.empty()) {
            auto g = ready.front(); ready.pop(); ++visited;
            for (auto target : check_children[g]) if (--degree[target] == 0) ready.push(target);
        }
        require(visited == groups.size(), "quotient cycle");
    }
};

struct IssueRequest {
    Cycle cycle = 0; CtaId cta = 0;
    std::uint32_t group = 0, member = 0, warp = 0, sp = 0, subop = 0;
    Token token = 0;
    std::uint32_t external_kind = 0;
    std::uint64_t external_descriptor = 0;
};
struct IssueReply {
    enum class Status { Accepted, Retry } status = Status::Retry;
    bool last_subop = false, completed_now = false;
    // Retry deadline must be authoritative and strictly in the future. null
    // means the external driver must call wake_retry; no invented polling rate.
    std::optional<Cycle> retry_cycle;
    // Accepted non-final subop: original port gives its next issue readiness.
    Cycle next_issue_cycle = 0;
};
struct MemoryPort {
    virtual ~MemoryPort() = default;
    virtual IssueReply try_issue(const IssueRequest&) = 0;
};
struct Stats {
    std::uint64_t ctas_added = 0, ctas_completed = 0, groups_completed = 0;
    std::uint64_t logical_members_completed = 0, compute_elements_completed = 0, tensor_fma_completed = 0;
    std::uint64_t logical_external_edges_released = 0, typed_group_events_released = 0;
    std::uint64_t compute_subops_issued = 0, external_subops_accepted = 0;
    std::uint64_t external_attempts = 0, external_retries = 0, events_processed = 0;
    // Actual software visits across all SPs; these are never stall cycles.
    std::uint64_t schedule_invocations = 0, warp_candidate_visits = 0;
};

class LegacyRuntime {
    struct State {
        std::uint32_t remaining = 0, next_subop = 0, pending = 0;
        Cycle ready = 0;
        Token retry_token = 0;
        std::uint64_t retry_generation = 0;
        bool started = false, issue_done = false, issue_released = false, completed = false, retry_blocked = false;
    };
    struct Cta {
        std::shared_ptr<const Program> program;
        std::vector<State> states; // one per group, never one per packet member
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
        if (type==EdgeType::Issue) { require(!state.issue_released,"duplicate issue release"); state.issue_released=true; }
        for (auto edge_index:c.program->outgoing[g]) {
            const auto& e=c.program->edges[edge_index]; if(e.type!=type)continue;
            auto& dest=c.states[e.target]; require(dest.remaining>0,"dependency counter underflow");
            --dest.remaining; ++stats_.typed_group_events_released;
            stats_.logical_external_edges_released=add(stats_.logical_external_edges_released,e.logical_multiplicity);
            if (!dest.remaining) {dest.ready=add(now_,1);wake_sp(c.warp_sp[c.program->groups[e.target].warp],dest.ready);}
        }
    }
    void finish(CtaId id, unsigned g) {
        auto& c=cta(id);auto& state=c.states[g];const auto& group=c.program->groups[g];
        require(state.issue_done && state.pending==0 && !state.completed,"group completion closure");
        state.completed=true;++c.completed;++stats_.groups_completed;
        stats_.logical_members_completed=add(stats_.logical_members_completed,group.members.size());
        stats_.compute_elements_completed=add(stats_.compute_elements_completed,group.modeled_compute_elements);
        stats_.tensor_fma_completed=add(stats_.tensor_fma_completed,group.modeled_tensor_fma);
        release(id,g,EdgeType::Completion);
        if(c.completed==c.states.size())++stats_.ctas_completed;
    }
    void advance_cursor(Cta& c,unsigned warp,unsigned g) {
        auto& cursor=c.cursor[warp];
        require(cursor<c.program->warp_groups[warp].size() && c.program->warp_groups[warp][cursor]==g,"warp cursor closure");
        ++cursor;
    }
    Token token() {
        require(next_token_ != std::numeric_limits<Token>::max(),"token exhaustion");return next_token_++;
    }
    void compute_issue(CtaId id,unsigned g,unsigned spid) {
        auto& c=cta(id);auto& st=c.states[g];const auto& gdesc=c.program->groups[g];auto& sp=sps_[spid];
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
        auto& c=cta(id);auto& st=c.states[g];const auto& group=c.program->groups[g];
        if(!st.retry_token)st.retry_token=token();
        const IssueRequest request{now_,id,g,group.members[0].id,group.warp,spid,st.next_subop,st.retry_token,group.external_kind,group.external_descriptor};
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
            const auto& warp=c.program->warp_groups[ref.warp];if(c.cursor[ref.warp]==warp.size())continue;
            const auto g=warp[c.cursor[ref.warp]];auto& st=c.states[g];const auto& group=c.program->groups[g];
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
    explicit LegacyRuntime(unsigned sp_count,unsigned pipeline_count,MemoryPort* memory=nullptr,Cycle initial_cycle=0):sps_(sp_count),memory_(memory),now_(initial_cycle) {
        require(sp_count>0 && pipeline_count>0,"runtime resources");
        for(auto& sp:sps_)sp.pipeline_free.resize(pipeline_count);
    }
    Cycle now() const {return now_;}
    const Stats& stats() const {return stats_;}
    std::size_t pending_tokens() const {return pending_tokens_.size();}
    std::size_t resident_ctas() const {return ctas_.size();}
    std::optional<Cycle> next_event_cycle() const {return events_.empty()?std::nullopt:std::optional<Cycle>(events_.top().cycle);}
    void add_cta(CtaId id,std::shared_ptr<const Program> program,const std::vector<unsigned>& warp_sp,Cycle ready=0) {
        require(program && program->outgoing.size()==program->groups.size() && program->indegree.size()==program->groups.size(),"finalized shared program");
        require(!used_cta_ids_.count(id) && !ctas_.count(id) && warp_sp.size()==program->warp_groups.size() && ready>=now_,"CTA admission");
        for(auto sp:warp_sp)require(sp<sps_.size() && program->pipeline_count<=sps_[sp].pipeline_free.size(),"CTA resource mapping");
        auto c=std::make_unique<Cta>();c->program=std::move(program);c->warp_sp=warp_sp;c->cursor.resize(warp_sp.size());c->states.resize(c->program->groups.size());
        for(std::size_t g=0;g<c->states.size();++g){c->states[g].remaining=c->program->indegree[g];c->states[g].ready=ready;}
        ctas_.emplace(id,std::move(c));used_cta_ids_.insert(id);++stats_.ctas_added;
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
        auto& c=cta(id);require(group<c.states.size() && c.states[group].retry_blocked,"retry wake requires blocked group");
        queue(EventKind::RetryWake,at,id,group,0,c.states[group].retry_generation);
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
                if(st.retry_blocked && e.token==st.retry_generation){st.retry_blocked=false;st.ready=now_;wake_sp(c.warp_sp[c.program->groups[e.group].warp],now_);}continue;
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
        ctas_.erase(id);
    }
};
} // namespace packet_runtime
#include "../tilegen-memory-phase-runtime-r1/phase_plan.h"
namespace packet_runtime {
struct PhaseDiagnostics {
    std::uint64_t plans=0,phases=0,entries=0,checkpoints=0,retries=0,returns=0;
    std::uint64_t virtual_compute_issues=0,issue_tails=0,compute_tails=0,return_tails=0;
    std::uint64_t original_groups_completed=0,internal_edges_covered=0,gate_updates=0;
    std::uint64_t cycle_fallbacks=0;
};
struct PhaseProgram {
    memory_phase::Plan plan;
    std::vector<Group> descriptions;
    std::vector<std::vector<unsigned>> outgoing;
    std::vector<std::uint32_t> indegree;
    struct Prefix {Cycle first=0,last_reserve=0,complete=0;unsigned count=0;};
    std::vector<std::vector<Prefix>> prefixes;
    explicit PhaseProgram(memory_phase::Plan source):plan(std::move(source)) {
        outgoing.resize(plan.units.size());indegree.resize(plan.units.size());prefixes.resize(plan.units.size());
        for(unsigned i=0;i<plan.units.size();++i) {
            const auto& u=plan.units[i];Group g;
            if(!u.phase)g=plan.original->groups[u.original_groups[0]];
            else {
                g.kind=GroupKind::ExternalSingleton;g.warp=u.warp;g.pipeline=3;
                g.modeled_compute_elements=u.compute_elements;
                for(auto old:u.original_groups) {
                    const auto& original=plan.original->groups[old];
                    g.members.insert(g.members.end(),original.members.begin(),original.members.end());
                }
                for(const auto& checkpoint:u.checkpoints) {
                    Prefix p;p.count=checkpoint.compute_end-checkpoint.compute_begin;
                    if(p.count) {
                        p.first=u.compute[checkpoint.compute_begin].dispatch;
                        for(unsigned k=checkpoint.compute_begin;k<checkpoint.compute_end;++k) {
                            p.last_reserve=std::max(p.last_reserve,u.compute[k].reservation_end);
                            p.complete=std::max(p.complete,u.compute[k].completion);
                        }
                    }
                    prefixes[i].push_back(p);
                }
            }
            descriptions.push_back(std::move(g));
        }
        for(unsigned i=0;i<plan.gates.size();++i) {
            const auto& edge=plan.gates[i];outgoing[edge.source].push_back(i);
            require(indegree[edge.target]!=std::numeric_limits<std::uint32_t>::max(),"phase gate indegree overflow");
            ++indegree[edge.target];
        }
    }
};

class PhaseEngine {
    struct State {
        std::uint32_t remaining = 0, next_subop = 0, pending = 0;
        Cycle ready = 0;
        Token retry_token = 0;
        std::uint64_t retry_generation = 0;
        bool started = false, issue_done = false, issue_released = false, completed = false, retry_blocked = false;
    };
    struct PhaseState {
        unsigned next=0,booked_compute=0;
        Cycle base=0,shift=0,compute_frontier=0,return_frontier=0;
        std::uint64_t lease_generation=0;
        bool prefix_counted=false,compute_done=false,returns_done=false,return_queued=false;
    };
    struct Cta {
        std::shared_ptr<const Program> program;
        std::shared_ptr<const PhaseProgram> phases;
        std::vector<std::unique_ptr<PhaseState>> phase_states; // only phases own extra state
        std::vector<State> states; // one per phase or retained original unit
        std::vector<std::uint32_t> cursor, warp_sp;
        std::uint64_t completed = 0;
    };
    struct WarpRef { CtaId cta; unsigned warp; };
    struct Lease {CtaId cta;unsigned group;std::uint64_t generation;};
    struct Sp {
        std::optional<Lease> lease;
        Cycle next_issue = 0;
        std::vector<Cycle> pipeline_free;
        std::vector<WarpRef> warps;
        std::size_t rr = 0;
        std::optional<Cycle> queued_wake;
    };
    enum class EventKind : unsigned { CompletePart = 0, ReleaseIssue = 1, RetryWake = 2, PhaseCompute = 3, PhaseReturns = 4, PhaseCheckpoint = 5, Schedule = 6 };
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
    PhaseDiagnostics phase_stats_;
    unsigned width_;
    std::map<const Program*,std::shared_ptr<const PhaseProgram>> programs_;
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
    void release(CtaId id,unsigned g,EdgeType type) {
        auto& c=cta(id);auto& st=c.states[g];
        if(type==EdgeType::Issue){require(!st.issue_released,"duplicate issue release");st.issue_released=true;}
        if(phase(c,g)) {
            require(type==EdgeType::Issue,"phase completion uses separate milestones");
            ++phase_stats_.issue_tails;release_gate(id,g,memory_phase::Milestone::IssueTail);maybe_finish_phase(id,g);
        } else release_gate(id,g,type==EdgeType::Issue?memory_phase::Milestone::OriginalIssue:memory_phase::Milestone::OriginalCompletion);
    }
    void finish(CtaId id, unsigned g) {
        auto& c=cta(id);auto& state=c.states[g];const auto& group=description(c,g);
        require(state.issue_done && state.pending==0 && !state.completed,"group completion closure");
        require(!phase(c,g),"phase must use milestone closure");
        state.completed=true;++c.completed;++stats_.groups_completed;++phase_stats_.original_groups_completed;
        stats_.logical_members_completed=add(stats_.logical_members_completed,group.members.size());
        stats_.compute_elements_completed=add(stats_.compute_elements_completed,group.modeled_compute_elements);
        stats_.tensor_fma_completed=add(stats_.tensor_fma_completed,group.modeled_tensor_fma);
        release(id,g,EdgeType::Completion);
        if(c.completed==c.states.size())++stats_.ctas_completed;
    }
    void advance_cursor(Cta& c,unsigned warp,unsigned g) {
        auto& cursor=c.cursor[warp];
        require(cursor<warp_groups(c)[warp].size() && warp_groups(c)[warp][cursor]==g,"warp cursor closure");
        ++cursor;
    }
    Token token() {
        require(next_token_ != std::numeric_limits<Token>::max(),"token exhaustion");return next_token_++;
    }
    void compute_issue(CtaId id,unsigned g,unsigned spid) {
        auto& c=cta(id);auto& st=c.states[g];const auto& gdesc=description(c,g);auto& sp=sps_[spid];
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
        auto& c=cta(id);auto& st=c.states[g];const auto& group=description(c,g);
        if(!st.retry_token)st.retry_token=token();
        const IssueRequest request{now_,id,g,group.members[0].id,group.warp,spid,st.next_subop,st.retry_token,group.external_kind,group.external_descriptor};
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
        if(sps_[spid].lease)return;
        ++stats_.schedule_invocations;
        auto& sp=sps_[spid];const auto count=sp.warps.size();if(!count)return;
        std::optional<Cycle> earliest;
        for(std::size_t n=0;n<count;++n) {
            ++stats_.warp_candidate_visits;
            const auto index=(sp.rr+n)%count;const auto ref=sp.warps[index];auto& c=cta(ref.cta);
            const auto& warp=warp_groups(c)[ref.warp];if(c.cursor[ref.warp]==warp.size())continue;
            const auto g=warp[c.cursor[ref.warp]];auto& st=c.states[g];const auto& group=description(c,g);
            if(st.remaining || st.retry_blocked)continue;
            Cycle due=std::max(st.ready,sp.next_issue);
            if(!phase(c,g)&&is_compute(group.kind))due=std::max(due,sp.pipeline_free[group.pipeline]);
            if(due>now_) {if(!earliest || due<*earliest)earliest=due;continue;}
            if(phase(c,g)) {
                phase_issue(ref.cta,g,spid);if(st.retry_blocked)continue;
            } else if(is_external(group.kind)) {
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
    bool phase(const Cta& c,unsigned g)const {return c.phases->plan.units[g].phase;}
    const Group& description(const Cta& c,unsigned g)const {return c.phases->descriptions[g];}
    const std::vector<std::vector<unsigned>>& warp_groups(const Cta& c)const {return c.phases->plan.warp_units;}
    void release_gate(CtaId id,unsigned g,memory_phase::Milestone milestone) {
        auto& c=cta(id);
        for(auto index:c.phases->outgoing[g]) {
            const auto& edge=c.phases->plan.gates[index];if(edge.milestone!=milestone)continue;
            auto& target=c.states[edge.target];require(target.remaining>0,"phase gate counter underflow");
            --target.remaining;++stats_.typed_group_events_released;++phase_stats_.gate_updates;
            stats_.logical_external_edges_released=add(stats_.logical_external_edges_released,edge.original_multiplicity);
            if(!target.remaining){target.ready=add(now_,1);wake_sp(c.warp_sp[description(c,edge.target).warp],target.ready);}
        }
    }
    void maybe_finish_phase(CtaId id,unsigned g) {
        auto& c=cta(id);auto& st=c.states[g];auto& ps=*c.phase_states[g];
        if(!st.issue_released||!ps.compute_done||!ps.returns_done)return;
        require(st.issue_done&&st.pending==0&&!st.completed,"phase final closure");
        st.completed=true;++c.completed;++stats_.groups_completed;
        const auto& unit=c.phases->plan.units[g];
        stats_.logical_members_completed=add(stats_.logical_members_completed,unit.logical_members);
        stats_.compute_elements_completed=add(stats_.compute_elements_completed,unit.compute_elements);
        phase_stats_.original_groups_completed=add(phase_stats_.original_groups_completed,unit.original_groups.size());
        phase_stats_.internal_edges_covered=add(phase_stats_.internal_edges_covered,unit.original_internal_edges);
        if(c.completed==c.states.size())++stats_.ctas_completed;
    }
    void queue_return_tail(CtaId id,unsigned g,Cycle at) {
        auto& c=cta(id);auto& st=c.states[g];auto& ps=*c.phase_states[g];
        if(st.issue_done&&st.pending==0&&!ps.return_queued) {
            ps.return_queued=true;queue(EventKind::PhaseReturns,std::max(at,ps.return_frontier),id,g);
        }
    }
    // Book only the compute prefix ending at the next memory checkpoint.
    // No per-compute runtime loop: its static maximum/profile was sealed once.
    void book_checkpoint(CtaId id,unsigned g,unsigned spid) {
        auto& c=cta(id);auto& ps=*c.phase_states[g];auto& sp=sps_[spid];
        const auto& unit=c.phases->plan.units[g];const auto& ck=unit.checkpoints.at(ps.next);
        const auto& prefix=c.phases->prefixes[g].at(ps.next);
        auto absolute=[&](Cycle offset){return add(add(ps.base,ps.shift),offset);};
        if(prefix.count) {
            auto earliest=absolute(prefix.first);auto ready=std::max(sp.next_issue,sp.pipeline_free[0]);
            if(earliest<ready)ps.shift=add(ps.shift,ready-earliest);
            sp.pipeline_free[0]=absolute(prefix.last_reserve);
            ps.compute_frontier=std::max(ps.compute_frontier,absolute(prefix.complete));
        }
        auto due=absolute(ck.earliest_dispatch);
        require(due>=sp.next_issue,"phase checkpoint must follow reserved issue prefix");
        require(due>=now_,"phase checkpoint backdated");
        ps.booked_compute=prefix.count;ps.prefix_counted=false;
        sp.next_issue=due;queue(EventKind::PhaseCheckpoint,due,id,g,spid,ps.lease_generation);
    }
    void phase_checkpoint(CtaId id,unsigned g,unsigned spid) {
        auto& c=cta(id);auto& st=c.states[g];auto& ps=*c.phase_states[g];auto& sp=sps_[spid];
        require(sp.lease&&sp.lease->cta==id&&sp.lease->group==g,"phase checkpoint lease ownership");
        const auto& unit=c.phases->plan.units[g];const auto& ck=unit.checkpoints.at(ps.next);
        if(!ps.prefix_counted) {
            stats_.compute_subops_issued=add(stats_.compute_subops_issued,ps.booked_compute);
            phase_stats_.virtual_compute_issues=add(phase_stats_.virtual_compute_issues,ps.booked_compute);
            ps.prefix_counted=true;
        }
        const Cycle nominal=add(ps.base,ck.earliest_dispatch);
        require(now_>=nominal,"phase memory dispatch cannot precede isolated checkpoint");
        ps.shift=std::max(ps.shift,now_-nominal);
        if(!st.retry_token)st.retry_token=token();
        const auto& original=c.program->groups[ck.original_group];
        const IssueRequest request{now_,id,g,ck.source_member,unit.warp,spid,0,st.retry_token,original.external_kind,original.external_descriptor};
        ++phase_stats_.checkpoints;++stats_.external_attempts;
        const auto reply=memory_->try_issue(request);
        if(reply.status==IssueReply::Status::Retry) {
            require(!reply.completed_now&&!reply.last_subop,"phase Retry cannot claim dispatch");
            ++stats_.external_retries;++phase_stats_.retries;st.retry_blocked=true;
            require(st.retry_generation!=std::numeric_limits<std::uint64_t>::max(),"phase retry generation overflow");++st.retry_generation;
            sp.lease.reset();sp.next_issue=now_; // Retry did not consume an issue slot.
            if(reply.retry_cycle){require(*reply.retry_cycle>now_,"phase retry must advance");queue(EventKind::RetryWake,*reply.retry_cycle,id,g,0,st.retry_generation);}
            wake_sp(spid,now_);return;
        }
        require(reply.last_subop&&!reply.retry_cycle,"one original phase memory subop required");
        st.retry_blocked=false;++st.next_subop;++st.pending;++stats_.external_subops_accepted;
        const Token accepted=st.retry_token;require(pending_tokens_.emplace(accepted,PendingToken{id,g,false}).second,"duplicate phase token");st.retry_token=0;
        ++ps.next;sp.next_issue=add(now_,1);
        if(ps.next==unit.checkpoints.size()) {
            st.issue_done=true;advance_cursor(c,unit.warp,g);
            queue(EventKind::ReleaseIssue,now_,id,g);
            queue(EventKind::PhaseCompute,std::max(now_,ps.compute_frontier),id,g);
            sp.lease.reset();wake_sp(spid,sp.next_issue);
            queue_return_tail(id,g,now_);
        } else book_checkpoint(id,g,spid);
        if(reply.completed_now)complete(accepted,now_);
    }
    void phase_issue(CtaId id,unsigned g,unsigned spid) {
        require(memory_!=nullptr,"phase requires original memory port");
        auto& c=cta(id);auto& st=c.states[g];auto& ps=*c.phase_states[g];auto& sp=sps_[spid];
        require(!sp.lease,"double SP phase lease");
        require(ps.lease_generation!=std::numeric_limits<std::uint64_t>::max(),"phase lease generation overflow");++ps.lease_generation;
        sp.lease=Lease{id,g,ps.lease_generation};
        if(!st.started){st.started=true;ps.base=now_;ps.prefix_counted=true;++phase_stats_.entries;}
        // A retry resumes this already-consumed prefix; it never rebooks it.
        phase_checkpoint(id,g,spid);
    }

public:
    enum class Stop { Complete, WaitingForPort, Budget };
    explicit PhaseEngine(unsigned sp_count,unsigned pipeline_count,MemoryPort* memory,Cycle initial_cycle,unsigned width):sps_(sp_count),memory_(memory),now_(initial_cycle),width_(width) {
        require(sp_count>0 && pipeline_count>0&&(width==4||width==16),"phase runtime resources/width");
        for(auto& sp:sps_)sp.pipeline_free.resize(pipeline_count);
    }
    void register_phase_program(std::shared_ptr<const Program> p,const std::vector<memory_phase::SourceFact>& facts) {
        require(p&& !programs_.count(p.get()),"unique phase program registration");
        auto plan=[&](){
            try{return memory_phase::make_plan(p,facts,width_);}
            catch(const std::invalid_argument& e){
                if(std::string(e.what())!="phase quotient cycle; reject partition, never remove dependencies")throw;
                ++phase_stats_.cycle_fallbacks;
                // Full original partition, all typed multiplicities and warp
                // cursor edges are checked again. No edge is silently removed.
                return memory_phase::make_plan(p,facts,1);
            }
        }();
        phase_stats_.phases=add(phase_stats_.phases,plan.phase_count);++phase_stats_.plans;
        programs_.emplace(p.get(),std::make_shared<PhaseProgram>(std::move(plan)));
    }
    const PhaseDiagnostics& phase_stats()const{return phase_stats_;}
    const memory_phase::Plan& plan_for(const Program& p)const {auto it=programs_.find(&p);require(it!=programs_.end(),"registered phase plan");return it->second->plan;}
    Cycle now() const {return now_;}
    const Stats& stats() const {return stats_;}
    std::size_t pending_tokens() const {return pending_tokens_.size();}
    std::size_t resident_ctas() const {return ctas_.size();}
    std::optional<Cycle> next_event_cycle() const {return events_.empty()?std::nullopt:std::optional<Cycle>(events_.top().cycle);}
    void add_cta(CtaId id,std::shared_ptr<const Program> program,const std::vector<unsigned>& warp_sp,Cycle ready=0) {
        require(program && program->outgoing.size()==program->groups.size() && program->indegree.size()==program->groups.size(),"finalized shared program");
        require(!used_cta_ids_.count(id) && !ctas_.count(id) && warp_sp.size()==program->warp_groups.size() && ready>=now_,"CTA admission");
        for(auto sp:warp_sp)require(sp<sps_.size() && program->pipeline_count<=sps_[sp].pipeline_free.size(),"CTA resource mapping");
        auto found=programs_.find(program.get());require(found!=programs_.end(),"phase program must be registered before CTA admission");
        auto c=std::make_unique<Cta>();c->program=std::move(program);c->phases=found->second;
        c->warp_sp=warp_sp;c->cursor.resize(warp_sp.size());c->states.resize(c->phases->plan.units.size());c->phase_states.resize(c->states.size());
        for(std::size_t g=0;g<c->states.size();++g){c->states[g].remaining=c->phases->indegree[g];c->states[g].ready=ready;
            if(c->phases->plan.units[g].phase)c->phase_states[g]=std::make_unique<PhaseState>();}
        ctas_.emplace(id,std::move(c));used_cta_ids_.insert(id);++stats_.ctas_added;
        for(unsigned w=0;w<warp_sp.size();++w){sps_[warp_sp[w]].warps.push_back({id,w});wake_sp(warp_sp[w],ready);}
    }
    // Queue notifications before step_once if they coincide with its next
    // cycle, so completion/retry wake precedes same-cycle scheduling.
    void complete(Token token,Cycle at) {
        auto it=pending_tokens_.find(token);require(it!=pending_tokens_.end() && !it->second.completion_queued,"unknown/duplicate external completion");
        require(at>=now_,"backdated external completion");it->second.completion_queued=true;
        auto& owner=cta(it->second.cta);const unsigned group=it->second.group;
        if(phase(owner,group)) {
            const CtaId id=it->second.cta;auto& st=owner.states[group];require(st.pending>0,"phase return pending underflow");
            owner.phase_states[group]->return_frontier=std::max(owner.phase_states[group]->return_frontier,at);
            --st.pending;++phase_stats_.returns;pending_tokens_.erase(it);queue_return_tail(id,group,at);return;
        }
        queue(EventKind::CompletePart,at,it->second.cta,it->second.group,0,token);
    }
    void wake_retry(CtaId id,unsigned group,Cycle at) {
        auto& c=cta(id);require(group<c.states.size() && c.states[group].retry_blocked,"retry wake requires blocked group");
        queue(EventKind::RetryWake,at,id,group,0,c.states[group].retry_generation);
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
                if(st.retry_blocked && e.token==st.retry_generation){st.retry_blocked=false;st.ready=now_;wake_sp(c.warp_sp[description(c,e.group).warp],now_);}continue;
            }
            auto& c=cta(e.cta);auto& st=c.states[e.group];
            if(e.kind==EventKind::ReleaseIssue) {release(e.cta,e.group,EdgeType::Issue);continue;}
            if(e.kind==EventKind::PhaseCheckpoint) {
                auto& lease=sps_[e.sp].lease;
                require(lease&&lease->cta==e.cta&&lease->group==e.group&&lease->generation==e.token,"stale phase checkpoint");
                phase_checkpoint(e.cta,e.group,e.sp);continue;
            }
            if(e.kind==EventKind::PhaseCompute) {
                auto& ps=*c.phase_states[e.group];require(st.issue_done&&!ps.compute_done,"phase compute tail closure");
                ps.compute_done=true;++phase_stats_.compute_tails;release_gate(e.cta,e.group,memory_phase::Milestone::ComputeTail);maybe_finish_phase(e.cta,e.group);continue;
            }
            if(e.kind==EventKind::PhaseReturns) {
                auto& ps=*c.phase_states[e.group];require(st.issue_done&&st.pending==0&&!ps.returns_done,"phase return tail closure");
                ps.returns_done=true;++phase_stats_.return_tails;release_gate(e.cta,e.group,memory_phase::Milestone::ReadReturnsTail);maybe_finish_phase(e.cta,e.group);continue;
            }
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
        ctas_.erase(id);
    }
};
} // namespace packet_runtime
#include "../tilegen-prefill-compute-native-r1/pref_support/selector.h"
namespace packet_runtime {
class Runtime {
    std::unique_ptr<LegacyRuntime> legacy_;
    std::unique_ptr<PhaseEngine> phase_;
    std::unique_ptr<prefill_compute_runtime::KernelEngine> prefill_;
    std::unordered_map<const Program*,unsigned> prefill_program_indices_;
    PhaseDiagnostics off_diagnostics_{};
public:
    using Stop=LegacyRuntime::Stop;
    Runtime(unsigned sp_count,unsigned pipelines,MemoryPort* memory=nullptr,Cycle initial_cycle=0,unsigned phase_width=1,
            std::shared_ptr<const prefill_compute_runtime::KernelSelection> prefill_selection={}) {
        require(phase_width==1||phase_width==4||phase_width==16,"phase width 1/4/16");
        if(prefill_selection&&prefill_selection->enabled()) {
            require(phase_width==1,"Prefill and GEMV phases cannot be active together");
            require(prefill_selection->sp_count()==sp_count&&prefill_selection->pipeline_count()==pipelines,"Prefill selection/runtime resources differ");
            require(prefill_selection->template_count()<=std::numeric_limits<unsigned>::max(),"Prefill template index domain");
            for(unsigned i=0;i<prefill_selection->template_count();++i) {
                const auto& program=prefill_selection->original(i);
                require(prefill_program_indices_.emplace(program.get(),i).second,"duplicate Prefill Program pointer");
            }
            prefill_=std::make_unique<prefill_compute_runtime::KernelEngine>(std::move(prefill_selection),memory,initial_cycle);
        } else if(phase_width==1)legacy_=std::make_unique<LegacyRuntime>(sp_count,pipelines,memory,initial_cycle);
        else phase_=std::make_unique<PhaseEngine>(sp_count,pipelines,memory,initial_cycle,phase_width);
    }
    void register_phase_program(std::shared_ptr<const Program> p,const std::vector<memory_phase::SourceFact>& f) {
        if(phase_)phase_->register_phase_program(std::move(p),f);
    }
    const PhaseDiagnostics& phase_stats()const{return phase_?phase_->phase_stats():off_diagnostics_;}
    const memory_phase::Plan* phase_plan(const Program& p)const{return phase_?&phase_->plan_for(p):nullptr;}
    const prefill_compute_runtime::ActualStats* prefill_stats()const{return prefill_?prefill_->actual_stats():nullptr;}
    bool prefill_enabled()const{return bool(prefill_);}
    Cycle now()const{if(prefill_)return prefill_->now();return phase_?phase_->now():legacy_->now();}
    const Stats& stats()const{if(prefill_)return prefill_->stats();return phase_?phase_->stats():legacy_->stats();}
    std::size_t pending_tokens()const{if(prefill_)return prefill_->pending_tokens();return phase_?phase_->pending_tokens():legacy_->pending_tokens();}
    std::size_t resident_ctas()const{if(prefill_)return prefill_->resident_ctas();return phase_?phase_->resident_ctas():legacy_->resident_ctas();}
    std::optional<Cycle> next_event_cycle()const{if(prefill_)return prefill_->next_event_cycle();return phase_?phase_->next_event_cycle():legacy_->next_event_cycle();}
    void add_cta(CtaId id,std::shared_ptr<const Program> p,const std::vector<unsigned>& map,Cycle ready=0){
        if(prefill_){const auto i=prefill_program_indices_.find(p.get());require(i!=prefill_program_indices_.end(),"Prefill Program not in kernel selection");prefill_->add_cta(id,i->second,map,ready);return;}
        if(phase_)phase_->add_cta(id,std::move(p),map,ready);else legacy_->add_cta(id,std::move(p),map,ready);
    }
    void complete(Token t,Cycle at){if(prefill_){prefill_->complete(t,at);return;}if(phase_)phase_->complete(t,at);else legacy_->complete(t,at);}
    void wake_retry(CtaId id,unsigned g,Cycle at){if(prefill_){prefill_->wake_retry(id,g,at);return;}if(phase_)phase_->wake_retry(id,g,at);else legacy_->wake_retry(id,g,at);}
    bool step_once(){if(prefill_)return prefill_->step_once();return phase_?phase_->step_once():legacy_->step_once();}
    void advance_to(Cycle c){if(prefill_){prefill_->advance_to(c);return;}if(phase_)phase_->advance_to(c);else legacy_->advance_to(c);}
    Stop run_known_events(std::uint64_t n){if(prefill_)return prefill_->run_known_events(n);return phase_?static_cast<Stop>(phase_->run_known_events(n)):legacy_->run_known_events(n);}
    bool cta_complete(CtaId id)const{if(prefill_)return prefill_->cta_complete(id);return phase_?phase_->cta_complete(id):legacy_->cta_complete(id);}
    void retire_cta(CtaId id){if(prefill_){prefill_->retire_cta(id);return;}if(phase_)phase_->retire_cta(id);else legacy_->retire_cta(id);}
};

} // namespace packet_runtime
