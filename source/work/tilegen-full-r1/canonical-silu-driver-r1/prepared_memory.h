#pragma once
#include "model_plan.h"

namespace canonical_silu {
// Shared pre-cache source materializer for the fine DAG Builder and native
// binding. It changes host preparation only: the original Model remains the
// source/qualification authority; ordering and all dynamic GPU state stay with
// the caller. One compiled range per original memory instruction is immutable.
class PreparedMemory {
    struct Range {
        U first, stride, bytes, object_begin, object_end;
    };
    U ctas_;
    std::vector<Range> ranges_;
public:
    PreparedMemory(const Model& model,const J& call)
        :ctas_(p::natural(call.at("grid")[0])) {
        p::need(ctas_==1||ctas_==32,"SiLU prepared original fixed grid domain");
        const auto& source=model.source().program.bodies.at(0).records;
        p::need(source.size()==model.records.size()&&source.size()==744,
                "SiLU prepared full original record bijection");
        ranges_.reserve(source.size());
        for(U i=0;i<source.size();++i) {
            const auto& mem=source.at(i);const auto& formula=model.records.at(i);
            p::need(mem.lanes.size()==32&&mem.mask==UINT32_MAX&&
                U(mem.width)==formula.width&&(formula.width==2||formula.width==16)&&
                mem.op==formula.op&&(formula.role=="input"||formula.role=="out"),
                "SiLU prepared original lane/width/role contract");
            const auto& object=call.at("objects").at(formula.role);
            const U begin=p::natural(object.at("pointer")),end=p::natural(object.at("end_exclusive"));
            const U stride=formula.role=="input"?57344:28672;
            // The unchanged formula is affine in CTA and in lane for this
            // closed source. Keep original endpoint guards, then prove that
            // the entire32-lane span fits every selected full-grid CTA.
            const U first=model.address(call,0,formula,0);
            const U last=model.address(call,0,formula,31);
            const U bytes=p::multiply(32,formula.width);
            p::need(last==p::add(first,p::multiply(31,formula.width)),
                    "SiLU prepared exact contiguous lane endpoint");
            const U grid_end=p::add(p::add(first,p::multiply(ctas_-1,stride)),bytes);
            p::need(first>=begin&&grid_end<=end,"SiLU prepared whole-grid target bounds");
            ranges_.push_back({first,stride,bytes,begin,end});
        }
    }
    U ctas() const {return ctas_;}
    U records() const {return ranges_.size();}
    g::ExplicitMemorySubop materialize(U cta,U record) const {
        p::need(cta<ctas_&&record<ranges_.size(),"SiLU prepared CTA/record domain");
        const auto& range=ranges_[record];
        const U address=p::add(range.first,p::multiply(cta,range.stride));
        p::need(address>=range.object_begin&&p::add(address,range.bytes)<=range.object_end,
                "SiLU prepared materialized target span");
        g::ExplicitMemorySubop result;result.requested_bytes=range.bytes;
        result.source_member_ordinals.push_back(-1);
        result.ranges.push_back({-1,address,range.bytes});
        return result;
    }
};
} // namespace canonical_silu
