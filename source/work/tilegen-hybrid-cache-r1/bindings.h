#pragma once
// Include after the original full entry has defined canonical_full::Model and
// Prepared, compressed_frame::Cache and frame_bridge. Root's VFS common bridge
// suppresses the two duplicate extracted prefixes; provider headers stay intact.
#include "prefill_binding.h"
#include "../tilegen-tiny-basic-r1/gemv_binding.h"
#include "../tilegen-tiny-basic-r1/silu_binding.h"
#include "../tilegen-tiny-norm-r1/norm_binding.h"
#include "../tilegen-tiny-rotary-r1/rotary_binding.h"
#include <map>
#include <memory>
#include <set>
#include <string>

namespace hybrid_full {
using namespace native_sequence;

// Same evidence rule as the qualified nine-family tiny entry. No averaging of
// Rotary classes or future nonuniform source families is permitted here.
inline U source_total(const J& evidence,const std::string& field,U count) {
    if(evidence.contains("selected_totals"))
        return p::natural(evidence.at("selected_totals").at(field));
    const std::string family=evidence.at("family").get<std::string>();
    p::need(family=="GEMV"||family=="P28QKV"||family=="GEMMO"||
        family=="GEMMGate"||family=="GEMMDown",
        "hybrid nonuniform source requires selected totals");
    return p::multiply(p::natural(evidence.at(field+"_per_CTA")),count);
}

// Borrows the already-constructed full canonical Model and full service mapper.
// Each returned binding must be destroyed before Model/mapper/frame catalog.
// This factory retains at most four immutable typed Prefill templates, but no
// frame DOM/decoded bytes, per-CTA DAG, absolute request addresses or runtime state.
class Bindings {
    const canonical_full::Model& model_;
    compressed_frame::Cache& frames_;
    const coupling::ServiceMapper& mapper_;
    std::map<std::string,const canonical_full::Target*> selected_;
    U bound_calls_=0,prefill_frame_bindings_=0,max_call_ctas_=0;
    U max_template_nodes_=0,max_template_classes_=0;
    tiny_full::PrefillTemplateCache prefill_templates_;
    std::map<std::string,J> frame_identities_;

    void validate(const canonical_full::Prepared& prepared,
                  const tiny_full::KernelBinding& binding) {
        const U count=prepared.executed;
        p::need(count>0&&binding.ctas()==count,
            "hybrid binding uses exact original Prepared CTA selection");
        p::need(prepared.resident>0&&prepared.warps>0&&
            binding.resident_limit()==unsigned(prepared.resident),
            "hybrid binding preserves original resident limit");
        const J evidence=binding.evidence();
        p::need(evidence.at("family")==prepared.family&&
            evidence.at("source_launch_key")==prepared.call.at("source_launch_key")&&
            evidence.at("canonical_call")==prepared.call&&
            p::natural(evidence.at("selected_CTAs"))==count,
            "hybrid complete original call and evidence identity");
        p::need(evidence.at("original_model_validation_preserved")==true&&
            evidence.at("native_hardware_timing_qualified")==false&&
            evidence.at("implicit_register_dependencies_complete")==false&&
            evidence.at("per_CTA_DAGNode_allocation")==false&&
            evidence.at("full_trace_saved")==false,
            "hybrid provider qualification remains explicit");
        p::need(source_total(evidence,"nodes",count)==prepared.total_nodes&&
            source_total(evidence,"global_read_bytes",count)==prepared.expected_read&&
            source_total(evidence,"global_write_bytes",count)==prepared.expected_write,
            "hybrid provider matches original Prepared node and logical-byte census");
        // Check actual span/class metadata for every selected CTA without
        // building a CTA or materializing any memory address. Uniform providers
        // share one SourceNode array; Rotary keeps its actual class population.
        std::map<U,U> class_nodes;
        U next=0;
        for(U c=0;c<count;++c) {
            const auto nodes=binding.nodes(c);const U n=U(nodes.size());
            p::need(n>0&&binding.first_node(c)==next&&
                binding.warps(c)==unsigned(prepared.warps),
                "hybrid original contiguous CTA spans and warp shape");
            next=p::add(next,n);
            p::need(next<=U(INT32_MAX),"hybrid original int completion-token domain");
            const U id=binding.template_class(c);
            const auto [it,inserted]=class_nodes.emplace(id,n);
            p::need(inserted||it->second==n,"hybrid immutable template class size");
        }
        p::need(next==prepared.total_nodes,"hybrid all selected CTA span census");
        U template_nodes=0;
        for(const auto& item:class_nodes)template_nodes=p::add(template_nodes,item.second);
        max_call_ctas_=std::max(max_call_ctas_,count);
        max_template_nodes_=std::max(max_template_nodes_,template_nodes);
        max_template_classes_=std::max(max_template_classes_,U(class_nodes.size()));
    }
public:
    static bool supports(const std::string& family) {
        return family=="GEMV"||family=="SiLU"||family=="FusedNorm"||
            family=="PlainNorm"||family=="Rotary"||family=="P28QKV"||
            family=="GEMMO"||family=="GEMMGate"||family=="GEMMDown";
    }
    Bindings(const canonical_full::Model& model,compressed_frame::Cache& frames,
             const coupling::ServiceMapper& mapper)
        :model_(model),frames_(frames),mapper_(mapper) {
        p::need(bool(model_.gemv)&&bool(model_.legacy),
            "hybrid requires original complete canonical source model");
        for(const auto& frame:frames_.decoded_manifest())
            p::need(frame_identities_.emplace(frame.at("key").get<std::string>(),frame).second,
                "unique immutable decoded frame identity");
        for(const auto& target:model_.calls) {
            const auto key=target.call->at("source_launch_key").get<std::string>();
            p::need(selected_.emplace(key,&target).second,
                "hybrid selected canonical calls remain unique");
        }
    }
    Bindings(const Bindings&)=delete;
    Bindings& operator=(const Bindings&)=delete;

    std::unique_ptr<tiny_full::KernelBinding>
    bind(const canonical_full::Prepared& prepared) {
        const auto& family=prepared.family;const auto& call=prepared.call;
        const auto key=call.at("source_launch_key").get<std::string>();
        const auto it=selected_.find(key);
        p::need(it!=selected_.end()&&it->second->family==family&&
            it->second->call==&call&&prepared.t.call==&call,
            "hybrid Prepared borrows this selected canonical Model call");
        p::need(supports(family),"unsupported tiny family must use original fine fallback");
        std::unique_ptr<tiny_full::KernelBinding> result;
        const U count=prepared.executed;
        if(family=="GEMV") {
            p::need(prepared.bundle==&model_.gemv->source(call),
                "hybrid GEMV original prepared source");
            result=std::make_unique<tiny_full::GemvBinding>(*model_.gemv,call,count,mapper_);
        } else if(family=="SiLU") {
            p::need(bool(model_.legacy->silu)&&prepared.bundle==&model_.legacy->silu->source(),
                "hybrid SiLU original prepared source");
            result=std::make_unique<tiny_full::SiluBinding>(*model_.legacy->silu,call,count,mapper_);
        } else if(family=="FusedNorm") {
            p::need(bool(model_.legacy->fused)&&prepared.bundle==&model_.legacy->fused->source(call),
                "hybrid FusedNorm original prepared source");
            result=std::make_unique<tiny_full::FusedNormBinding>(*model_.legacy->fused,call,count,mapper_);
        } else if(family=="PlainNorm") {
            p::need(bool(model_.legacy->norm)&&prepared.bundle==&model_.legacy->norm->source(call),
                "hybrid PlainNorm original phase-selected source");
            result=std::make_unique<tiny_full::PlainNormBinding>(*model_.legacy->norm,call,count,mapper_);
        } else if(family=="Rotary") {
            p::need(bool(model_.legacy->rotary)&&prepared.class_bundle==&model_.legacy->rotary->source(call),
                "hybrid Rotary original class/body catalog");
            result=std::make_unique<tiny_full::RotaryBinding>(*model_.legacy->rotary,call,count,mapper_);
        } else {
            p::need(canonical_full::is_gemm(family)&&frames_.contains(family),
                "hybrid original Prefill frame required");
            result=prefill_templates_.bind(family,frame_identities_.at(family),[&] {
                return frames_.with_decoded(family,[&](const std::string& raw) {
                    return frame_bridge::parse(raw);
                });
            },call,count,mapper_);
            p::need(frames_.receipt().at("live_decoded_bytes")==0,
                "hybrid Prefill decoded frame released after typed construction");
            ++prefill_frame_bindings_;
        }
        validate(prepared,*result);++bound_calls_;return result;
    }
    J receipt() const {
        return {{"schema","HYBRID_BORROWED_NINE_PROVIDER_BINDINGS_V1"},
            {"canonical_Model_constructed_here",false},{"source_catalogs_duplicated",false},
            {"selected_call_index_entries",selected_.size()},{"bound_tiny_calls",bound_calls_},
            {"Prefill_frame_bindings",prefill_frame_bindings_},
            {"Prefill_template_cache",prefill_templates_.receipt()},
            {"maximum_selected_CTAs_per_call",max_call_ctas_},
            {"maximum_shared_SourceNode_count_per_call",max_template_nodes_},
            {"maximum_source_template_classes_per_call",max_template_classes_},
            {"SourceNode_arrays_are_per_CTA",false},{"full_trace_saved",false},
            {"per_call_metadata_scope","Provider template/class nodes shared; original Prefill span vector and Rotary CTA class/evidence may scale with selected CTAs. Dynamic runtime state is owned separately by resident-only TinyContext."},
            {"source_finish_obligation","Caller retains canonical_full::Model::finish() exactly once after the workflow."},
            {"retirement_scope","Tiny uses original-source work/token/CTA ledgers; it does not claim original fine Builder per-node timing/retire hashes."}};
    }
};
} // namespace hybrid_full
