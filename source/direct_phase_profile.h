#pragma once
// Include after the native SourceNode/KernelBinding and SimulatorConfig types.
// Static work accounting only: no DAG expansion, scheduler or timing runtime.
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace direct_phase {
using U = std::uint64_t;
using J = nlohmann::json;
inline void require(bool ok,const char* why) {
    if(!ok)throw std::runtime_error(why);
}
inline U add(U a,U b) {
    if(b>std::numeric_limits<U>::max()-a)throw std::overflow_error("stage work overflow");
    return a+b;
}
inline U multiply(U a,U b) {
    if(a&&b>std::numeric_limits<U>::max()/a)throw std::overflow_error("stage work overflow");
    return a*b;
}
inline U ceil_div(U n,U d) {
    require(d>0,"stage work has zero service rate");
    return n/d+U(n%d!=0);
}
inline constexpr std::array<const char*,4> pipelines={"SIMD","SFU","SHFL","Tensor"};

struct Work {
    U nodes=0,compute_nodes=0,control_nodes=0,tensor_nodes=0;
    U scalar_elements=0,tensor_fma=0,subops=0,reservation_cycles=0,tail_latency_cycles=0;
    J json()const {
        return {{"nodes",nodes},{"compute_nodes",compute_nodes},{"control_nodes",control_nodes},
            {"tensor_nodes",tensor_nodes},{"modeled_scalar_elements",scalar_elements},
            {"declared_tensor_FMA",tensor_fma},{"issue_subops",subops},
            {"reservation_cycles",reservation_cycles},{"tail_latency_cycles",tail_latency_cycles},
            {"estimated_service_cycles",nodes?add(reservation_cycles,tail_latency_cycles):0}};
    }
};

struct TemplateWork {
    const tiny_full::SourceNode* identity=nullptr;
    U node_count=0,warps=0,selected_ctas=0,compute_cycles=0;
    U global_nodes=0,shared_nodes=0,async_nodes=0,barrier_nodes=0;
    std::array<std::array<Work,4>,4> resources{};

    TemplateWork(const tiny_full::KernelBinding& binding,U cta,const GTSim::SimulatorConfig& cfg) {
        const auto nodes=binding.nodes(cta);identity=nodes.data();node_count=nodes.size();warps=binding.warps(cta);
        require(node_count>0&&warps>0,"stage template is empty");
        for(const auto& node:nodes) {
            require(node.warp<warps,"stage source warp out of range");
            using K=tiny_full::Kind;
            if(node.kind==K::Global){++global_nodes;continue;}
            if(node.kind==K::Shared){++shared_nodes;continue;}
            if(node.kind==K::AsyncCopy){++async_nodes;continue;}
            if(node.kind==K::Barrier){++barrier_nodes;continue;}
            unsigned pipeline=0;U subops=0,span=0,latency=0;
            if(node.kind==K::Tensor) {
                require(cfg.tensor_issue_work_semantics==GTSim::TensorIssueWorkSemantics::DECLARED_FMA_WORK&&
                    node.tensor_fma>0&&node.compute_elements==0&&cfg.tensor_core_width>0&&
                    cfg.tensor_core_latency_cycles>=0,"stage tensor work profile differs");
                pipeline=3;subops=1;span=ceil_div(node.tensor_fma,U(cfg.tensor_core_width));
                latency=U(cfg.tensor_core_latency_cycles);
            } else {
                require(node.kind==K::Compute||node.kind==K::Control,"unsupported stage source kind");
                if(node.pipeline=="SIMD")pipeline=0;
                else if(node.pipeline=="SFU")pipeline=1;
                else {require(node.pipeline=="SHFL","unsupported stage scalar pipeline");pipeline=2;}
                const int throughput=pipeline==0?cfg.simd_throughput:pipeline==1?cfg.sfu_throughput:(1<<30);
                const int width=pipeline==0?cfg.simd_width:pipeline==1?cfg.sfu_width:(1<<30);
                const int delay=pipeline==0?cfg.simd_latency_cycles:pipeline==1?cfg.sfu_latency_cycles:cfg.shfl_latency_cycles;
                require(node.compute_elements>0&&throughput>0&&width>0&&delay>=0,"stage scalar work profile differs");
                subops=ceil_div(node.compute_elements,U(throughput));span=ceil_div(U(throughput),U(width));
                latency=U(delay);
            }
            auto& work=resources[node.warp%4][pipeline];
            work.nodes=add(work.nodes,1);
            if(node.kind==K::Compute)++work.compute_nodes;
            else if(node.kind==K::Control)++work.control_nodes;
            else ++work.tensor_nodes;
            work.scalar_elements=add(work.scalar_elements,node.compute_elements);
            work.tensor_fma=add(work.tensor_fma,node.tensor_fma);
            work.subops=add(work.subops,subops);
            work.reservation_cycles=add(work.reservation_cycles,multiply(subops,span));
            work.tail_latency_cycles=std::max(work.tail_latency_cycles,latency);
        }
        for(const auto& sp:resources)for(const auto& work:sp)if(work.nodes)
            compute_cycles=std::max(compute_cycles,add(work.reservation_cycles,work.tail_latency_cycles));
    }
    J json(U class_id)const {
        J subpartitions=J::array();
        for(unsigned sp=0;sp<4;++sp) {
            J work=J::object();for(unsigned p=0;p<4;++p)work[pipelines[p]]=resources[sp][p].json();
            subpartitions.push_back({{"subpartition",sp},{"pipelines",std::move(work)}});
        }
        return {{"template_class",class_id},{"selected_CTAs",selected_ctas},{"nodes_per_CTA",node_count},
            {"warps_per_CTA",warps},{"compute_cycles_per_CTA",compute_cycles},
            {"excluded_nodes_per_CTA",{{"global",global_nodes},{"shared",shared_nodes},
                {"async_copy",async_nodes},{"barrier",barrier_nodes}}},
            {"per_CTA_resources",std::move(subpartitions)}};
    }
};

// Cache scope is one bound call, so equal class IDs in different kernels never
// alias. Only four SP resource bins are accumulated; no per-warp runtime exists.
class Call {
    const tiny_full::KernelBinding& binding_;
    const GTSim::SimulatorConfig& cfg_;
    std::map<U,TemplateWork> templates_;
    std::map<U,U> group_classes_;
    std::vector<U> sm_cycles_;
    U call_index_,next_cta_=0,stage_begin_=0;
public:
    Call(const tiny_full::KernelBinding& binding,const GTSim::SimulatorConfig& cfg,U index)
        :binding_(binding),cfg_(cfg),sm_cycles_(std::size_t(cfg.num_sms),0),call_index_(index) {
        require(cfg.num_sms>0,"stage profile needs positive SM count");
    }
    void observe_cta(U cta) {
        require(cta==next_cta_&&cta<binding_.ctas(),"stage CTA order differs from direct order");
        const U key=binding_.template_class(cta);
        auto it=templates_.find(key);
        if(it==templates_.end())it=templates_.try_emplace(key,binding_,cta,cfg_).first;
        auto& work=it->second;const auto nodes=binding_.nodes(cta);
        require(nodes.data()==work.identity&&nodes.size()==work.node_count&&binding_.warps(cta)==work.warps,
            "stage template class must have one immutable source array");
        work.selected_ctas=add(work.selected_ctas,1);
        auto& population=group_classes_[key];population=add(population,1);
        auto& sm=sm_cycles_[std::size_t(cta%U(cfg_.num_sms))];sm=add(sm,work.compute_cycles);
        ++next_cta_;
    }
    J finish_stage(U id,U cta_begin,U cta_end,U record_begin,U record_end) {
        require(cta_begin==stage_begin_&&cta_end==next_cta_&&cta_end>cta_begin&&record_end>=record_begin,
            "stage range is not a contiguous half-open interval");
        J populations=J::array(),sms=J::array();
        for(const auto& [key,count]:group_classes_)populations.push_back({{"template_class",key},{"CTAs",count}});
        for(U sm=0;sm<sm_cycles_.size();++sm)if(sm_cycles_[sm])
            sms.push_back({{"sm",sm},{"compute_cycles",sm_cycles_[sm]}});
        const U cycles=*std::max_element(sm_cycles_.begin(),sm_cycles_.end());
        J result={{"id",id},{"call_index",call_index_},{"record_begin",record_begin},{"record_end",record_end},
            {"cta_begin",cta_begin},{"cta_end",cta_end},{"compute_cycles",cycles},
            {"template_class_population",std::move(populations)},{"per_SM_compute_cycles",std::move(sms)}};
        std::fill(sm_cycles_.begin(),sm_cycles_.end(),0);group_classes_.clear();stage_begin_=cta_end;
        return result;
    }
    J ledger()const {
        require(next_cta_==binding_.ctas()&&stage_begin_==next_cta_,"stage call census is incomplete");
        J rows=J::array();for(const auto& [key,work]:templates_)rows.push_back(work.json(key));
        return {{"call_index",call_index_},{"source_evidence",binding_.evidence()},{"templates",std::move(rows)}};
    }
};

inline J profile(const GTSim::SimulatorConfig& cfg,U phase_ctas) {
    require(phase_ctas>0&&cfg.num_sms>0,"stage profile grouping must be positive");
    return {{"schema","TILEGEN_CTA_STAGE_PROFILE_V1"},
        {"qualification","EXPLICIT_UNCALIBRATED_STAGE_APPROXIMATION"},
        {"phase_ctas",phase_ctas},{"num_sms",cfg.num_sms},{"subpartitions_per_SM",4},
        {"clock_frequency_MHz",cfg.core_frequency_mhz},{"cycle_unit","GPU core cycle"},
        {"range_convention","zero-based half-open; record ranges include zero-record stages"},
        {"source_work","original immutable SourceNode templates; scalar elements are modeled lane slots, not FLOPs"},
        {"template_cache_scope","call_index and original template_class; exact selected class population"},
        {"compute_estimator","SP=warp%4; per-SP/pipeline sum of reservation cycles plus one maximum pipeline tail latency; CTA=max over resources; SM sums CTA estimates for cta%num_sms; stage=max over SMs"},
        {"scalar_reservation_formula","ceil(elements/throughput)*ceil(throughput/width); SHFL throughput=width=2^30"},
        {"tensor_reservation_formula","ceil(declared_tensor_FMA/tensor_core_width)"},
        {"compute_only",true},{"compute_dependencies_scheduled",false},{"cache_time_omitted",true},
        {"excluded_from_compute_cycles",J::array({"global memory service","shared memory service","async copy service",
            "barrier service and synchronization","warp issue and dependency stalls","kernel launch latency"})},
        {"memory_bundle","all post-cache fill/RFO/dirty-writeback records produced by this CTA group in unchanged direct order"},
        {"writeback_semantics","eviction-triggered traffic, not a compute output dependency"},
        {"overlap_policy","not applied by exporter; downstream stage model must declare overlap assumptions"},
        {"hardware_timing_calibrated",false},{"timing_equivalence_to_cosim",false},
        {"pipeline_profile",{{"SIMD",{{"throughput",cfg.simd_throughput},{"width",cfg.simd_width},{"latency_cycles",cfg.simd_latency_cycles}}},
            {"SFU",{{"throughput",cfg.sfu_throughput},{"width",cfg.sfu_width},{"latency_cycles",cfg.sfu_latency_cycles}}},
            {"SHFL",{{"throughput",1<<30},{"width",1<<30},{"latency_cycles",cfg.shfl_latency_cycles}}},
            {"Tensor",{{"width",cfg.tensor_core_width},{"latency_cycles",cfg.tensor_core_latency_cycles},{"work_unit","declared FMA"}}}}},
        {"stages",J::array()},{"calls",J::array()}};
}
} // namespace direct_phase
