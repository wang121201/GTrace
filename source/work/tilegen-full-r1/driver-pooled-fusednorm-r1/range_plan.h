#pragma once
#include "native_memory_program.h"
#include "dag_node.h"
// Adjacent, disjoint, input-order spans only. Never sort/deduplicate; shared
// operands never enter this helper. The immutable source lanes are retained.
namespace native_range_plan {
using U=native_program::U;
struct Group { std::size_t first,count;U bytes; };
inline std::vector<Group> compile(const native_program::Record& r) {
    std::vector<Group> out;out.reserve(r.lanes.size());
    for(std::size_t i=0;i<r.lanes.size();++i){const auto& lane=r.lanes[i];
        if(!out.empty()){auto& g=out.back();const auto& first=r.lanes[g.first];
            // Requiring the same symbolic rule proves equal translation for
            // every CTA. Reference identity is kept even though mapping uses
            // object/rule only. Strict adjacency preserves byte multiplicity.
            if(lane.object==first.object&&lane.rule==first.rule&&lane.reference==first.reference&&
               first.offset<=UINT64_MAX-g.bytes&&lane.offset==first.offset+g.bytes){
                g.bytes=native_program::add(g.bytes,U(r.width));++g.count;continue;}}
        out.push_back({i,1,U(r.width)});
    }
    U sum=0;for(const auto& g:out)sum=native_program::add(sum,g.bytes);
    native_program::need(sum==native_program::multiply(r.lanes.size(),r.width),"range plan byte conservation");return out;
}
inline GTSim::ExplicitMemorySubop materialize(const native_program::Program& p,
        const native_program::Record& r,const std::vector<Group>& groups,U cta,bool merged) {
    GTSim::ExplicitMemorySubop sub;sub.requested_bytes=native_program::multiply(r.lanes.size(),r.width);
    sub.source_member_ordinals.reserve(r.lanes.size());
    for(const auto& lane:r.lanes)sub.source_member_ordinals.push_back(lane.lane);
    sub.ranges.reserve(merged?groups.size():r.lanes.size());
    if(merged){for(const auto& g:groups){const auto& first=r.lanes[g.first];
        // A multi-lane span is not assigned one fictitious lane; the complete
        // lane provenance remains in source_member_ordinals and the source.
        sub.ranges.push_back({g.count==1?first.lane:-1,p.address(first,g.bytes,cta),g.bytes});}}
    else for(const auto& lane:r.lanes)sub.ranges.push_back({lane.lane,p.address(lane,r.width,cta),U(r.width)});
    return sub;
}
struct Plan {std::vector<std::vector<Group>> records;U source_ranges=0,merged_ranges=0;
    explicit Plan(const native_program::Program& p){native_program::need(p.bodies.size()==1,"one native source body required");
        records.reserve(p.bodies[0].records.size());for(const auto& r:p.bodies[0].records){records.push_back(compile(r));source_ranges+=r.lanes.size();merged_ranges+=records.back().size();}}
};
}
