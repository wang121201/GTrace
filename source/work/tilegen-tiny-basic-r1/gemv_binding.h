#pragma once
#include "../tilegen-tiny-full-r1/source.h"
#include "../tilegen-packet-integration-r1/binding.h"

namespace tiny_full {

// Adapter only: the canonical Model and full-workflow ServiceMapper must
// outlive this binding. SourceBundle and its already-expanded BAR/topological
// program stay immutable. No per-CTA DAGNode or alternate runtime is created.
class GemvBinding final : public KernelBinding {
    J call_;
    packet_binding::CallBinding original_;
    U count_;
    std::vector<SourceNode> nodes_;
    J evidence_;
public:
    GemvBinding(const canonical_gemv::Model& model,const J& call,U count,
                const coupling::ServiceMapper& mapper)
        :call_(call),original_(model,call_,mapper),count_(count) {
        bool found=false;
        for(const auto* candidate:model.calls)
            if(candidate->at("source_launch_key")==call_.at("source_launch_key")) {
                p::need(!found && *candidate==call_,"tiny GEMV canonical selected call identity");
                found=true;
            }
        p::need(found && count_>0 && count_<=original_.ctas,
                "tiny GEMV count within selected canonical call");
        const auto& source=original_.source;
        p::need(source.resident>0 && source.program.warps>0 &&
                !source.registers.order.empty(),"tiny GEMV native source resources");
        const U count_nodes=source.registers.order.size();
        p::need(p::multiply(count_nodes,count_)<=INT32_MAX,"tiny GEMV original dense ID domain");
        nodes_.reserve(count_nodes);
        U completion=0,issue=0,scalar=0,zero=0,rd=0,wr=0,srd=0,swr=0,ranges=0;
        J kinds=J::object();
        for(U member=0;member<count_nodes;++member) {
            const auto& n=original_.node(member);
            SourceNode s;
            s.id=unsigned(member);s.warp=unsigned(n.warp);
            s.ordinal=unsigned(n.local);s.source_ordinal=U(n.local);
            s.pc=n.pc;s.mask=n.mask;s.write=n.op=='W';
            for(int d:n.completion)s.completion_dependencies.push_back(unsigned(d));
            for(int d:n.issue)s.issue_dependencies.push_back(unsigned(d));
            completion+=n.completion.size();issue+=n.issue.size();
            if(n.kind=="global") {
                const auto& r=source.program.body(0).records.at(n.memory);
                p::need(r.op==n.op,"tiny GEMV original global direction");
                if(r.lanes.empty()) {
                    // Exactly ModeledBuilder's zero-lane global conversion.
                    s.kind=Kind::Compute;s.pipeline="SIMD";s.compute_elements=U(n.elements);++zero;
                } else {
                    s.kind=Kind::Global;s.pipeline=s.write?"ST":"LD";
                    (s.write?wr:rd)=p::add(s.write?wr:rd,p::multiply(r.lanes.size(),U(r.width)));
                    ranges+=source.range_plan.records.at(n.memory).size();
                }
            } else if(n.kind=="shared") {
                s.kind=Kind::Shared;s.pipeline=s.write?"ST":"LD";
                (s.write?swr:srd)=p::add(s.write?swr:srd,p::multiply(n.lanes.size(),U(n.width)));
                ranges+=n.lanes.size();
            } else if(n.kind=="barrier") {
                s.kind=Kind::Barrier;s.pipeline="BARRIER";
            } else {
                p::need(n.kind=="compute" || n.kind=="control","tiny GEMV closed source kind");
                s.kind=n.kind=="compute"?Kind::Compute:Kind::Control;
                s.pipeline=n.pipeline;s.compute_elements=U(n.elements);
            }
            scalar=p::add(scalar,s.compute_elements);
            const auto tag=s.kind==Kind::Global?"global":s.kind==Kind::Shared?"shared":
                s.kind==Kind::Barrier?"barrier":s.kind==Kind::Control?"control":"compute";
            if(!kinds.contains(tag))kinds[tag]=0;
            kinds[tag]=kinds[tag].get<U>()+1;
            nodes_.push_back(std::move(s));
        }
        p::need(rd==source.program.body(0).read && wr==source.program.body(0).write,
                "tiny GEMV independent original byte census");
        evidence_={{"schema","TINY_GEMV_SOURCE_EVIDENCE_V1"},{"family","GEMV"},
            {"source_launch_key",call_.at("source_launch_key")},{"template_key",call_.at("template_key")},
            {"selected_CTAs",count_},{"warps_per_CTA",source.program.warps},
            {"native_resident_limit",source.resident},{"nodes_per_CTA",count_nodes},
            {"completion_edges_per_CTA",completion},{"issue_edges_per_CTA",issue},
            {"kinds_per_CTA",kinds},{"scalar_compute_elements_per_CTA",scalar},
            {"declared_tensor_FMA_per_CTA",0},{"global_read_bytes_per_CTA",rd},
            {"global_write_bytes_per_CTA",wr},{"shared_read_bytes_per_CTA",srd},
            {"shared_write_bytes_per_CTA",swr},{"ranges_per_CTA",ranges},
            {"async_copies_per_CTA",0},{"zero_source_async_copies_per_CTA",0},
            {"zero_lane_global_converted_to_SIMD_per_CTA",zero},
            {"register_source_pin",source.input.at("register_file")},
            {"memory_source_pin",source.input.at("program_file")},
            {"canonical_call",call_},{"original_model_validation_preserved",true},
            {"native_hardware_timing_qualified",false},{"implicit_register_dependencies_complete",false},
            {"per_CTA_DAGNode_allocation",false},{"full_trace_saved",false},
            {"addresses","existing CallBinding and PreparedAddress; original canonical call pointers"}};
    }
    GemvBinding(const GemvBinding&)=delete;
    GemvBinding& operator=(const GemvBinding&)=delete;
    GemvBinding(GemvBinding&&)=delete;
    GemvBinding& operator=(GemvBinding&&)=delete;
    U ctas() const override {return count_;}
    unsigned warps(U cta) const override {
        p::need(cta<count_,"tiny GEMV warp CTA");return unsigned(original_.source.program.warps);
    }
    unsigned resident_limit() const override {return unsigned(original_.source.resident);}
    U first_node(U cta) const override {
        p::need(cta<count_,"tiny GEMV span CTA");return p::multiply(cta,nodes_.size());
    }
    std::span<const SourceNode> nodes(U cta) const override {
        p::need(cta<count_,"tiny GEMV nodes CTA");return nodes_;
    }
    U template_class(U cta) const override {p::need(cta<count_,"tiny GEMV class CTA");return 0;}
    MemoryDescriptor memory(U cta,unsigned member) const override {
        p::need(cta<count_ && member<nodes_.size(),"tiny GEMV memory bounds");
        const auto kind=nodes_[member].kind;
        p::need(kind==Kind::Global || kind==Kind::Shared,"tiny GEMV nonmemory member");
        auto old=original_.memory(cta,member);
        p::need(!old.zero_lane_compute && old.global==(kind==Kind::Global),
                "tiny GEMV original descriptor kind");
        MemoryDescriptor out;out.write=old.write;
        out.path=old.global?Path::DirectGlobal:Path::Shared;
        if(old.global) {
            out.global_bytes=old.logical_bytes;out.global_subops=std::move(old.subops);
            out.lines=std::move(old.lines);
        } else {
            out.shared_bytes=old.logical_bytes;out.shared_subops=std::move(old.subops);
            out.shared_service=old.shared_service;
        }
        return out;
    }
    J evidence() const override {return evidence_;}
};
} // namespace tiny_full
