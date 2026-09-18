#pragma once
// New provider only: do not overlay the frozen PrefillBinding header. The old
// class remains independently includable for the source/address oracle.
#include "../tilegen-tiny-full-r1/source.h"
#include "../tilegen-tiny-full-r1/prefill_support.h"
#include "../tilegen-tiny-full-r1/prefill_seals.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_set>

namespace tiny_full {
class CachedPrefillBinding;

// Private construction + const shared ownership prevents a post-seal mutable
// alias. Both typed and SourceNode arrays are once-per-template, never per CTA.
class SealedPrefillTemplate final {
    friend class CachedPrefillBinding;
    std::string family_;
    J frame_identity_;
    std::vector<p28::Node> p28_nodes_;
    std::vector<prefill_gemm::Node> next_nodes_;
    std::vector<SourceNode> nodes_;
    J evidence_static_;
    U ranges_=0;

    template<class Model>
    void initialize(const Model& model,const J& call,U count) {
        nodes_.reserve(model.nodes.size());
        U completion=0, issue=0, elements=0, fma=0;
        J kinds=J::object();
        for (std::size_t i=0; i<model.nodes.size(); ++i) {
            const auto& n=model.nodes[i];
            SourceNode s;
            s.id=unsigned(i); s.warp=unsigned(n.warp);
            s.ordinal=unsigned(n.ordinal); s.source_ordinal=U(n.old);
            s.pc=U(n.pc); s.mask=n.effective; s.pipeline=n.pipe;
            s.write=n.memop=="W";
            for(int d:n.deps) s.completion_dependencies.push_back(unsigned(d));
            for(int d:n.issues) s.issue_dependencies.push_back(unsigned(d));
            completion+=n.deps.size(); issue+=n.issues.size();
            if(n.kind=="async_copy") {
                s.kind=Kind::AsyncCopy; s.pipeline="LD";
            } else if(n.kind=="global") {
                s.kind=Kind::Global; s.pipeline=s.write?"ST":"LD";
            } else if(n.kind=="shared" || n.kind=="shared_matrix") {
                s.kind=Kind::Shared; s.pipeline=s.write?"ST":"LD";
            } else if(n.kind=="tensor") {
                s.kind=Kind::Tensor; s.pipeline="Tensor"; s.tensor_fma=n.fma;
                p::need(s.tensor_fma==2048,"tiny Prefill exact HMMA work");
            } else if(n.kind=="barrier") {
                s.kind=Kind::Barrier; s.pipeline="BARRIER";
            } else {
                p::need(n.kind=="compute" || n.kind=="control" ||
                        n.kind=="async_commit" || n.kind=="async_wait",
                        "tiny Prefill closed compute/control kinds");
                s.kind=n.kind=="compute"?Kind::Compute:Kind::Control;
                s.compute_elements=U(n.elements);
            }
            elements+=s.compute_elements; fma+=s.tensor_fma;
            if(!kinds.contains(n.kind)) kinds[n.kind]=0;
            kinds[n.kind]=kinds[n.kind].template get<U>()+1;
            nodes_.push_back(std::move(s));
        }
        const J& seal=prefill_seals().at("frame_nodes").at(family_);
        p::need(nodes_.size()==p::natural(seal.at("nodes")) &&
                completion+issue==p::natural(seal.at("edges")),
                "tiny Prefill original template nodes/typed edges");
        evidence_static_={{"schema","TINY_PREFILL_SOURCE_EVIDENCE_V1"},
            {"family",family_},{"source_launch_key",call.at("source_launch_key")},
            {"selected_CTAs",count},{"warps_per_CTA",4},{"native_resident_limit",1},
            {"nodes_per_CTA",nodes_.size()},{"completion_edges_per_CTA",completion},
            {"issue_edges_per_CTA",issue},{"kinds_per_CTA",kinds},
            {"scalar_compute_elements_per_CTA",elements},
            {"declared_tensor_FMA_per_CTA",fma},
            {"global_read_bytes_per_CTA",model.read},
            {"global_write_bytes_per_CTA",model.write},
            {"shared_read_bytes_per_CTA",model.shared_read},
            {"shared_write_bytes_per_CTA",model.shared_write},
            {"ranges_per_CTA",model.ranges},{"async_copies_per_CTA",model.copies},
            {"zero_source_async_copies_per_CTA",model.zero_copies},
            {"tensor_nodes_per_CTA",model.tensors},
            {"frame_node_seal",seal},
            {"original_model_validation_preserved",true},
            {"per_CTA_DAGNode_allocation",false},{"full_trace_saved",false},
            {"addresses","original canonical call pointer + source offset + CTA stride"},
            {"native_hardware_timing_qualified",false},
            {"implicit_register_dependencies_complete",false},
            {"canonical_call",call}};
        evidence_static_.erase("source_launch_key");
        evidence_static_.erase("selected_CTAs");
        evidence_static_.erase("canonical_call");
    }


    SealedPrefillTemplate(const std::string& family,const J& frame_identity,
                         J&& frame,const J& call,U count)
        :family_(family),frame_identity_(frame_identity) {
        p::need(frame_identity.is_object()&&frame_identity.size()==3&&
            frame_identity.contains("key")&&frame_identity.contains("bytes")&&
            frame_identity.contains("sha256")&&frame_identity.at("key")==family&&
            p::natural(frame_identity.at("bytes"))>0&&
            frame_identity.at("sha256").is_string(),"exact decoded template identity");
        p::need(prefill_seals().at("plans").contains(family),"tiny Prefill family");
        (void)check_pin(prefill_seals().at("sealed_inputs_pin"));
        const J plan=strict_parse(check_pin(prefill_seals().at("plans").at(family)));
        bool matched=false;
        for(const auto& c:plan.at("calls"))
            if(c.at("source_launch_key")==call.at("source_launch_key")) {
                p::need(!matched && c==call,"tiny Prefill canonical call exact identity");
                matched=true;
            }
        p::need(matched && count>0 && count<=p::natural(call.at("grid")[0]),
                "tiny Prefill actual call/prefix");
        const J& seal=prefill_seals().at("frame_nodes").at(family);
        { const std::string raw=frame.at("nodes").dump();
          p::need(raw.size()==p::natural(seal.at("bytes")) &&
                  tiny_sha::sha256(raw)==seal.at("sha256").get<std::string>(),
                  "tiny Prefill original RAM frame nodes seal"); }
        const std::array<std::string,3> roles={"weight","input","output"};
        std::array<U,3> strides;
        if(family=="P28QKV") {
            p::need(call.at("role")=="self_attn.qkv_proj" &&
                    call.at("grid")==J::array({48,1,1}),"tiny Prefill QKV domain");
            strides={1048576,0,256};
        } else {
            const std::string role=family=="GEMMO"?"o":family=="GEMMGate"?"gate":"down";
            const auto profile=prefill_gemm::profile(role);
            p::need(frame.at("role")==role && call.at("grid")==J::array({profile.grid,1,1}) &&
                    call.at("native_resources").at("static_shared_bytes")==49152 &&
                    call.at("native_resources").at("dynamic_shared_bytes")==profile.shared-49152 &&
                    call.at("native_resources").at("registers")==profile.regs,
                    "tiny Prefill role/native resources");
            strides=profile.strides;
        }
        J objects=J::array();
        for(unsigned i=0;i<3;++i) {
            const J& o=call.at("objects").at(roles[i]);
            objects.push_back({{"role",roles[i]},{"logical_identity",o.at("logical_identity")},
                {"pointer",o.at("pointer")},{"bytes",o.at("bytes")},{"cta_stride",strides[i]}});
        }
        frame["objects"]=std::move(objects); frame["ctas"]=count;
        if(family=="P28QKV") {
            p28::Model model(frame);initialize(model,call,count);
            ranges_=model.ranges; p28_nodes_=std::move(model.nodes);
        } else {
            prefill_gemm::Model model(frame);initialize(model,call,count);
            ranges_=model.ranges; next_nodes_=std::move(model.nodes);
        }
        // No frame DOM, first-call objects, CTA spans, or absolute request lines
        // survive construction. Only typed relative source data is moved out.

    }
public:
    SealedPrefillTemplate(const SealedPrefillTemplate&)=delete;
    SealedPrefillTemplate& operator=(const SealedPrefillTemplate&)=delete;
    static std::shared_ptr<const SealedPrefillTemplate> create(
            const std::string& family,const J& identity,J&& frame,
            const J& call,U count) {
        return std::shared_ptr<const SealedPrefillTemplate>(
            new SealedPrefillTemplate(family,identity,std::move(frame),call,count));
    }
    const std::string& family() const {return family_;}
    const J& frame_identity() const {return frame_identity_;}
    std::span<const SourceNode> nodes() const {return nodes_;}
    U ranges() const {return ranges_;}
    U edges() const {
        return p::add(p::natural(evidence_static_.at("completion_edges_per_CTA")),
                      p::natural(evidence_static_.at("issue_edges_per_CTA")));
    }
    // Source identity only, not a compiled Program or mutable runtime state.
    J identity() const {return {{"family",family_},{"decoded_frame",frame_identity_},
        {"node_seal",prefill_seals().at("frame_nodes").at(family_)},
        {"plan_pin",prefill_seals().at("plans").at(family_)},
        {"provider_contract","SEALED_PREFILL_TYPED_TEMPLATE_V1"}};}
};

class CachedPrefillBinding final : public KernelBinding {
    std::shared_ptr<const SealedPrefillTemplate> template_;
    const coupling::ServiceMapper& mapper_;
    std::vector<p28::Object> objects_;
    J evidence_;
    U ctas_=0;

    // Recheck the small canonical source files for every call, as before. This
    // version caches only the large immutable frame/template, not plan I/O.
    void validate_call(const J& call) {
        const auto& family=template_->family_;
        p::need(prefill_seals().at("plans").contains(family),"tiny Prefill family");
        (void)check_pin(prefill_seals().at("sealed_inputs_pin"));
        const J plan=strict_parse(check_pin(prefill_seals().at("plans").at(family)));
        bool matched=false;
        for(const auto& c:plan.at("calls"))
            if(c.at("source_launch_key")==call.at("source_launch_key")) {
                p::need(!matched&&c==call,"tiny Prefill canonical call exact identity");
                matched=true;
            }
        p::need(matched&&ctas_>0&&ctas_<=p::natural(call.at("grid")[0]),
                "tiny Prefill actual call/prefix");
        const bool qkv=family=="P28QKV";
        const std::array<std::string,3> roles={"weight","input","output"};
        std::array<U,3> sizes,strides;
        if(qkv) {
            p::need(call.at("role")=="self_attn.qkv_proj"&&
                call.at("grid")==J::array({48,1,1}),"tiny Prefill QKV domain");
            sizes={50331648,262144,393216};strides={1048576,0,256};
        } else {
            const auto profile=prefill_gemm::profile(family=="GEMMO"?"o":family=="GEMMGate"?"gate":"down");
            p::need(call.at("grid")==J::array({profile.grid,1,1})&&
                call.at("native_resources").at("static_shared_bytes")==49152&&
                call.at("native_resources").at("dynamic_shared_bytes")==profile.shared-49152&&
                call.at("native_resources").at("registers")==profile.regs,
                "tiny Prefill role/native resources");
            sizes=profile.sizes;strides=profile.strides;
        }
        // These adapters select the same original arithmetic/error domain as
        // the family Model. Object order and nested disjoint checks are intact.
        auto integer=[&](const J& x){return qkv?p28::integer(x):prefill_gemm::integer(x);};
        auto add=[&](U a,U b){return qkv?p28::add(a,b):prefill_gemm::add(a,b);};
        auto mul=[&](U a,U b){return qkv?p28::mul(a,b):prefill_gemm::mul(a,b);};
        auto need=[&](bool ok,const std::string& message){
            if(qkv)p28::need(ok,message);else prefill_gemm::need(ok,message);
        };
        objects_.reserve(3);
        for(unsigned i=0;i<3;++i) {
            const J& o=call.at("objects").at(roles[i]);
            p28::Object v{roles[i],o.at("logical_identity"),integer(o.at("pointer")),
                          integer(o.at("bytes")),strides[i]};
            need(v.role==roles[i]&&v.bytes==sizes[i]&&v.stride==strides[i]&&
                 !v.logical.empty()&&v.pointer%128==0,"closed role geometry");
            (void)add(v.pointer,v.bytes);objects_.push_back(std::move(v));
        }
        for(int i=0;i<3;++i)for(int k=i+1;k<3;++k)
            need(add(objects_[i].pointer,objects_[i].bytes)<=objects_[k].pointer||
                 add(objects_[k].pointer,objects_[k].bytes)<=objects_[i].pointer,
                 "disjoint call roles");
        // Deliberately retain every original per-call range check in original
        // node/range order. No max-extent certificate or address cache here.
        auto ranges=[&](const auto& nodes) {
            for(const auto& n:nodes)for(const auto& x:n.global) {
                const auto& o=objects_.at(x.role);
                need(add(add(x.offset,mul(ctas_-1,o.stride)),x.width)<=o.bytes,
                     "whole selected grid width inside own role");
            }
        };
        if(qkv)ranges(template_->p28_nodes_);else ranges(template_->next_nodes_);
        if(qkv) {
            need(template_->ranges_<=500000&&mul(template_->ranges_,ctas_)<=24000000&&
                 mul(template_->nodes_.size(),ctas_)<=2000000,"finite declared graph capacity");
        } else {
            const auto profile=prefill_gemm::profile(family=="GEMMO"?"o":family=="GEMMGate"?"gate":"down");
            const U live_nodes=U(std::min<U>(ctas_,96))*U(profile.perwarp)*4;
            const U live_ranges=U(std::min<U>(ctas_,96))*profile.ranges;
            need(template_->ranges_==profile.ranges&&live_ranges<=40820736&&live_nodes<=4190592,
                 "finite declared graph capacity");
        }
        p::need(p::multiply(ctas_,U(template_->nodes_.size()))<=U(INT32_MAX),
                "tiny Prefill original int span domain");
    }

    template<class Model>
    MemoryDescriptor materialize(const Model& model,U cta,unsigned member) const {
        p::need(cta<ctas_ && member<model.nodes.size(),"tiny Prefill memory bounds");
        const auto& n=model.nodes[member];
        p::need(n.kind=="global" || n.kind=="async_copy" ||
                n.kind=="shared" || n.kind=="shared_matrix",
                "tiny Prefill memory called on nonmemory node");
        MemoryDescriptor result;
        result.write=n.memop=="W";
        result.path=n.kind=="async_copy"?Path::AsyncGlobalToShared:
                    n.kind=="global"?Path::DirectGlobal:Path::Shared;
        result.bypass_l1=n.kind=="async_copy";
        if(n.kind=="global" || n.kind=="async_copy") {
            g::ExplicitMemorySubop sub;
            sub.ranges.reserve(n.global.size());
            sub.source_member_ordinals.reserve(n.global.size());
            std::unordered_set<U> seen;
            for(const auto& range:n.global) {
                const auto& object=model.objects.at(range.role);
                const U address=p28::add(object.pointer,
                    p28::add(range.offset,p28::mul(cta,object.stride)));
                sub.ranges.push_back({range.lane,address,range.width});
                sub.source_member_ordinals.push_back(range.lane);
                sub.requested_bytes=p28::add(sub.requested_bytes,range.width);
                p::need(range.width && address<=UINT64_MAX-(range.width-1),
                        "tiny Prefill global range overflow");
                const U last=(address+range.width-1)/128*128;
                for(U line=address/128*128;;line+=128) {
                    const g::CacheLineKey key{1,line};
                    (void)mapper_.map(key);
                    if(seen.insert(line).second) result.lines.push_back(key);
                    if(line==last) break;
                }
            }
            result.global_bytes=sub.requested_bytes;
            // Preserve the empty explicit subop of a zero-source async copy.
            result.global_subops.push_back(std::move(sub));
        }
        if(n.kind=="shared" || n.kind=="shared_matrix" || n.kind=="async_copy") {
            g::ExplicitMemorySubop sub;
            sub.ranges.reserve(n.shared.size());
            sub.source_member_ordinals.reserve(n.shared.size());
            for(const auto& range:n.shared) {
                sub.ranges.push_back({range.lane,range.offset,range.width});
                sub.source_member_ordinals.push_back(range.lane);
                sub.requested_bytes=p28::add(sub.requested_bytes,range.width);
            }
            result.shared_bytes=sub.requested_bytes;
            result.shared_service=g::describe_explicit_sram_ranges(sub);
            result.shared_subops.push_back(std::move(sub));
        }
        return result;
    }

    template<class Node>struct BoundView {
        const std::vector<Node>& nodes;
        const std::vector<p28::Object>& objects;
    };
public:
    CachedPrefillBinding(std::shared_ptr<const SealedPrefillTemplate> source,
                        const J& call,U count,const coupling::ServiceMapper& mapper)
        :template_(std::move(source)),mapper_(mapper),ctas_(count) {
        p::need(bool(template_),"nonnull sealed Prefill template");
        validate_call(call);
        evidence_=template_->evidence_static_;
        evidence_["source_launch_key"]=call.at("source_launch_key");
        evidence_["selected_CTAs"]=ctas_;evidence_["canonical_call"]=call;
    }
    U ctas() const override {return ctas_;}
    unsigned warps(U cta) const override {p::need(cta<ctas_,"tiny Prefill CTA");return 4;}
    unsigned resident_limit() const override {return 1;}
    U first_node(U cta) const override {
        p::need(cta<ctas_,"tiny Prefill span CTA");
        return p::multiply(cta,U(template_->nodes_.size()));
    }
    std::span<const SourceNode> nodes(U cta) const override {
        p::need(cta<ctas_,"tiny Prefill nodes CTA");return template_->nodes_;
    }
    U template_class(U cta) const override {p::need(cta<ctas_,"tiny Prefill class CTA");return 0;}
    MemoryDescriptor memory(U cta,unsigned member) const override {
        if(template_->family_=="P28QKV")
            return materialize(BoundView<p28::Node>{template_->p28_nodes_,objects_},cta,member);
        return materialize(BoundView<prefill_gemm::Node>{template_->next_nodes_,objects_},cta,member);
    }
    J evidence() const override {return evidence_;}
    const std::shared_ptr<const SealedPrefillTemplate>& template_owner() const {return template_;}
};

// Session-owned, four-key closed cache. It must be destroyed before the source
// catalog/mapper, while bindings independently keep their const template alive.
// Cache miss publication happens only after the new call also validates.
class PrefillTemplateCache final {
    std::map<std::string,std::shared_ptr<const SealedPrefillTemplate>> templates_;
    U hits_=0,misses_=0,nodes_=0,edges_=0,ranges_=0;
public:
    PrefillTemplateCache()=default;
    PrefillTemplateCache(const PrefillTemplateCache&)=delete;
    PrefillTemplateCache& operator=(const PrefillTemplateCache&)=delete;
    template<class Loader>
    std::unique_ptr<KernelBinding> bind(const std::string& family,const J& frame_identity,
            Loader load,const J& call,U count,const coupling::ServiceMapper& mapper) {
        p::need(prefill_seals().at("plans").contains(family),"tiny Prefill family");
        auto it=templates_.find(family);
        if(it!=templates_.end()) {
            p::need(it->second->frame_identity()==frame_identity,
                    "sealed Prefill cache frame identity unchanged");
            auto binding=std::make_unique<CachedPrefillBinding>(it->second,call,count,mapper);
            ++hits_;return binding;
        }
        p::need(templates_.size()<4,"at most four closed Prefill templates");
        auto source=SealedPrefillTemplate::create(family,frame_identity,load(),call,count);
        const U nodes=p::add(nodes_,source->nodes().size());
        const U edges=p::add(edges_,source->edges());
        const U ranges=p::add(ranges_,source->ranges());
        p::need(nodes<=252944&&edges<=1560144&&ranges<=2524320,
                "four original template source capacity");
        auto binding=std::make_unique<CachedPrefillBinding>(source,call,count,mapper);
        p::need(templates_.emplace(family,std::move(source)).second,"unique sealed template publish");
        nodes_=nodes;edges_=edges;ranges_=ranges;++misses_;return binding;
    }
    J receipt() const {
        return {{"schema","PREFILL_SEALED_TYPED_CACHE_V1"},{"templates",templates_.size()},
            {"hits",hits_},{"misses",misses_},{"typed_template_nodes",nodes_},
            {"typed_template_edges",edges_},{"typed_template_ranges",ranges_},
            {"maximum_templates",4},{"retained_frame_JSON_DOMs",0},
            {"absolute_request_address_cache",false},{"per_call_range_checks_retained",true},
            {"source_arrays_copied_per_call",false},{"full_trace_saved",false},
            {"bytes_are_RSS",false}};
    }
};
} // namespace tiny_full
