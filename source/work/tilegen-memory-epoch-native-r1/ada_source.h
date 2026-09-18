#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tiny_full {
// One instance per SM. This is the source-posting stage only: returned IDs
// have posted all source lines, but may still await L2 and SRAM responses.
// No DAGNode, address program, cache policy or completion latency is owned here.
class AdaSource {
public:
    using Cycle=std::uint64_t;
    struct Rate { std::uint64_t numerator_bytes=0,denominator_cycles=0; };
    struct RequestView { int id,warp_token;Cycle ready_cycle;std::size_t line_count,next_line; };
    struct SourceView { int warp_token;std::uint64_t credit_units;std::vector<RequestView> requests; };
    struct StateView {
        Cycle current_cycle;std::uint64_t aggregate_credit_units;
        std::size_t round_robin_source;std::vector<RequestView> setup;
        std::vector<SourceView> sources;
    };
    struct Statistics {
        std::uint64_t enqueued=0,posted=0,line_attempts=0,lines_accepted=0,line_rejections=0,steps=0;
    };
private:
    struct Request {int id,warp_token;Cycle ready_cycle;std::size_t line_count,next_line=0;};
    struct Source {int warp_token;std::uint64_t credit_units=0;std::deque<Request> requests;};
    Rate per_source_,aggregate_;
    std::uint64_t source_cost_,aggregate_cost_,source_cap_,aggregate_cap_;
    Cycle current_cycle_;
    std::deque<Request> setup_;
    std::vector<Source> sources_; // first-ready order, retained even when empty
    std::size_t round_robin_source_=0,pending_requests_=0;
    std::uint64_t aggregate_credit_units_=0,pending_lines_=0;
    Statistics stats_;
    static void require(bool condition,const char* why) {
        if(!condition)throw std::invalid_argument(why);
    }
    static std::uint64_t add(std::uint64_t a,std::uint64_t b) {
        if(b>std::numeric_limits<std::uint64_t>::max()-a)
            throw std::overflow_error("Ada source integer addition overflow");
        return a+b;
    }
    static std::uint64_t cost(Rate r) {
        require(r.numerator_bytes>0&&r.denominator_cycles>0,"positive Ada source rate required");
        if(r.denominator_cycles>std::numeric_limits<std::uint64_t>::max()/128)
            throw std::overflow_error("bulk-copy fixed-point line cost overflow");
        const auto result=128*r.denominator_cycles;
        require(r.numerator_bytes<result,"Ada LDGSTS contract requires a sub-line-per-cycle rate");
        return result;
    }
    static std::uint64_t cap(Rate r,std::uint64_t line_cost) {
        if(line_cost>std::numeric_limits<std::uint64_t>::max()-(r.numerator_bytes-1))
            throw std::overflow_error("bulk-copy token cap overflow");
        return line_cost+r.numerator_bytes-1;
    }
    static std::uint64_t add_capped(std::uint64_t value,std::uint64_t increment,std::uint64_t limit) {
        if(value>=limit||increment>=limit-value)return limit;
        return value+increment;
    }
    Source& find_or_create(int warp_token) {
        for(auto& s:sources_)if(s.warp_token==warp_token)return s;
        sources_.push_back({warp_token,0,{}});return sources_.back();
    }
    bool active() const {
        for(const auto& s:sources_)if(!s.requests.empty())return true;
        return false;
    }
    static RequestView view(const Request& r) {
        return {r.id,r.warp_token,r.ready_cycle,r.line_count,r.next_line};
    }
public:
    AdaSource(Rate per_source,Rate aggregate_sm,Cycle initial_cycle=0)
        :per_source_(per_source),aggregate_(aggregate_sm),
         source_cost_(cost(per_source)),aggregate_cost_(cost(aggregate_sm)),
         source_cap_(cap(per_source,source_cost_)),aggregate_cap_(cap(aggregate_sm,aggregate_cost_)),
         current_cycle_(initial_cycle) {
        // Matches BulkCopyEngineConfig::ada_ldgsts validation, including its
        // long-double comparison rather than a different rational criterion.
        const long double a=static_cast<long double>(per_source.numerator_bytes)/per_source.denominator_cycles;
        const long double b=static_cast<long double>(aggregate_sm.numerator_bytes)/aggregate_sm.denominator_cycles;
        require(b>=a,"Ada LDGSTS aggregate SM rate is below the per-source rate");
    }
    static Cycle ready_after_dispatch(Cycle dispatch,int setup_latency) {
        require(setup_latency>=0,"nonnegative Ada source setup required");
        return add(dispatch,std::max<std::uint64_t>(1,std::uint64_t(setup_latency)));
    }
    void enqueue(int id,int warp_token,Cycle ready_cycle,std::size_t line_count) {
        require(id>=0&&warp_token>=0,"nonnegative Ada source identity required");
        require(ready_cycle>current_cycle_,"Ada source enqueue must follow this cycle's service");
        require(line_count<=std::size_t(std::numeric_limits<int>::max()),"Ada source line count exceeds original int domain");
        const auto pending=add(pending_lines_,line_count);
        if(pending_requests_==std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("Ada source pending request count overflow");
        setup_.push_back({id,warp_token,ready_cycle,line_count,0});
        ++pending_requests_;pending_lines_=pending;++stats_.enqueued;
    }
    // Exactly one call per core cycle. The callback must not re-enter/mutate
    // this object, must not complete the instruction, and must return true iff
    // this one real source line was accepted by the original L2 admission path.
    // Exceptions are fail-stop, as in the original bulk service; no rollback.
    template<class TryLine> std::vector<int> step(Cycle cycle,TryLine&& try_line) {
        require(cycle==add(current_cycle_,1),"Ada source step must be consecutive");
        current_cycle_=cycle;++stats_.steps;std::vector<int> posted;
        // FIFO setup promotion deliberately retains head-of-line behavior even
        // if callers provide later-enqueued requests with earlier ready times.
        while(!setup_.empty()&&setup_.front().ready_cycle<=cycle) {
            Request request=std::move(setup_.front());setup_.pop_front();
            find_or_create(request.warp_token).requests.push_back(std::move(request));
        }
        if(!active()){aggregate_credit_units_=0;return posted;}
        for(auto& source:sources_) {
            if(source.requests.empty()){source.credit_units=0;continue;}
            source.credit_units=add_capped(source.credit_units,per_source_.numerator_bytes,source_cap_);
        }
        aggregate_credit_units_=add_capped(aggregate_credit_units_,aggregate_.numerator_bytes,aggregate_cap_);
        while(!sources_.empty()) {
            bool found=false;std::size_t selected=0;
            for(std::size_t offset=0;offset<sources_.size();++offset) {
                const auto index=(round_robin_source_+offset)%sources_.size();
                const auto& source=sources_[index];if(source.requests.empty())continue;
                const auto& request=source.requests.front();
                const auto a=request.line_count==0?0:source_cost_;
                const auto b=request.line_count==0?0:aggregate_cost_;
                if(source.credit_units<a||aggregate_credit_units_<b)continue;
                found=true;selected=index;break;
            }
            if(!found)break;
            auto& source=sources_[selected];auto& request=source.requests.front();
            bool accepted=true;
            if(request.line_count!=0){
                ++stats_.line_attempts;accepted=try_line(request.id,request.next_line);
                if(!accepted)++stats_.line_rejections;
            }
            // Rejection stops this entire SM loop. It neither spends credits
            // nor advances RR or the line cursor, even if another source fits.
            if(!accepted)break;
            if(request.line_count!=0){
                source.credit_units-=source_cost_;aggregate_credit_units_-=aggregate_cost_;
                --pending_lines_;++stats_.lines_accepted;
            }
            ++request.next_line;round_robin_source_=(selected+1)%sources_.size();
            if(request.next_line>=request.line_count){
                posted.push_back(request.id);source.requests.pop_front();
                --pending_requests_;++stats_.posted;
                if(source.requests.empty())source.credit_units=0;
            }
        }
        return posted;
    }
    struct EpochStatistics {
        std::uint64_t service_calls=0,virtual_slots=0,posting_checks=0,rejection_stops=0;
        std::uint64_t blocked_epochs=0,blocked_prefix_slots=0;
    };
private:
    EpochStatistics epoch_stats_;
    bool epoch_previous_rejection_=false;
public:
    const EpochStatistics& epoch_statistics()const{return epoch_stats_;}
    bool epoch_previous_rejection()const{return epoch_previous_rejection_;}
    // The caller continued real dispatch/enqueue between the two boundaries.
    // Slots only accrue finite credit and select eligible original heads.
    // Every actual try_line callback happens NOW at the real boundary B.
    template<class TryLine> std::vector<int> step_epoch(Cycle cycle,unsigned span,TryLine&& try_line) {
        require(span>=1&&span<=8,"Ada epoch span must be 1..8");
        require(cycle==add(current_cycle_,span),"Ada epoch must cover the next contiguous interval");
        const Cycle previous=current_cycle_;
        current_cycle_=cycle;++stats_.steps;
        epoch_stats_.service_calls=add(epoch_stats_.service_calls,1);
        epoch_stats_.virtual_slots=add(epoch_stats_.virtual_slots,span);
        const bool previously_rejected=epoch_previous_rejection_;
        if(previously_rejected){
            epoch_stats_.blocked_epochs=add(epoch_stats_.blocked_epochs,1);
            epoch_stats_.blocked_prefix_slots=add(epoch_stats_.blocked_prefix_slots,span-1);
        }
        std::vector<int> posted;
        bool rejected=false;
        for(unsigned offset=1;offset<=span;++offset) {
            const Cycle slot=add(previous,offset);
            // Keep FIFO/HOL promotion after a rejection as well. A newly
            // ready source gets only the remaining eligible resource slots.
            while(!setup_.empty()&&setup_.front().ready_cycle<=slot) {
                Request request=std::move(setup_.front());setup_.pop_front();
                find_or_create(request.warp_token).requests.push_back(std::move(request));
            }
            if(!active()){aggregate_credit_units_=0;continue;}
            for(auto& source:sources_) {
                if(source.requests.empty()){source.credit_units=0;continue;}
                source.credit_units=add_capped(source.credit_units,per_source_.numerator_bytes,source_cap_);
            }
            aggregate_credit_units_=add_capped(aggregate_credit_units_,aggregate_.numerator_bytes,aggregate_cap_);
            // No retry-to-stability, hidden backend service, or unlimited
            // carry after failure. Remaining slots only promote and accrue.
            if(rejected||(previously_rejected&&offset<span))continue;
            while(!sources_.empty()) {
                epoch_stats_.posting_checks=add(epoch_stats_.posting_checks,1);
                bool found=false;std::size_t selected=0;
                for(std::size_t offset=0;offset<sources_.size();++offset) {
                    const auto index=(round_robin_source_+offset)%sources_.size();
                    const auto& source=sources_[index];if(source.requests.empty())continue;
                    const auto& request=source.requests.front();
                    const auto a=request.line_count==0?0:source_cost_;
                    const auto b=request.line_count==0?0:aggregate_cost_;
                    if(source.credit_units<a||aggregate_credit_units_<b)continue;
                    found=true;selected=index;break;
                }
                if(!found)break;
                auto& source=sources_[selected];auto& request=source.requests.front();
                bool accepted=true;
                if(request.line_count!=0){
                    ++stats_.line_attempts;accepted=try_line(request.id,request.next_line);
                    if(!accepted)++stats_.line_rejections;
                }
                if(!accepted){
                    rejected=true;epoch_stats_.rejection_stops=add(epoch_stats_.rejection_stops,1);break;
                }
                if(request.line_count!=0){
                    source.credit_units-=source_cost_;aggregate_credit_units_-=aggregate_cost_;
                    --pending_lines_;++stats_.lines_accepted;
                }
                ++request.next_line;round_robin_source_=(selected+1)%sources_.size();
                if(request.next_line>=request.line_count){
                    posted.push_back(request.id);source.requests.pop_front();
                    --pending_requests_;++stats_.posted;
                    if(source.requests.empty())source.credit_units=0;
                }
            }
            // As in old step, aggregate credit is not reset on the final
            // pop: the NEXT idle resource-slot entry performs that reset.
        }
        epoch_previous_rejection_=rejected;
        return posted;
    }

    bool has_work()const{return pending_requests_!=0;}
    std::size_t pending_requests()const{return pending_requests_;}
    std::uint64_t pending_lines()const{return pending_lines_;}
    Cycle current_cycle()const{return current_cycle_;}
    const Statistics& statistics()const{return stats_;}
    StateView state_for_audit()const {
        StateView out{current_cycle_,aggregate_credit_units_,round_robin_source_,{}, {}};
        for(const auto&r:setup_)out.setup.push_back(view(r));
        for(const auto&s:sources_){SourceView v{s.warp_token,s.credit_units,{}};for(const auto&r:s.requests)v.requests.push_back(view(r));out.sources.push_back(std::move(v));}
        return out;
    }
};
} // namespace tiny_full
