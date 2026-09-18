#pragma once
#include "prepared_address.h"

namespace canonical_gemv {
// One immutable instruction shape per call, shared by functional direct and
// fine cosimulation. Only addresses depend on the CTA. This is pre-cache data:
// no cache decisions, issue times, responses or scheduler state are retained.
class PreparedMemory {
    struct Instruction {
        g::ExplicitMemorySubop shape;
        std::vector<const p::Lane*> lanes;
    };
    const Model& model_;
    const J& call_;
    PreparedAddress addresses_;
    const coupling::ServiceMapper& mapper_;
    std::vector<Instruction> instructions_;
public:
    PreparedMemory(const Model& model,const J& call,const coupling::ServiceMapper& mapper)
        :model_(model),call_(call),addresses_(model,call),mapper_(mapper) {
        const auto& source=model.source(call);
        const auto& records=source.program.body(0).records;
        instructions_.reserve(records.size());
        for(std::size_t i=0;i<records.size();++i) {
            const auto& record=records[i];const auto& plan=source.range_plan.records.at(i);
            Instruction prepared;
            prepared.shape.requested_bytes=p::multiply(record.lanes.size(),U(record.width));
            prepared.shape.source_member_ordinals.reserve(record.lanes.size());
            for(const auto& lane:record.lanes)prepared.shape.source_member_ordinals.push_back(lane.lane);
            prepared.shape.ranges.reserve(plan.size());prepared.lanes.reserve(plan.size());
            for(const auto& group:plan) {
                const auto& lane=record.lanes.at(group.first);
                prepared.lanes.push_back(&lane);
                prepared.shape.ranges.push_back({group.count==1?lane.lane:-1,0,group.bytes});
            }
            instructions_.push_back(std::move(prepared));
        }
    }
    g::ExplicitMemorySubop materialize(std::size_t record,U cta) const {
        const auto& instruction=instructions_.at(record);
        // Copy each vector once instead of rebuilding its lane/range shape
        // through repeated capacity growth for every resident CTA.
        auto sub=instruction.shape;
        for(std::size_t i=0;i<sub.ranges.size();++i) {
            auto& range=sub.ranges[i];const auto& lane=*instruction.lanes[i];
            const U va=host_address::json_reference?model_.address(call_,lane,range.byte_count,cta)
                :addresses_.address(lane,range.byte_count,cta);
            range.offset_bytes=va;
            p::need(range.byte_count>0&&va<=UINT64_MAX-(range.byte_count-1),"prepared GEMV range overflow");
            const U last=(va+range.byte_count-1)/128*128;
            for(U line=va/128*128;;line+=128) {
                (void)mapper_.map(g::CacheLineKey{1,line});if(line==last)break;
            }
        }
        return sub;
    }
};
} // namespace canonical_gemv
