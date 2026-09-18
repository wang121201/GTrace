#pragma once
// Program and LegacyRuntime are supplied by the including derived runtime.
#include "../../tilegen-prefill-compute-phase-r1/runtime-recurrence-r1/resource_recurrence.h"

namespace prefill_compute_runtime {
namespace rt=packet_runtime;
namespace pc=prefill_compute_phase;
namespace recurrence=prefill_resource_recurrence;
using namespace packet_runtime;
struct InternalContributions {std::array<std::uint64_t,2> typed{},logical{};};
class CompiledProgram {
    std::shared_ptr<const pc::Plan> plan_;
    std::vector<std::vector<unsigned>> outgoing_;
    std::vector<std::uint32_t> indegree_;
    std::vector<std::optional<recurrence::PreparedRecipe>> recipes_;
    std::vector<InternalContributions> internal_;
    std::vector<std::uint64_t> compute_subops_;
public:
    // Input must be the immutable output of the qualified make_plan. No CTA
    // dynamic state or per-member descriptors are constructed here.
    explicit CompiledProgram(std::shared_ptr<const pc::Plan> p):plan_(std::move(p)) {
        require(plan_&&plan_->source&&plan_->typed_and_cursor_DAG_checked,"checked immutable segment plan");
        require((plan_->width==4||plan_->width==16)&&source().warp_groups.size()==4&&source().pipeline_count==6,"Prefill component domain");
        const auto n=plan_->segments.size();require(n>0,"nonempty segment partition");
        outgoing_.resize(n);indegree_.resize(n);recipes_.resize(n);internal_.resize(n);compute_subops_.resize(n);
        std::uint64_t typed=0,logical=0,groups=0,members=0,elements=0,fma=0;
        for(unsigned s=0;s<n;++s) {
            const auto& segment=plan_->segments[s];require(!segment.groups.empty(),"nonempty segment");
            groups=add(groups,segment.groups.size());members=add(members,segment.logical_members);
            elements=add(elements,segment.compute_elements);fma=add(fma,segment.tensor_fma);
            if(segment.groups.size()>1)recipes_[s]=recurrence::prepare_recipe(segment,source());
            for(auto original:segment.groups) {
                require(original<source().groups.size()&&plan_->group_to_segment.at(original)==s,"original group mapping");
                const auto& g=source().groups[original];require(g.warp==segment.warp,"segment warp identity");
                if(is_compute(g.kind))compute_subops_[s]=add(compute_subops_[s],g.kind==GroupKind::ComputePacket?g.members.size():g.subops);
            }
            for(auto index:segment.internal_edge_indices) {
                const auto& e=source().edges.at(index);const auto type=unsigned(e.type);
                require(type<2&&plan_->group_to_segment.at(e.source)==s&&plan_->group_to_segment.at(e.target)==s,"internal edge binding");
                internal_[s].typed[type]=add(internal_[s].typed[type],1);
                internal_[s].logical[type]=add(internal_[s].logical[type],e.logical_multiplicity);
                typed=add(typed,1);logical=add(logical,e.logical_multiplicity);
            }
        }
        for(unsigned i=0;i<plan_->gates.size();++i) {
            const auto& e=plan_->gates[i];require(e.source<n&&e.target<n&&e.source!=e.target&&unsigned(e.type)<2,"segment gate domain");
            require(!e.original_edge_indices.empty()&&e.logical_multiplicity>0,"nonempty aggregate gate");
            outgoing_[e.source].push_back(i);require(indegree_[e.target]!=std::numeric_limits<std::uint32_t>::max(),"gate indegree overflow");++indegree_[e.target];
            typed=add(typed,e.original_edge_indices.size());logical=add(logical,e.logical_multiplicity);
        }
        require(groups==source().groups.size()&&members==source().logical_members&&elements==plan_->stats.compute_elements&&fma==plan_->stats.tensor_fma,"compiled original work closure");
        require(typed==source().edges.size()&&logical==source().logical_edges,"compiled typed contribution closure");
    }
    const pc::Plan& plan()const{return *plan_;}
    const Program& source()const{return *plan_->source;}
    const Group& group(unsigned segment)const{return source().groups[plan_->segments[segment].groups.front()];}
    unsigned original_group(unsigned segment)const{return plan_->segments[segment].groups.front();}
    const std::vector<unsigned>& outgoing(unsigned segment)const{return outgoing_[segment];}
    unsigned indegree(unsigned segment)const{return indegree_[segment];}
    const InternalContributions& internal(unsigned segment)const{return internal_[segment];}
    const std::optional<recurrence::PreparedRecipe>& recipe(unsigned segment)const{return recipes_[segment];}
    std::uint64_t compute_subops(unsigned segment)const{return compute_subops_[segment];}
};
inline std::shared_ptr<const CompiledProgram> compile_program(std::shared_ptr<const pc::Plan> plan){return std::make_shared<const CompiledProgram>(std::move(plan));}
struct ActualStats {
    std::uint64_t segments_completed=0,merged_segments_issued=0,merged_segments_completed=0,actual_gate_updates=0;
    std::uint64_t internal_typed_group_contributions=0,internal_logical_edge_contributions=0;
    std::uint64_t segment_state_slots_current=0,segment_state_slots_peak=0;
};
} // namespace prefill_compute_runtime
