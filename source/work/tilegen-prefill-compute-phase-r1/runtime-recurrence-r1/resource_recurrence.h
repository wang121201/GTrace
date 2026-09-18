#pragma once
// Caller first includes the qualified packet_runtime Program definition.
#include "../segment_plan.h"
#include <array>

namespace prefill_resource_recurrence {
namespace pc=prefill_compute_phase;
namespace rt=packet_runtime;
using Cycle=rt::Cycle;
using U=std::uint64_t;
constexpr unsigned max_groups=16;
constexpr unsigned max_internal_edges=max_groups*(max_groups-1); // two typed edges per forward pair
struct ResourceClocks {Cycle sp_next=0;std::array<Cycle,6> pipeline_free{};};
struct RecipeTiming {
    ResourceClocks after;
    Cycle first_dispatch=0,cursor_ready=0,issue_tail=0,completion_tail=0;
    U original_groups=0,logical_members=0,compute_elements=0,tensor_fma=0;
};
enum class Family { Gate, Down, Other };
struct Domain {
    Family family=Family::Other;
    unsigned width=1,warps_per_CTA=0,resident_limit=0,SPs_per_SM=0,pipelines_per_SP=0;
    std::array<unsigned,4> warp_to_global_SP{};
};
enum class DomainDecision { LegacyWidth1, LegacyUnsupported, Supported };
inline DomainDecision domain_decision(const Domain& d) {
    if(d.width==1)return DomainDecision::LegacyWidth1;
    if((d.family!=Family::Gate&&d.family!=Family::Down)||(d.width!=4&&d.width!=16)||
       d.warps_per_CTA!=4||d.resident_limit!=1||d.SPs_per_SM!=4||d.pipelines_per_SP!=6)
        return DomainDecision::LegacyUnsupported;
    for(unsigned w=0;w<4;++w)if(d.warp_to_global_SP[w]%4!=w||
        d.warp_to_global_SP[w]/4!=d.warp_to_global_SP[0]/4)return DomainDecision::LegacyUnsupported;
    return DomainDecision::Supported;
}
class PreparedRecipe;
PreparedRecipe prepare_recipe(const pc::SegmentRecipe&,const rt::Program&);
RecipeTiming evaluate_recipe(const PreparedRecipe&,Cycle,const ResourceClocks&);
// Immutable prepared incoming lists: construct once per static SegmentRecipe,
// outside the per-CTA path. Only used prefixes are stored; no 240-edge padding.
class PreparedRecipe {
    struct Dependency {unsigned source=0;rt::EdgeType type=rt::EdgeType::Completion;};
    std::vector<pc::ResourceStep> steps_;
    std::vector<unsigned> incoming_begin_;
    std::vector<Dependency> incoming_;
    U members_=0,elements_=0,fma_=0;
    friend PreparedRecipe prepare_recipe(const pc::SegmentRecipe&,const rt::Program&);
    friend RecipeTiming evaluate_recipe(const PreparedRecipe&,Cycle,const ResourceClocks&);
public:
    unsigned groups()const{return unsigned(steps_.size());}
    unsigned internal_constraints()const{return unsigned(incoming_.size());}
};
inline PreparedRecipe prepare_recipe(const pc::SegmentRecipe& segment,const rt::Program& program) {
    // Precondition: segment comes from an immutable make_plan result with the
    // complete Program partition and typed+cursor DAG already validated.
    rt::require(segment.hard_boundary==pc::BoundaryReason::None&&!segment.groups.empty()&&segment.groups.size()<=max_groups,"eligible bounded recipe");
    rt::require(program.pipeline_count==6&&segment.resource_steps.size()==segment.groups.size()&&segment.internal_edge_indices.size()<=max_internal_edges,"recipe profile shape");
    PreparedRecipe out;out.steps_.reserve(segment.groups.size());
    auto position=[&](unsigned group){
        for(unsigned i=0;i<segment.groups.size();++i)if(segment.groups[i]==group)return i;
        throw std::invalid_argument("internal predecessor/target outside recipe");
    };
    for(unsigned i=0;i<segment.groups.size();++i) {
        const auto id=segment.groups[i];rt::require(id<program.groups.size()&&position(id)==i,"original group identity");
        const auto& group=program.groups[id];rt::require(group.warp==segment.warp&&group.pipeline<6,"original group resources");
        const auto original=pc::detail::step(id,group);const auto& supplied=segment.resource_steps[i];
        rt::require(original.group==supplied.group&&original.pipeline==supplied.pipeline&&original.packet==supplied.packet&&
            original.sp_next_issue_offset==supplied.sp_next_issue_offset&&original.pipeline_occupied_until_offset==supplied.pipeline_occupied_until_offset&&
            original.issue_release_offset==supplied.issue_release_offset&&original.completion_offset==supplied.completion_offset,"unchanged group resource profile");
        rt::require(original.sp_next_issue_offset>0&&original.pipeline_occupied_until_offset>0&&
            original.completion_offset>=original.pipeline_occupied_until_offset&&original.issue_release_offset<=original.sp_next_issue_offset,"finite valid resource profile");
        out.steps_.push_back(original);out.members_=rt::add(out.members_,group.members.size());
        out.elements_=rt::add(out.elements_,group.modeled_compute_elements);out.fma_=rt::add(out.fma_,group.modeled_tensor_fma);
    }
    rt::require(out.members_==segment.logical_members&&out.elements_==segment.compute_elements&&out.fma_==segment.tensor_fma,"prepared recipe work closure");
    struct Indexed {unsigned source,target;rt::EdgeType type;};std::vector<Indexed> indexed;indexed.reserve(segment.internal_edge_indices.size());
    std::vector<unsigned> edge_indices=segment.internal_edge_indices;std::sort(edge_indices.begin(),edge_indices.end());
    rt::require(std::adjacent_find(edge_indices.begin(),edge_indices.end())==edge_indices.end(),"duplicate internal constraint");
    out.incoming_begin_.assign(segment.groups.size()+1,0);
    for(auto index:segment.internal_edge_indices) {
        rt::require(index<program.edges.size(),"original internal edge index");const auto& e=program.edges[index];
        const auto a=position(e.source),b=position(e.target);
        rt::require(a<b&&(e.type==rt::EdgeType::Issue||e.type==rt::EdgeType::Completion)&&e.logical_multiplicity>0,"ordered internal typed predecessor");
        indexed.push_back({a,b,e.type});++out.incoming_begin_[b+1];
    }
    for(unsigned i=1;i<out.incoming_begin_.size();++i)out.incoming_begin_[i]+=out.incoming_begin_[i-1];
    out.incoming_.resize(indexed.size());auto cursor=out.incoming_begin_;
    for(const auto& edge:indexed)out.incoming_[cursor[edge.target]++]={edge.source,edge.type};
    return out;
}
inline RecipeTiming evaluate_recipe(const PreparedRecipe& recipe,Cycle entry,const ResourceClocks& input) {
    rt::require(recipe.groups()>0&&recipe.groups()<=max_groups,"nonempty prepared recipe");
    RecipeTiming result;result.after=input;result.original_groups=recipe.groups();
    result.logical_members=recipe.members_;result.compute_elements=recipe.elements_;result.tensor_fma=recipe.fma_;
    // Only assigned prefix values are read; no scratch arrays escape the call.
    std::array<Cycle,max_groups> issue,complete;
    for(unsigned i=0;i<recipe.groups();++i) {
        const auto& step=recipe.steps_[i];
        Cycle dispatch=std::max(entry,std::max(result.after.sp_next,result.after.pipeline_free[step.pipeline]));
        for(unsigned j=recipe.incoming_begin_[i];j<recipe.incoming_begin_[i+1];++j) {
            const auto& edge=recipe.incoming_[j];
            const auto source=edge.type==rt::EdgeType::Issue?issue[edge.source]:complete[edge.source];
            dispatch=std::max(dispatch,rt::add(source,1));
        }
        issue[i]=rt::add(dispatch,step.issue_release_offset);complete[i]=rt::add(dispatch,step.completion_offset);
        result.after.sp_next=rt::add(dispatch,step.sp_next_issue_offset);
        result.after.pipeline_free[step.pipeline]=rt::add(dispatch,step.pipeline_occupied_until_offset);
        if(i==0)result.first_dispatch=dispatch;
        result.issue_tail=std::max(result.issue_tail,issue[i]);result.completion_tail=std::max(result.completion_tail,complete[i]);
    }
    result.cursor_ready=result.after.sp_next;return result;
}
// Convenience qualification API. Future Runtime must cache PreparedRecipe once,
// then use the overload above; this overload prepares anew and is not a fast path.
inline RecipeTiming evaluate_recipe(const pc::SegmentRecipe& segment,const rt::Program& program,Cycle entry,const ResourceClocks& clocks) {
    return evaluate_recipe(prepare_recipe(segment,program),entry,clocks);
}
} // namespace prefill_resource_recurrence
