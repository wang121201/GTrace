#pragma once
// Include after the original full-workflow Model, Prepared and hybrid Bindings.
// This consumes their native memory rules. It never invokes a scheduler or
// submits requests to HBFSIM. Unsupported bindings retain the exact fine Builder
// one CTA at a time and project its global subops without executing that DAG.
#include "direct_cache.h"
#include "direct_phase_profile.h"
#include <chrono>

namespace direct_native {
inline bool global_op(g::OpType op) {
    return op==g::OpType::LD_DRAM2REG||op==g::OpType::ST_REG2DRAM||
           op==g::OpType::CP_DRAM2SRAM_LDGSTS;
}
inline Context fine_context(const g::DAGNode& n,U call,U cta) {
    Context out;out.call_index=call;out.cta=cta;out.node_id=U(n.id);out.sm_id=U(n.sm_id);
    // Only these original Builders publish warp:ordinal:pc. Do not invent PC
    // or warp metadata for older Builders whose graph_node_id is empty.
    const auto first=n.graph_node_id.find(':');
    const auto last=n.graph_node_id.rfind(':');
    if(first!=std::string::npos&&last!=first) {
        out.warp=std::stoull(n.graph_node_id.substr(0,first));
        out.pc=std::stoull(n.graph_node_id.substr(last+1));
    }
    return out;
}
template<class Builder>
inline U project_fine(Builder& builder,const canonical_full::Prepared& prepared,
                      U call_index,FunctionalCache& cache) {
    U peak=0,total=0;
    for(U cta=0;cta<prepared.executed;++cta) {
        auto nodes=builder.build(int(cta)); // released before the next CTA
        peak=std::max(peak,U(nodes.size()));total+=nodes.size();
        for(const auto& owner:nodes) {
            const auto& n=*owner;
            if(!global_op(n.op_type))continue;
            require(!n.explicit_memory_subops.empty(),"direct fine global source lacks exact byte ranges");
            cache.instruction(n.matrix_id,n.op_type==g::OpType::ST_REG2DRAM,
                n.op_type==g::OpType::CP_DRAM2SRAM_LDGSTS&&n.async_copy_bypass_l1,
                n.explicit_memory_subops,fine_context(n,call_index,cta));
        }
    }
    require(total==prepared.total_nodes,"direct fine selected CTA node census differs");
    return peak;
}
inline U fallback(const canonical_full::Model& model,compressed_frame::Cache& frames,
                  const coupling::ServiceMapper& mapper,
                  const canonical_full::Prepared& prepared,U call,FunctionalCache& cache) {
    const auto& family=prepared.family;
    if(canonical_full::is_helper(family)) {
        helper_bridge::Builder builder(model.helpers->at(prepared.call.at("source_launch_key")),prepared.executed);
        return project_fine(builder,prepared,call,cache);
    }
    if(canonical_full::is_attention(family)) {
        auto catalog=frames.with_decoded(family,[&](const std::string& raw){
            return std::make_unique<canonical_full::GemmCatalog>(prepared.t,prepared.executed,raw);
        });
        canonical_full::AttentionBuilder builder(*catalog->attentionmodel);
        return project_fine(builder,prepared,call,cache);
    }
    if(family=="IndexPut") {
        canonical_indexput::ModeledBuilder builder(*model.legacy->indexput,prepared.call,
            *prepared.index_bundle,mapper,prepared.executed);
        return project_fine(builder,prepared,call,cache);
    }
    if(family=="PrefillCopy") {
        canonical_prefillcopy::ModeledBuilder builder(*model.legacy->copy,prepared.call,
            *prepared.copy_bundle,mapper,prepared.executed);
        return project_fine(builder,prepared,call,cache);
    }
    throw std::runtime_error("direct unsupported native family: "+family);
}
inline J run(const J& in,compressed_frame::Cache& frames,bool full,native_trace::Writer& writer,U phase_ctas=0) {
    using namespace native_sequence;
    const auto started=std::chrono::steady_clock::now();
    auto model_owner=frames.with_decoded("Helpers",[&](const std::string& raw){
        return std::make_unique<canonical_full::Model>(in,raw);
    });
    auto& model=*model_owner;
    // Fail before executing any call or emitting cache traffic. This optional
    // static profile must never silently construct a fine fallback CTA graph.
    if(phase_ctas)for(const auto& target:model.calls)
        if(!hybrid_full::Bindings::supports(target.family))
            throw std::runtime_error("direct stage profile lacks native binding for family: "+target.family);
    std::vector<std::unique_ptr<canonical_full::Prepared>> prepared;
    U node_count=0,cta_count=0;
    for(const auto& target:model.calls) {
        prepared.push_back(std::make_unique<canonical_full::Prepared>(model,target,false));
        node_count+=prepared.back()->total_nodes;cta_count+=prepared.back()->executed;
    }
    require(!full||(prepared.size()==1138&&model.available.size()==1138&&model.prefix==0),
            "direct full workflow requires original1138 call order and full grids");
    require(full||(prepared.size()<=64&&node_count<=15000000),
            "direct bounded selection exceeded without explicit full workflow");

    // Validate the same sealed memory-profile contract, but construct only the
    // address mapper. No HbmDevice, native service engine or GPU session exists.
    const auto cfg=g::make_rtx4000_ada_footprint_reference_config();
    J phase_profile=phase_ctas?direct_phase::profile(cfg,phase_ctas):J();
    const auto& profile=in.at("memory_model");
    require(profile.at("schema")=="SG_FRAGMENT_MEMORY_PROFILE_V1"&&
        profile.at("qualification")=="EXPLICIT_UNCALIBRATED_MODEL_PARAMETERS"&&
        profile.at("core_profile")=="pinned_RTX4000_Ada_footprint_reference",
        "direct requires original explicit memory profile identity");
    require(profile.at("backend")=="hbf_gddr6"&&profile.at("hbm_timing_scale").get<double>()==1.0,
        "direct source profile differs from native GDDR6 baseline");
    const auto& clock=profile.at("clock");
    sg_hbf::Clock checked_clock(coupling::natural(clock.at("period_ps_numerator")),
        coupling::natural(clock.at("period_ps_denominator")));
    const double mhz=1e6*double(checked_clock.cycle_denominator)/double(checked_clock.ps_numerator);
    require(std::abs(mhz-cfg.core_frequency_mhz)<1e-8,"direct source clock profile differs");
    const auto native=sg_hbf::native_config_file(profile.at("native_hbfsim_config_file").get<std::string>());
    coupling::match_reference(profile.at("gddr6"),native);
    require(coupling::natural(profile.at("max_live"))==4096&&
        coupling::natural(profile.at("credits_per_pc"))==32,"direct source native admission profile differs");
    (void)coupling::path_config(profile.at("memory_path"));
    native_sequence::Mapper source_mapper;
    coupling::ServiceMapper mapper(in.at("service_address_map"),source_mapper,native.device.capacity_bytes);
    require(!cfg.l2_bypass_cache&&cfg.l2_line_size_bytes==128,"direct cache supports pinned native cache policy");
    FunctionalCache cache(cfg.per_sm_l1,U(cfg.l2_cache_size_bytes),
        [&](int matrix,U line){return mapper.map(g::CacheLineKey{matrix,line});},
        [&](const native_trace::Record& record){writer.append(record);});
    hybrid_full::Bindings bindings(model,frames,mapper);
    J rows=J::array();U fast_calls=0,fallback_calls=0,peak_fallback_nodes=0;
    const auto engine_started=std::chrono::steady_clock::now();
    for(U index=0;index<prepared.size();++index) {
        const auto& item=*prepared[index];cache.begin_kernel();
        const auto before=cache.snapshot();const auto call_started=std::chrono::steady_clock::now();
        const bool fast=hybrid_full::Bindings::supports(item.family);
        if(fast) {
            auto binding=bindings.bind(item);
            std::unique_ptr<direct_phase::Call> phase_call;
            if(phase_ctas)phase_call=std::make_unique<direct_phase::Call>(*binding,cfg,index);
            U group_begin=0,record_begin=phase_ctas?before.at("trace_records").get<U>():0;
            for(U cta=0;cta<binding->ctas();++cta) {
                if(phase_call)phase_call->observe_cta(cta);
                const auto nodes=binding->nodes(cta);
                for(U member=0;member<nodes.size();++member) {
                    const auto& node=nodes[member];
                    if(node.kind!=tiny_full::Kind::Global&&node.kind!=tiny_full::Kind::AsyncCopy)continue;
                    auto memory=binding->memory(cta,unsigned(member));
                    require(memory.path!=tiny_full::Path::Shared,"direct global binding unexpectedly shared");
                    Context context;context.call_index=index;context.cta=cta;context.warp=node.warp;
                    context.pc=node.pc;context.node_id=binding->first_node(cta)+node.id;context.sm_id=cta%cfg.num_sms;
                    if(memory.global_subops.empty()) {
                        require(memory.global_bytes==0,"direct empty subops have nonzero bytes");
                        continue; // exact zero-source async copy, no DRAM request
                    }
                    U bytes=0;for(const auto& sub:memory.global_subops)bytes+=sub.requested_bytes;
                    require(bytes==memory.global_bytes,"direct binding byte census differs");
                    cache.instruction(1,memory.write,memory.bypass_l1,memory.global_subops,context);
                }
                if(phase_call&&(cta+1-group_begin==phase_ctas||cta+1==binding->ctas())) {
                    const U record_end=cache.snapshot().at("trace_records").get<U>();
                    auto& stages=phase_profile.at("stages");
                    stages.push_back(phase_call->finish_stage(U(stages.size()),group_begin,cta+1,record_begin,record_end));
                    group_begin=cta+1;record_begin=record_end;
                }
            }
            if(phase_call)phase_profile.at("calls").push_back(phase_call->ledger());
            ++fast_calls;
        } else {
            peak_fallback_nodes=std::max(peak_fallback_nodes,fallback(model,frames,mapper,item,index,cache));
            ++fallback_calls;
        }
        const auto after=cache.snapshot();
        const U read=after.at("source_read_bytes").get<U>()-before.at("source_read_bytes").get<U>();
        const U write=after.at("source_write_bytes").get<U>()-before.at("source_write_bytes").get<U>();
        require(read==item.expected_read&&write==item.expected_write,
            "direct original Prepared requested R/W byte census differs");
        require(frames.receipt().at("live_decoded_bytes")==0,"direct leaked decoded frame");
        rows.push_back({{"family",item.family},{"source_launch_key",item.call.at("source_launch_key")},
            {"phase",item.call.at("phase")},{"CTAs",item.executed},{"source_nodes",item.total_nodes},
            {"route",fast?"native_binding_no_per_CTA_DAG":"original_fine_builder_one_CTA_projection"},
            {"source_read_bytes",read},{"source_write_bytes",write},
            {"DRAM_read_bytes",after.at("DRAM_read_bytes").get<U>()-before.at("DRAM_read_bytes").get<U>()},
            {"DRAM_write_bytes",after.at("DRAM_write_bytes").get<U>()-before.at("DRAM_write_bytes").get<U>()},
            {"before",before},{"after",after},
            {"host_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-call_started).count()}});
        std::cerr<<J({{"progress_completed_calls",index+1},{"total_calls",prepared.size()},
            {"family",item.family},{"mode","functional-direct"}}).dump()<<std::endl;
    }
    const double engine_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-engine_started).count();
    cache.verify_resident_ledger();model.finish();
    J result={{"schema","NATIVE_MEMORY_FUNCTIONAL_DIRECT_V1"},{"status","COMPLETED"},
        {"mode","functional-direct"},{"ordering","canonical_call_then_CTA_ascending_then_original_member_then_subop_first_line_occurrence"},
        {"source_memory_rules","same original canonical Model and native bindings; one-CTA original fine Builder fallback"},
        {"source_identity",model.workflow.at("process")},{"workflow_file",canonical_full::seals().at("workflow_file")},
        {"selected_source_count",prepared.size()},{"registered_source_count",model.available.size()},
        {"all_registered_calls_executed",full&&prepared.size()==1138},
        {"selected_CTAs",cta_count},{"source_nodes",node_count},
        {"native_binding_calls",fast_calls},{"fine_builder_projection_calls",fallback_calls},
        {"peak_live_fallback_DAG_nodes",peak_fallback_nodes},
        {"compute_DAG_scheduled",false},{"HBFSIM_constructed",false},{"HBFSIM_executed",false},
        {"compute_work_executed",false},{"cycle_timestamps_available",false},
        {"timing_equivalence_to_cosim",false},{"cache_after_stream_equivalence_to_cosim",false},
        {"native_target_qualified",false},{"implicit_register_dependencies_complete",false},
        {"estimated_address",true},{"hardware_timing_calibrated",false},
        {"cache_completion_policy","serial immediate completion; no in-flight MSHR merging or latency"},
        {"cache_identity","same process/context source virtual128B lines; original packed service map"},
        {"cache_replacement","fully_associative_LRU"},{"L2_bytes",cfg.l2_cache_size_bytes},
        {"L1_policy",{{"implementation","original PerSmL1Cache"},{"mode",g::per_sm_l1_mode_name(cfg.per_sm_l1.mode)},
            {"persistence",g::per_sm_l1_persistence_name(cfg.per_sm_l1.persistence)},
            {"SMS",cfg.num_sms},{"bytes_per_SM",cfg.per_sm_l1.capacity_bytes_per_sm},{"ways",cfg.per_sm_l1.ways}}},
        {"read_fill_and_store_RFO_bytes",128},{"writeback_record_bytes",32},
        {"writeback_attribution","eviction trigger context, not last writer"},
        {"initial_cache_state","cold"},{"per_call_L2_reset",false},{"final_dirty_flush",false},
        {"cache",cache.snapshot()},{"pipeline",rows},{"bindings",bindings.receipt()},
        {"compressed_frame_cache",frames.receipt()},{"source_pool",model.receipt()},
        {"host_engine_seconds",engine_seconds},
        {"host_total_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()}};
    if(phase_ctas)result["phase_profile"]=std::move(phase_profile);
    return result;
}
} // namespace direct_native
