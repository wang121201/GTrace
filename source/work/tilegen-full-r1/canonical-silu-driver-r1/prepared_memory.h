#pragma once
#include "model_plan.h"

namespace canonical_silu {
class TypedBinding;
// Shared pre-cache source materializer for the fine DAG Builder and native
// binding. It changes host preparation only: the original Model remains the
// source/qualification authority; ordering and all dynamic GPU state stay with
// the caller. One compiled range per original memory instruction is immutable.
class PreparedMemory {
    struct Range {
        U first, stride, bytes, object_begin, object_end;
        char op;
    };
    struct ObjectView { U begin,end; };
    struct AddressView { U ctas;ObjectView input,out; };
    U ctas_;
    std::vector<Range> ranges_;
    friend class TypedBinding;
    static AddressView legacy_view(const J& call) {
        const U count=p::natural(call.at("grid")[0]);
        p::need(count==1||count==32,"SiLU prepared original fixed grid domain");
        auto view=[&](const char* role) {
            const auto& object=call.at("objects").at(role);
            const U begin=p::natural(object.at("pointer"));
            p::need(begin==p::natural(call.at("arguments").at(role)),"SiLU prepared argument/object pointer");
            return ObjectView{begin,p::natural(object.at("end_exclusive"))};
        };
        return {count,view("input"),view("out")};
    }
    // Both admitted legacy calls and explicit typed candidates compile through
    // this one affine formula and one immutable range materializer.
    static U first_address(const Formula& r,const ObjectView& object,U lane) {
        p::need(r.warp<32&&lane<32,"SiLU prepared warp/lane domain");
        const U tid=p::add(p::multiply(r.warp,32),lane);
        U element=0;
        if(r.width==16) {
            p::need(r.occurrence<(r.warp<24?2:1),"SiLU prepared vector loop count");
            element=p::multiply(p::add(tid,p::multiply(1024,r.occurrence)),8);
        } else {
            p::need(r.width==2&&r.occurrence<6,"SiLU prepared scalar loop count");
            element=p::add(8192,p::add(tid,p::multiply(1024,r.occurrence)));
        }
        p::need(p::add(element,r.width/2)<=14336,"SiLU prepared element extent");
        return p::add(object.begin,p::add(r.part,p::multiply(element,2)));
    }
    PreparedMemory(const p::Program& program,const std::vector<Formula>& formulas,AddressView view)
        :ctas_(view.ctas) {
        p::need(ctas_==1||ctas_==32,"SiLU prepared fixed compiled grid domain");
        const auto& source=program.bodies.at(0).records;
        p::need(source.size()==formulas.size()&&source.size()==744,
                "SiLU prepared full original record bijection");
        ranges_.reserve(source.size());
        std::array<U,32> ordinals{};
        for(U i=0;i<source.size();++i) {
            const auto& mem=source.at(i);const auto& formula=formulas.at(i);
            p::need(mem.lanes.size()==32&&mem.mask==UINT32_MAX&&
                U(mem.width)==formula.width&&(formula.width==2||formula.width==16)&&
                mem.op==formula.op&&(formula.role=="input"||formula.role=="out"),
                "SiLU prepared original lane/width/role contract");
            p::need(mem.warp>=0&&mem.warp<32&&formula.warp==U(mem.warp)&&formula.ordinal==ordinals[formula.warp]++&&
                mem.pc==formula.pc&&program.opcodes.at(mem.opcode)==formula.opcode,
                "SiLU prepared exact source instruction order");
            for(U lane=0;lane<32;++lane)p::need(mem.lanes[lane].lane==int(lane),"SiLU prepared lane order");
            const auto& object=formula.role=="input"?view.input:view.out;
            const U begin=object.begin,end=object.end;
            const U stride=formula.role=="input"?57344:28672;
            // The unchanged formula is affine in CTA and in lane for this
            // closed source. Keep original endpoint guards, then prove that
            // the entire32-lane span fits every selected full-grid CTA.
            const U first=first_address(formula,object,0);
            const U last=first_address(formula,object,31);
            const U bytes=p::multiply(32,formula.width);
            p::need(last==p::add(first,p::multiply(31,formula.width)),
                    "SiLU prepared exact contiguous lane endpoint");
            const U grid_end=p::add(p::add(first,p::multiply(ctas_-1,stride)),bytes);
            p::need(first>=begin&&grid_end<=end,"SiLU prepared whole-grid target bounds");
            ranges_.push_back({first,stride,bytes,begin,end,mem.op});
        }
    }
public:
    PreparedMemory(const Model& model,const J& call)
        :PreparedMemory(model.source().program,model.records,legacy_view(call)) {}
    U ctas() const {return ctas_;}
    U records() const {return ranges_.size();}
    char direction(U record) const {
        p::need(record<ranges_.size(),"SiLU prepared direction record domain");return ranges_[record].op;
    }
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
