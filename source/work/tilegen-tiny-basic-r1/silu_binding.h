#pragma once
#include "../tilegen-tiny-full-r1/source.h"
#include "../tilegen-full-r1/canonical-silu-driver-r1/model_plan.h"

namespace tiny_full {
// Model and mapper outlive this provider. The already-validated original
// register program stays shared; only one SourceNode array is built per call.
class SiluBinding final : public KernelBinding {
    const canonical_silu::Model& model_;
    J call_;
    const SourceBundle& source_;
    const coupling::ServiceMapper& mapper_;
    U count_;
    std::vector<SourceNode> nodes_;
    J evidence_;

    const native_register::Node& original(unsigned member) const {
        p::need(member<source_.registers.order.size(),"tiny SiLU source member");
        const auto& n=source_.registers.nodes.at(source_.registers.order.at(member));
        p::need(n.output==int(member),"tiny SiLU dense source topology");return n;
    }
public:
    SiluBinding(const canonical_silu::Model& model,const J& call,U count,
                const coupling::ServiceMapper& mapper)
        :model_(model),call_(call),source_(model.source()),mapper_(mapper),count_(count) {
        bool found=false;
        for(const auto* candidate:model_.calls)
            if(candidate->at("source_launch_key")==call_.at("source_launch_key")) {
                p::need(!found && *candidate==call_,"tiny SiLU selected canonical call identity");found=true;
            }
        p::need(found && count_>0 && count_<=p::natural(call_.at("grid")[0]),
                "tiny SiLU selected CTA count");
        p::need(source_.program.warps==32 && source_.resident==1 &&
            source_.registers.nodes.size()==11800 && source_.registers.shared_nodes==0 &&
            source_.registers.barrier_nodes==0,"tiny SiLU original closed resources and topology");
        const U ncount=source_.registers.order.size();
        p::need(ncount==source_.registers.nodes.size() &&
            p::multiply(ncount,count_)<=INT32_MAX,"tiny SiLU original dense ID range");
        nodes_.reserve(ncount);
        U completion=0,issue=0,compute=0,read=0,write=0,sread=0,swrite=0,ranges=0,zero=0;
        J kinds=J::object();
        for(U member=0;member<ncount;++member) {
            const auto& n=original(unsigned(member));SourceNode s;
            s.id=unsigned(member);s.warp=unsigned(n.warp);s.ordinal=unsigned(n.local);
            s.source_ordinal=U(n.local);s.pc=n.pc;s.mask=n.mask;s.write=n.op=='W';
            for(int d:n.completion)s.completion_dependencies.push_back(unsigned(d));
            for(int d:n.issue)s.issue_dependencies.push_back(unsigned(d));
            completion+=n.completion.size();issue+=n.issue.size();
            if(n.kind=="global") {
                const auto& mem=source_.program.body(0).records.at(n.memory);
                p::need(mem.op==n.op,"tiny SiLU source global direction");
                if(mem.lanes.empty()) {
                    s.kind=Kind::Compute;s.pipeline="SIMD";s.compute_elements=U(n.elements);++zero;
                } else {
                    s.kind=Kind::Global;s.pipeline=s.write?"ST":"LD";
                    (s.write?write:read)=p::add(s.write?write:read,32*U(mem.width));++ranges;
                }
            } else if(n.kind=="shared") {
                s.kind=Kind::Shared;s.pipeline=s.write?"ST":"LD";
                (s.write?swrite:sread)=p::add(s.write?swrite:sread,p::multiply(n.lanes.size(),U(n.width)));
                ranges+=n.lanes.size();
            } else if(n.kind=="barrier") {
                s.kind=Kind::Barrier;s.pipeline="BARRIER";
            } else {
                p::need(n.kind=="compute" || n.kind=="control","tiny SiLU source kind");
                s.kind=n.kind=="compute"?Kind::Compute:Kind::Control;
                s.pipeline=n.pipeline;s.compute_elements=U(n.elements);
            }
            compute=p::add(compute,s.compute_elements);
            const std::string kind=s.kind==Kind::Global?"global":s.kind==Kind::Shared?"shared":
                s.kind==Kind::Barrier?"barrier":s.kind==Kind::Control?"control":"compute";
            if(!kinds.contains(kind))kinds[kind]=0;
            kinds[kind]=kinds[kind].get<U>()+1;nodes_.push_back(std::move(s));
        }
        p::need(read==source_.program.body(0).read && write==source_.program.body(0).write,
                "tiny SiLU independent source byte census");
        J totals={{"nodes",p::multiply(ncount,count_)},
            {"completion_edges",p::multiply(completion,count_)},{"issue_edges",p::multiply(issue,count_)},
            {"global_read_bytes",p::multiply(read,count_)},{"global_write_bytes",p::multiply(write,count_)},
            {"shared_read_bytes",p::multiply(sread,count_)},{"shared_write_bytes",p::multiply(swrite,count_)},
            {"scalar_compute_elements",p::multiply(compute,count_)},{"declared_tensor_FMA",0},
            {"async_copies",0},{"zero_source_async_copies",0},
            {"zero_lane_global_converted_to_SIMD",p::multiply(zero,count_)}};
        evidence_={{"schema","TINY_SILU_SOURCE_EVIDENCE_V1"},{"family","SiLU"},
            {"source_launch_key",call_.at("source_launch_key")},{"canonical_call",call_},
            {"selected_CTAs",count_},{"warps_per_CTA",source_.program.warps},
            {"native_resident_limit",source_.resident},{"nodes_per_CTA",ncount},
            {"completion_edges_per_CTA",completion},{"issue_edges_per_CTA",issue},
            {"kinds_per_CTA",kinds},{"scalar_compute_elements_per_CTA",compute},
            {"declared_tensor_FMA_per_CTA",0},{"global_read_bytes_per_CTA",read},
            {"global_write_bytes_per_CTA",write},{"shared_read_bytes_per_CTA",sread},
            {"shared_write_bytes_per_CTA",swrite},{"ranges_per_CTA",ranges},
            {"async_copies_per_CTA",0},{"zero_source_async_copies_per_CTA",0},
            {"zero_lane_global_converted_to_SIMD_per_CTA",zero},{"selected_totals",totals},
            {"register_source_pin",source_.input.at("register_file")},
            {"memory_source_pin",source_.input.at("program_file")},
            {"original_model_validation_preserved",true},{"per_CTA_DAGNode_allocation",false},
            {"native_hardware_timing_qualified",false},{"implicit_register_dependencies_complete",false},
            {"full_trace_saved",false},{"addresses","original canonical_silu::Model::address plus original contiguous-lane verification"}};
    }
    SiluBinding(const SiluBinding&)=delete;
    SiluBinding& operator=(const SiluBinding&)=delete;
    SiluBinding(SiluBinding&&)=delete;
    SiluBinding& operator=(SiluBinding&&)=delete;
    U ctas() const override {return count_;}
    unsigned warps(U cta) const override {p::need(cta<count_,"tiny SiLU warp CTA");return unsigned(source_.program.warps);}
    unsigned resident_limit() const override {return unsigned(source_.resident);}
    U first_node(U cta) const override {
        p::need(cta<count_,"tiny SiLU span CTA");return p::multiply(cta,nodes_.size());
    }
    std::span<const SourceNode> nodes(U cta) const override {
        p::need(cta<count_,"tiny SiLU nodes CTA");return nodes_;
    }
    U template_class(U cta) const override {p::need(cta<count_,"tiny SiLU class CTA");return 0;}
    MemoryDescriptor memory(U cta,unsigned member) const override {
        p::need(cta<count_ && member<nodes_.size(),"tiny SiLU memory bounds");
        const auto& n=original(member);const auto kind=nodes_[member].kind;
        p::need(kind==Kind::Global || kind==Kind::Shared,"tiny SiLU nonmemory member");
        MemoryDescriptor out;out.write=n.op=='W';
        out.path=kind==Kind::Global?Path::DirectGlobal:Path::Shared;
        g::ExplicitMemorySubop sub;
        if(kind==Kind::Global) {
            const auto& mem=source_.program.body(0).records.at(n.memory);
            const auto& formula=model_.records.at(n.memory);sub.requested_bytes=32*U(mem.width);
            const U va=model_.address(call_,cta,formula,0);
            for(int lane=0;lane<32;++lane)
                p::need(model_.address(call_,cta,formula,lane)==va+U(lane*mem.width),
                        "exact modeled contiguous lane order");
            // These ordinals are intentionally [-1], not all32 lane IDs.
            sub.ranges.push_back({-1,va,sub.requested_bytes});sub.source_member_ordinals.push_back(-1);
            const U last=(va+sub.requested_bytes-1)/128*128;
            for(U line=va/128*128;;line+=128) {
                const g::CacheLineKey key{1,line};(void)mapper_.map(key);
                out.lines.push_back(key);if(line==last)break;
            }
            out.global_bytes=sub.requested_bytes;out.global_subops.push_back(std::move(sub));
        } else {
            sub.requested_bytes=p::multiply(n.lanes.size(),U(n.width));
            for(auto [lane,address]:n.lanes) {
                sub.ranges.push_back({lane,address,U(n.width)});sub.source_member_ordinals.push_back(lane);
            }
            out.shared_bytes=sub.requested_bytes;out.shared_service=g::describe_explicit_sram_ranges(sub);
            out.shared_subops.push_back(std::move(sub));
        }
        return out;
    }
    J evidence() const override {return evidence_;}
};
} // namespace tiny_full
