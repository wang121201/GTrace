#define TILEGEN_NO_EXECUTABLE_MAIN
#include "../source/work/tilegen-full-r1/canonical-full-runtime-r4/streaming.cpp"

namespace projection_test {
using namespace native_sequence;
using namespace canonical_full;
struct FineOwner {
    std::shared_ptr<const GemmCatalog> catalog;
    std::unique_ptr<P28Builder> p28;
    std::unique_ptr<NextBuilder> next;
    std::unique_ptr<canonical_gemv::ModeledBuilder> gemv;
    std::unique_ptr<canonical_silu::ModeledBuilder> silu;
    std::unique_ptr<canonical_fused::ModeledBuilder> fused;
    std::unique_ptr<canonical_norm::ModeledBuilder> norm;
    std::unique_ptr<canonical_rotary::ModeledBuilder> rotary;
    FineOwner(const Model& model,const canonical_full::Prepared& k,compressed_frame::Cache& frames,
              const coupling::ServiceMapper& mapper) {
        if(is_gemm(k.family)) {
            catalog=frames.with_decoded(k.family,[&](const std::string& raw){
                return std::make_shared<const GemmCatalog>(k.t,k.executed,raw);
            });
            if(k.family=="P28QKV")p28=std::make_unique<P28Builder>(*catalog->p28model);
            else next=std::make_unique<NextBuilder>(*catalog->nextmodel);
        } else if(k.family=="GEMV")gemv=std::make_unique<canonical_gemv::ModeledBuilder>(*model.gemv,k.call,*k.bundle,mapper,k.executed);
        else if(k.family=="SiLU")silu=std::make_unique<canonical_silu::ModeledBuilder>(*model.legacy->silu,k.call,*k.bundle,mapper,k.executed);
        else if(k.family=="FusedNorm")fused=std::make_unique<canonical_fused::ModeledBuilder>(*model.legacy->fused,k.call,*k.bundle,mapper,k.executed);
        else if(k.family=="PlainNorm")norm=std::make_unique<canonical_norm::ModeledBuilder>(*model.legacy->norm,k.call,*k.bundle,mapper,k.executed);
        else if(k.family=="Rotary")rotary=std::make_unique<canonical_rotary::ModeledBuilder>(*model.legacy->rotary,k.call,*k.class_bundle,mapper,k.executed);
        else p::need(false,"projection test only accepts exact nine native binding families");
    }
    g::CtaGraphStore::Owned first_CTA() {
        if(p28)return p28->build(0);if(next)return next->build(0);if(gemv)return gemv->build(0);
        if(silu)return silu->build(0);if(fused)return fused->build(0);if(norm)return norm->build(0);
        return rotary->build(0);
    }
};
inline void word(tiny_sha::Sha256& digest,U value) {
    std::array<char,8> b{};for(unsigned i=0;i<8;++i)b[i]=char(value>>(i*8));digest.add(b.data(),b.size());
}
inline void compare(const tiny_full::MemoryDescriptor& projected,const g::DAGNode& original,
                    tiny_sha::Sha256& digest,U& ranges,U& bytes) {
    const bool write=original.op_type==g::OpType::ST_REG2DRAM;
    const bool bypass=original.op_type==g::OpType::CP_DRAM2SRAM_LDGSTS&&original.async_copy_bypass_l1;
    p::need(projected.write==write&&projected.bypass_l1==bypass&&original.matrix_id==1,
        "native binding projection changed op/cache operator/matrix identity");
    const auto& actual=projected.global_subops;const auto& expected=original.explicit_memory_subops;
    p::need(actual.size()==expected.size(),"native projection changed subop partition count");
    word(digest,write);word(digest,bypass);word(digest,U(original.matrix_id));word(digest,actual.size());
    U count_bytes=0;
    for(U sub=0;sub<actual.size();++sub) {
        const auto& a=actual[sub];const auto& e=expected[sub];
        p::need(a.requested_bytes==e.requested_bytes&&a.source_member_ordinals==e.source_member_ordinals&&
            a.ranges.size()==e.ranges.size(),"native projection changed exact source lanes/ranges/multiplicity");
        word(digest,a.requested_bytes);word(digest,a.source_member_ordinals.size());
        for(const auto lane:a.source_member_ordinals)word(digest,U(lane));
        word(digest,a.ranges.size());
        for(U i=0;i<a.ranges.size();++i) {
            const auto& x=a.ranges[i];const auto& y=e.ranges[i];
            p::need(x.source_member_ordinal==y.source_member_ordinal&&x.offset_bytes==y.offset_bytes&&
                x.byte_count==y.byte_count,"native projection changed lane/address/width or range order");
            word(digest,U(x.source_member_ordinal));word(digest,x.offset_bytes);word(digest,x.byte_count);
            ++ranges;
        }
        count_bytes+=a.requested_bytes;
    }
    p::need(count_bytes==projected.global_bytes,"native projection descriptor byte total changed");bytes+=count_bytes;
}
J run() {
    const auto started=std::chrono::steady_clock::now();
    const auto transport=compressed_frame::read_control(std::cin);const auto control=transport.at("decoded_control");
    compressed_frame::Cache frames(transport.at("frames"));
    for(U i=0;i<transport.at("frames").size();++i)frames.read_one(std::cin);frames.finish(std::cin);
    p::need(frames.size()==8,"projection test exact eight compressed frames");
    std::map<std::string,J> manifest;
    for(const auto& q:control.at("frames"))p::need(manifest.emplace(q.at("key"),q).second,"unique decoded frame identity");
    for(const auto& q:frames.decoded_manifest())p::need(manifest.at(q.at("key"))==q,"exact decoded frame SHA/length");
    auto owner=frames.with_decoded("Helpers",[&](const std::string& raw){return std::make_unique<Model>(control,raw);});
    auto& model=*owner;Mapper source;
    const auto native=sg_hbf::native_config_file(control.at("memory_model").at("native_hbfsim_config_file").get<std::string>());
    coupling::ServiceMapper mapper(control.at("service_address_map"),source,native.device.capacity_bytes);
    hybrid_full::Bindings bindings(model,frames,mapper);J rows=J::array();std::set<std::string> seen;
    for(const auto& target:model.calls) {
        if(!hybrid_full::Bindings::supports(target.family)||!seen.insert(target.family).second)continue;
        const canonical_full::Prepared k(model,target,false);auto binding=bindings.bind(k);
        FineOwner fine(model,k,frames,mapper);auto original=fine.first_CTA();
        std::vector<const g::DAGNode*> expected;
        for(const auto& n:original)if(direct_native::global_op(n->op_type))expected.push_back(n.get());
        U compared=0,ranges=0,bytes=0;tiny_sha::Sha256 digest;
        for(U member=0;member<binding->nodes(0).size();++member) {
            const auto kind=binding->nodes(0)[member].kind;
            if(kind!=tiny_full::Kind::Global&&kind!=tiny_full::Kind::AsyncCopy)continue;
            p::need(compared<expected.size(),"native binding introduced extra global instruction");
            compare(binding->memory(0,unsigned(member)),*expected[compared],digest,ranges,bytes);++compared;
        }
        p::need(compared==expected.size(),"native binding dropped a global instruction");
        rows.push_back({{"family",target.family},{"source_launch_key",k.call.at("source_launch_key")},
            {"CTA",0},{"memory_instructions",compared},{"ranges",ranges},{"source_bytes",bytes},
            {"ordered_exact_projection_sha256",digest.hex()},{"status","PASS_EXACT_RANGE_SEQUENCE"}});
        std::cerr<<J({{"projection_family",target.family},{"status","PASS_EXACT_RANGE_SEQUENCE"}}).dump()<<std::endl;
    }
    p::need(seen.size()==9,"projection input must cover all nine native binding families");model.finish();
    return {{"status","PASS_ALL_NINE_NATIVE_BINDINGS_MATCH_ORIGINAL_FINE_BUILDERS"},
        {"comparison","instruction direction, bypass, matrix, subop partition, lane provenance, exact address and width in order"},
        {"node_ids_compared",false},{"scope","first CTA of first selected launch for each native binding family"},
        {"GPU_or_HBFSIM_executed",false},{"transport_control_sha256",tiny_sha::sha256(transport.dump())},
        {"families",rows},{"host_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()}};
}
}
int main() {
    try {std::cout<<projection_test::run().dump()<<'\n';return 0;}
    catch(const std::exception& error) {std::cerr<<nlohmann::json({{"status","FAIL"},{"reason",error.what()}}).dump()<<'\n';return 2;}
}
