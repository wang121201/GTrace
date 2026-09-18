#pragma once
#include "engine.h"

namespace prefill_compute_runtime {
struct TemplateInput {
    std::shared_ptr<const Program> program;
    std::vector<pc::SourceFact> facts;
};
// Prepared before the first CTA admission. Placement is the actual set of SM
// placements allowed for this kernel, not an inferred count or average class.
class KernelSelection {
    recurrence::DomainDecision decision_;
    std::vector<std::shared_ptr<const Program>> original_;
    std::vector<std::shared_ptr<const CompiledProgram>> compiled_;
    std::vector<std::vector<unsigned>> placements_;
    unsigned sp_count_=0,pipeline_count_=0;
public:
    KernelSelection(recurrence::Domain domain,unsigned sp_count,unsigned pipeline_count,
                    const std::vector<TemplateInput>& templates,
                    std::vector<std::vector<unsigned>> placements)
        :decision_(recurrence::DomainDecision::LegacyUnsupported),placements_(std::move(placements)),sp_count_(sp_count),pipeline_count_(pipeline_count) {
        require(sp_count>0&&pipeline_count>0&&!templates.empty(),"kernel selection resources/templates");
        if(!placements_.empty()&&placements_.front().size()==4) {
            const auto& first=placements_.front();domain.warp_to_global_SP={first[0],first[1],first[2],first[3]};
        }
        decision_=recurrence::domain_decision(domain);
        for(const auto& t:templates){require(bool(t.program),"kernel original program");original_.push_back(t.program);}
        if(decision_!=recurrence::DomainDecision::Supported)return; // no planner/helper on Legacy path
        bool supported=sp_count%4==0&&pipeline_count==6&&!placements_.empty();
        for(const auto& t:templates)supported=supported&&t.program->warp_groups.size()==4&&t.program->pipeline_count==6;
        for(const auto& placement:placements_) {
            if(placement.size()!=4){supported=false;continue;}
            domain.warp_to_global_SP={placement[0],placement[1],placement[2],placement[3]};
            supported=supported&&recurrence::domain_decision(domain)==recurrence::DomainDecision::Supported;
            for(auto sp:placement)supported=supported&&sp<sp_count;
        }
        if(!supported){decision_=recurrence::DomainDecision::LegacyUnsupported;return;}
        // Invalid source/typed input remains an error. Only declared domain
        // exclusions fall back; source validation errors are never swallowed.
        for(const auto& t:templates)compiled_.push_back(compile_program(std::make_shared<const pc::Plan>(pc::make_plan(t.program,t.facts,domain.width))));
    }
    recurrence::DomainDecision decision()const{return decision_;}
    bool enabled()const{return decision_==recurrence::DomainDecision::Supported;}
    unsigned sp_count()const{return sp_count_;}
    unsigned pipeline_count()const{return pipeline_count_;}
    std::size_t template_count()const{return original_.size();}
    const std::shared_ptr<const Program>& original(unsigned i)const{return original_.at(i);}
    const std::shared_ptr<const CompiledProgram>& compiled(unsigned i)const{return compiled_.at(i);}
    bool allows(const std::vector<unsigned>& placement)const{return std::find(placements_.begin(),placements_.end(),placement)!=placements_.end();}
};
// Whole-kernel selection only. This component does not replace or select the
// separate existing GEMV memory-phase engine in the production driver.
class KernelEngine {
    std::shared_ptr<const KernelSelection> selection_;
    std::unique_ptr<LegacyRuntime> legacy_;
    std::unique_ptr<PrefillComputeEngine> compute_;
public:
    KernelEngine(std::shared_ptr<const KernelSelection> selection,MemoryPort* port=nullptr,Cycle initial_cycle=0):selection_(std::move(selection)) {
        require(bool(selection_),"kernel selection");
        if(selection_->enabled())compute_=std::make_unique<PrefillComputeEngine>(selection_->sp_count(),selection_->pipeline_count(),port,initial_cycle);
        else legacy_=std::make_unique<LegacyRuntime>(selection_->sp_count(),selection_->pipeline_count(),port,initial_cycle);
    }
    bool enabled()const{return bool(compute_);}
    void add_cta(CtaId id,unsigned template_index,const std::vector<unsigned>& warp_sp,Cycle ready=0) {
        if(compute_){require(selection_->allows(warp_sp),"placement not preflighted before kernel");compute_->add_cta(id,selection_->compiled(template_index),warp_sp,ready);}
        else legacy_->add_cta(id,selection_->original(template_index),warp_sp,ready);
    }
    Cycle now()const{return compute_?compute_->now():legacy_->now();}
    const Stats& stats()const{return compute_?compute_->stats():legacy_->stats();}
    std::size_t pending_tokens()const{return compute_?compute_->pending_tokens():legacy_->pending_tokens();}
    std::size_t resident_ctas()const{return compute_?compute_->resident_ctas():legacy_->resident_ctas();}
    std::optional<Cycle> next_event_cycle()const{return compute_?compute_->next_event_cycle():legacy_->next_event_cycle();}
    void complete(Token token,Cycle at){if(compute_)compute_->complete(token,at);else legacy_->complete(token,at);}
    void wake_retry(CtaId id,unsigned original_group,Cycle at){if(compute_)compute_->wake_retry(id,original_group,at);else legacy_->wake_retry(id,original_group,at);}
    void advance_to(Cycle at){if(compute_)compute_->advance_to(at);else legacy_->advance_to(at);}
    bool step_once(){return compute_?compute_->step_once():legacy_->step_once();}
    LegacyRuntime::Stop run_known_events(std::uint64_t n){return compute_?static_cast<LegacyRuntime::Stop>(compute_->run_known_events(n)):legacy_->run_known_events(n);}
    bool cta_complete(CtaId id)const{return compute_?compute_->cta_complete(id):legacy_->cta_complete(id);}
    void retire_cta(CtaId id){if(compute_)compute_->retire_cta(id);else legacy_->retire_cta(id);}
    const ActualStats* actual_stats()const{return compute_?&compute_->actual_stats():nullptr;}
};
} // namespace prefill_compute_runtime
