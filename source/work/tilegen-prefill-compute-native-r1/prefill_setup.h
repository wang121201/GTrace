#pragma once
// Included after the qualified tiny binding/import and new Runtime definitions.
namespace hybrid_full {
class PrefillSetup {
    using U=std::uint64_t;
    using J=nlohmann::json;
    using TemplateInput=prefill_compute_runtime::TemplateInput;
    const tiny_full::KernelBinding& binding_;
    const GTSim::SimulatorConfig& cfg_;
    std::string family_,reason_;
    unsigned requested_=1;
    bool inspect_=false,all_four_warps_=true;
    U checked_ctas_=0,segments_=0,merged_=0,gates_=0,internal_typed_=0,internal_logical_=0;
    U original_typed_=0,compute_subops_=0,fallback_segments_=0;
    std::vector<TemplateInput> inputs_;
    std::map<const packet_runtime::Program*,unsigned> indices_;
    std::set<std::vector<unsigned>> placements_;
    std::shared_ptr<const prefill_compute_runtime::KernelSelection> selection_;
public:
    PrefillSetup(const tiny_full::KernelBinding& binding,const std::string& family,
                 unsigned q,const GTSim::SimulatorConfig& cfg,unsigned requested)
        :binding_(binding),cfg_(cfg),family_(family),requested_(requested) {
        tiny_full::p::need(requested==1||requested==4||requested==16,"Prefill compute width 1/4/16");
        if(requested==1){reason_="width1_prefill_disabled";return;}
        if(family!="GEMMGate"&&family!="GEMMDown"){reason_="family_unchanged";return;}
        if(q!=16){reason_="q16_required_legacy_fallback";return;}
        // This identity check runs before any device begin_kernel or operation.
        // Resource values below are actual binding methods, not an average CTA.
        const auto evidence=binding.evidence();
        tiny_full::p::need(evidence.at("family")==family,"Prefill preflight family binding");
        inspect_=true;reason_="resource_domain_legacy_fallback";
    }
    void observe(U cta,const std::shared_ptr<const packet_runtime::Program>& program) {
        if(!inspect_)return;
        namespace p=tiny_full::p;namespace pc=prefill_compute_phase;
        p::need(cta==checked_ctas_,"Prefill preflight complete ordered CTA coverage");++checked_ctas_;
        const auto warps=binding_.warps(cta);all_four_warps_=all_four_warps_&&warps==4;
        // Only the bounded four-warp placement is retained. Any incompatible
        // class selects Legacy for the whole kernel before first admission.
        if(warps==4) {
            std::vector<unsigned> placement;
            for(unsigned w=0;w<4;++w)placement.push_back(unsigned(cta%U(cfg_.num_sms))*4+
                unsigned(binding_.warp_token(cta,w,unsigned(cfg_.num_sms))%4));
            placements_.insert(std::move(placement));
        }
        auto found=indices_.find(program.get());if(found!=indices_.end())return;
        p::need(inputs_.size()<=UINT32_MAX,"Prefill template index range");
        TemplateInput input;input.program=program;
        const auto source=binding_.nodes(cta);input.facts.reserve(source.size());
        for(const auto& node:source) {
            p::need(node.id==input.facts.size(),"Prefill dense source fact identity");
            pc::SourceKind kind;
            switch(node.kind) {
                case tiny_full::Kind::Compute:kind=pc::SourceKind::Compute;break;
                case tiny_full::Kind::Tensor:kind=pc::SourceKind::Tensor;break;
                case tiny_full::Kind::Control:kind=pc::SourceKind::Control;break;
                case tiny_full::Kind::Global:kind=pc::SourceKind::Global;break;
                case tiny_full::Kind::Shared:kind=pc::SourceKind::Shared;break;
                case tiny_full::Kind::AsyncCopy:kind=pc::SourceKind::AsyncCopy;break;
                case tiny_full::Kind::Barrier:kind=pc::SourceKind::Barrier;break;
                default:throw std::invalid_argument("unsupported Prefill source kind");
            }
            input.facts.push_back({node.id,node.warp,kind,node.compute_elements,node.tensor_fma});
        }
        indices_.emplace(program.get(),unsigned(inputs_.size()));inputs_.push_back(std::move(input));
    }
    void finish(const std::vector<std::shared_ptr<const packet_runtime::Program>>& cta_programs) {
        if(!inspect_)return;
        namespace p=tiny_full::p;namespace rr=prefill_resource_recurrence;
        p::need(checked_ctas_==binding_.ctas()&&cta_programs.size()==checked_ctas_,"all CTA placements preflighted");
        rr::Domain domain;domain.family=family_=="GEMMGate"?rr::Family::Gate:rr::Family::Down;
        domain.width=requested_;domain.warps_per_CTA=all_four_warps_?4:0;
        domain.resident_limit=binding_.resident_limit();domain.SPs_per_SM=4;domain.pipelines_per_SP=6;
        // Include a valid representative map in the declared domain as well as
        // the full set checked by KernelSelection (component r2 contract).
        domain.warp_to_global_SP={0,1,2,3};
        const std::vector<std::vector<unsigned>> placements(placements_.begin(),placements_.end());
        selection_=std::make_shared<const prefill_compute_runtime::KernelSelection>(domain,
            unsigned(cfg_.num_sms)*4,6,inputs_,placements);
        inputs_.clear(); // facts were needed only by make_plan, not runtime state
        if(!selection_->enabled())return;
        reason_="GateDown_q16_four_warps_resident1_preflight_pass";
        struct Totals {U segments=0,merged=0,gates=0,internal_typed=0,internal_logical=0,typed=0,subops=0,fallback=0;};
        std::map<const packet_runtime::Program*,Totals> cached;
        for(const auto& entry:indices_) {
            const auto& compiled=*selection_->compiled(entry.second);const auto& plan=compiled.plan();
            Totals t;t.segments=plan.segments.size();t.merged=plan.stats.final_multi_segments;t.gates=plan.gates.size();
            t.internal_typed=plan.stats.new_internal_typed_group_edges;t.internal_logical=plan.stats.new_internal_logical_edges;
            t.typed=compiled.source().edges.size();t.fallback=plan.stats.fallback_original_groups;
            for(unsigned s=0;s<plan.segments.size();++s)t.subops=p::add(t.subops,compiled.compute_subops(s));
            cached.emplace(entry.first,t);
        }
        for(const auto& program:cta_programs) {
            const auto& t=cached.at(program.get());segments_=p::add(segments_,t.segments);merged_=p::add(merged_,t.merged);
            gates_=p::add(gates_,t.gates);internal_typed_=p::add(internal_typed_,t.internal_typed);
            internal_logical_=p::add(internal_logical_,t.internal_logical);original_typed_=p::add(original_typed_,t.typed);
            compute_subops_=p::add(compute_subops_,t.subops);fallback_segments_=p::add(fallback_segments_,t.fallback);
        }
    }
    const std::shared_ptr<const prefill_compute_runtime::KernelSelection>& selection()const{return selection_;}
    J report(const packet_runtime::Runtime& runtime,U nodes,U groups,U edges,U q16_internal,U elements,U fma)const {
        namespace p=tiny_full::p;
        const bool enabled=selection_&&selection_->enabled();
        p::need(runtime.prefill_enabled()==enabled,"Prefill actual engine selection");
        const auto* actual=runtime.prefill_stats();const auto& s=runtime.stats();
        if(enabled) {
            p::need(actual&&actual->segments_completed==segments_&&actual->merged_segments_issued==merged_&&
                actual->merged_segments_completed==merged_&&actual->actual_gate_updates==gates_&&
                actual->internal_typed_group_contributions==internal_typed_&&actual->internal_logical_edge_contributions==internal_logical_&&
                actual->segment_state_slots_current==0,"Prefill actual segment lifecycle/gate closure");
            p::need(s.groups_completed==groups&&s.logical_members_completed==nodes&&
                s.compute_elements_completed==elements&&s.tensor_fma_completed==fma&&
                s.typed_group_events_released==original_typed_&&s.logical_external_edges_released==edges&&
                s.compute_subops_issued==compute_subops_,"Prefill original group/typed/subop work closure");
        } else p::need(actual==nullptr,"Legacy/GEMV has no Prefill mutable state");
        const prefill_compute_runtime::ActualStats zero{};const auto& a=actual?*actual:zero;
        return {{"requested_width",requested_},{"effective_width",enabled?requested_:1},{"enabled",enabled},
            {"selection_reason",reason_},{"family",family_},{"preflight_CTAs",checked_ctas_},
            {"preflight_template_classes",indices_.size()},{"preflight_unique_placements",placements_.size()},
            {"original_groups",groups},{"original_members",nodes},{"original_external_logical_edges",edges},
            {"original_packet_internal_logical_edges",q16_internal},{"original_compute_elements",elements},{"original_tensor_FMA",fma},
            {"expected_original_typed_contributions",original_typed_},{"expected_original_compute_subops",compute_subops_},
            {"expected_segments",segments_},{"expected_merged_segments",merged_},{"expected_actual_gate_updates",gates_},
            {"expected_internal_typed_contributions",internal_typed_},{"expected_internal_logical_contributions",internal_logical_},
            {"cycle_fallback_original_groups",fallback_segments_},{"actual_segments_completed",a.segments_completed},
            {"actual_merged_entries",a.merged_segments_issued},{"actual_merged_completions",a.merged_segments_completed},
            {"actual_gate_updates",a.actual_gate_updates},{"internal_typed_contributions",a.internal_typed_group_contributions},
            {"internal_logical_contributions",a.internal_logical_edge_contributions},
            {"segment_state_slots_current",a.segment_state_slots_current},{"segment_state_slots_peak",a.segment_state_slots_peak},
            {"actual_event_pops",enabled?s.events_processed:0},{"actual_schedule_invocations",enabled?s.schedule_invocations:0},
            {"actual_warp_candidate_visits",enabled?s.warp_candidate_visits:0},
            {"work_counter_scope","ON runtime group/typed counters are original contributions; actual segment/gate/event work is separately listed; merged compute_subops credited at IssueTail."},
            {"memory_phase_scope","The separate memory_phase object describes GEMV only; its OFF expected_runtime_units is the original imported-group contribution census, not Prefill segment-state slots."},
            {"resource_scope","ON: four independent SPs, six pipelines each, one warp per SP, one resident CTA per SM; original issue span differs from pipeline occupancy."},
            {"timing_contract","ON aggregates incoming readiness at entry and outgoing visibility at Issue/Completion tails; original memory instructions and port callbacks retained; not Legacy timing equivalence."},
            {"hardware_timing_qualified",false}};
    }
};
} // namespace hybrid_full
