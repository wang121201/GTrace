#include "simulator.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <map>
#include <stdexcept>

// source_support.h only checks that the enclosing canonical source types exist.
// They are unrelated to this static census. Supply test-only placeholders so
// this fixture uses the real SourceNode/KernelBinding without the full workflow.
namespace native_sequence {
using U=std::uint64_t;
using J=nlohmann::json;
namespace g=GTSim;
struct SourceBundle {};
struct SourceCatalog {};
namespace p {inline void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}}
}
#include "work/tilegen-tiny-full-r1/source.h"
#include "direct_phase_profile.h"

using U=std::uint64_t;
using J=nlohmann::json;
using Node=tiny_full::SourceNode;
using Kind=tiny_full::Kind;
static U checks=0;
static void check(bool ok,const char* why) {
    ++checks;if(!ok)throw std::runtime_error(why);
}
template<class F>static void rejects(F&& run,const char* why) {
    bool caught=false;try{run();}catch(const std::exception&){caught=true;}
    check(caught,why);
}
static Node node(Kind kind,unsigned warp,const char* pipeline="",U elements=0,U fma=0) {
    Node n;n.kind=kind;n.warp=warp;n.pipeline=pipeline;n.compute_elements=elements;n.tensor_fma=fma;return n;
}
struct Binding final:tiny_full::KernelBinding {
    std::map<U,std::vector<Node>> classes;
    std::vector<U> population;
    mutable U memory_calls=0;
    U ctas()const override{return population.size();}
    unsigned warps(U)const override{return 8;}
    unsigned resident_limit()const override{return 1;}
    U first_node(U cta)const override {
        U n=0;for(U i=0;i<cta;++i)n+=classes.at(population.at(i)).size();return n;
    }
    std::span<const Node> nodes(U cta)const override{return classes.at(population.at(cta));}
    U template_class(U cta)const override{return population.at(cta);}
    tiny_full::MemoryDescriptor memory(U,unsigned)const override {
        ++memory_calls;throw std::runtime_error("static census must not materialize memory");
    }
    J evidence()const override{return {{"schema","STATIC_STAGE_TEST_SOURCE"}};}
};
static GTSim::SimulatorConfig config() {
    GTSim::SimulatorConfig c;c.num_sms=48;
    c.simd_throughput=32;c.simd_width=32;c.simd_latency_cycles=4;
    c.sfu_throughput=16;c.sfu_width=4;c.sfu_latency_cycles=30;c.shfl_latency_cycles=4;
    c.tensor_core_width=32;c.tensor_core_latency_cycles=32;
    c.tensor_issue_work_semantics=GTSim::TensorIssueWorkSemantics::DECLARED_FMA_WORK;
    return c;
}
static Binding fixture(U ctas) {
    Binding b;b.population.assign(std::size_t(ctas),0);
    b.classes[0]={node(Kind::Compute,0,"SIMD",32),node(Kind::Control,4,"SIMD",16),
        node(Kind::Compute,1,"SFU",32),node(Kind::Compute,5,"SFU",16),
        node(Kind::Compute,2,"SHFL",32),node(Kind::Tensor,3,"Tensor",0,64),
        node(Kind::Global,0),node(Kind::Shared,0),node(Kind::AsyncCopy,0),node(Kind::Barrier,0)};
    return b;
}
static const J& pipeline(const J& ledger,U cls,unsigned sp,const char* name) {
    return ledger.at("templates").at(cls).at("per_CTA_resources").at(sp).at("pipelines").at(name);
}
int main() {
    try {
        const auto cfg=config();auto b=fixture(1);direct_phase::Call one(b,cfg,3);
        one.observe_cta(0);const J stage=one.finish_stage(0,0,1,7,7);const J ledger=one.ledger();
        // Hand calculation: SP0 SIMD=1+1+4=6; SP1 SFU=(2*4)+(1*4)+30=42;
        // SP2 shuffle=1+4=5; SP3 tensor=64/32+32=34. The maximum is 42.
        check(stage.at("compute_cycles")==42,"one CTA resource maximum must be 42, not divided by 48 SMs");
        check(stage.at("record_begin")==7&&stage.at("record_end")==7,"zero-DRAM stage must be retained");
        check(stage.at("per_SM_compute_cycles").size()==1&&stage.at("per_SM_compute_cycles").at(0).at("sm")==0,
            "one CTA must occupy only its assigned SM");
        const auto& simd=pipeline(ledger,0,0,"SIMD");const auto& sfu=pipeline(ledger,0,1,"SFU");
        check(simd.at("nodes")==2&&simd.at("control_nodes")==1&&simd.at("modeled_scalar_elements")==48,
            "warp 0 and warp 4 must share the same SIMD resource");
        check(simd.at("reservation_cycles")==2&&simd.at("estimated_service_cycles")==6,"SIMD tail applied once per resource");
        check(sfu.at("issue_subops")==3&&sfu.at("reservation_cycles")==12&&sfu.at("estimated_service_cycles")==42,
            "SFU must account for subops, issue width, and one tail");
        check(pipeline(ledger,0,2,"SHFL").at("estimated_service_cycles")==5,"shuffle pipeline service");
        check(pipeline(ledger,0,3,"Tensor").at("declared_tensor_FMA")==64&&
            pipeline(ledger,0,3,"Tensor").at("estimated_service_cycles")==34,"declared FMA tensor service");
        check(ledger.at("templates").at(0).at("excluded_nodes_per_CTA")==
            J({{"global",1},{"shared",1},{"async_copy",1},{"barrier",1}}),"excluded kinds must remain visible in census");
        check(b.memory_calls==0,"work accounting must not materialize memory");

        auto many=fixture(49);direct_phase::Call repeated(many,cfg,4);
        for(U i=0;i<49;++i)repeated.observe_cta(i);
        const J wave=repeated.finish_stage(1,0,49,7,20);
        check(wave.at("compute_cycles")==84,"CTA 0 and 48 must serialize on SM 0");
        check(wave.at("per_SM_compute_cycles").size()==48,"49 CTAs use 48 SMs");
        check(repeated.ledger().at("templates").size()==1&&
            repeated.ledger().at("templates").at(0).at("selected_CTAs")==49,"one cached class must preserve exact population");

        direct_phase::Call split(many,cfg,5);
        for(U i=0;i<48;++i)split.observe_cta(i);
        check(split.finish_stage(2,0,48,20,30).at("compute_cycles")==42,"first one-CTA-per-SM wave");
        split.observe_cta(48);
        check(split.finish_stage(3,48,49,30,30).at("compute_cycles")==42,"SM accumulators must reset across waves");

        auto mixed=fixture(3);mixed.population={0,7,0};
        mixed.classes[7]={node(Kind::Compute,0,"SIMD",32)};
        direct_phase::Call classes(mixed,cfg,6);for(U i=0;i<3;++i)classes.observe_cta(i);
        const auto class_stage=classes.finish_stage(4,0,3,30,31);const J census=classes.ledger();
        check(census.at("templates").size()==2&&census.at("templates").at(0).at("selected_CTAs")==2&&
            census.at("templates").at(1).at("selected_CTAs")==1,"nonuniform template class populations");
        check(census.at("templates").at(1).at("compute_cycles_per_CTA")==5&&class_stage.at("compute_cycles")==42,
            "different template classes must retain independent work");
        auto other=fixture(1);other.classes[0]={node(Kind::Compute,0,"SIMD",32)};
        direct_phase::Call other_call(other,cfg,7);other_call.observe_cta(0);
        check(other_call.finish_stage(5,0,1,31,31).at("compute_cycles")==5,"class ID 0 must not alias across calls");

        auto empty=fixture(1);empty.classes[0]={node(Kind::Global,0),node(Kind::Shared,0),node(Kind::Barrier,0)};
        direct_phase::Call zero(empty,cfg,8);zero.observe_cta(0);
        const auto z=zero.finish_stage(6,0,1,31,31);
        check(z.at("compute_cycles")==0&&z.at("record_begin")==z.at("record_end"),"zero compute and zero DRAM stage");
        check(zero.ledger().at("templates").at(0).at("nodes_per_CTA")==3,"excluded-only source retains node census");

        direct_phase::Call invalid(b,cfg,9);
        rejects([&]{invalid.observe_cta(1);},"out-of-order CTA accepted");
        invalid.observe_cta(0);
        rejects([&]{invalid.observe_cta(0);},"duplicate CTA accepted");
        rejects([&]{invalid.finish_stage(7,1,1,0,0);},"noncontiguous or empty CTA interval accepted");
        rejects([&]{invalid.finish_stage(7,0,1,2,1);},"reversed trace interval accepted");
        (void)invalid.finish_stage(7,0,1,0,0);(void)invalid.ledger();
        const auto profile=direct_phase::profile(cfg,48);
        check(profile.at("schema")=="TILEGEN_CTA_STAGE_PROFILE_V1"&&profile.at("compute_only")==true&&
            profile.at("cache_time_omitted")==true&&profile.at("compute_dependencies_scheduled")==false,
            "approximation qualification and omissions must be explicit");
        std::cout<<J({{"status","PASS_DIRECT_PHASE_PROFILE"},{"checks",checks}}).dump()<<'\n';
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
