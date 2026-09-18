#include "trace_replay.h"
#include "stage_replay.h"
#include "hbf_replay_backend.h"
#include "replay_phase_report.h"
#include "work/tilegen-norm-shared-r1/adapter/integration.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

namespace {
using J=nlohmann::json;using U=std::uint64_t;
void need(bool x,const char* why){native_trace::need(x,why);}
std::string read_file(const std::string& path,U cap){
    std::ifstream in(path,std::ios::binary);need(bool(in),"cannot read replay input file");
    std::string data;std::array<char,65536> chunk{};
    while(in){in.read(chunk.data(),chunk.size());const auto n=in.gcount();
        need(U(n)<=cap-data.size(),"replay metadata exceeds size limit");data.append(chunk.data(),std::size_t(n));}
    need(in.eof(),"replay metadata read failed");return data;
}
std::string verified(const J& spec,const char* name,U cap){
    const auto& item=spec.at(name);auto bytes=read_file(item.at("path").get<std::string>(),cap);
    need(tiny_sha::sha256(bytes)==item.at("sha256").get<std::string>(),"replay metadata SHA mismatch");return bytes;
}
struct Identity final:GTSim::L2DramAddressMapper{
    U map(const GTSim::CacheLineKey& k)const override{need(k.matrix_id==1,"expected process VA root 1");return k.line_addr;}
};
bool declares_option(const std::string& bytes,const std::string& option){
    std::istringstream stream(bytes);std::string line;
    while(std::getline(stream,line)){
        const auto begin=line.find_first_not_of(" \t\r");
        if(begin==std::string::npos||line[begin]=='#')continue;
        const auto equal=line.find('=',begin);if(equal==std::string::npos)continue;
        const auto end=line.find_last_not_of(" \t\r",equal-1);
        if(end!=std::string::npos&&line.substr(begin,end-begin+1)==option)return true;
    }return false;
}
void check_target_family(const hbfsim::physical::hbm::HbmConfig& cfg,
                         const std::string& kind,const std::string& bytes){
    const auto& d=cfg.device;
    // HbmDevice::standard is a fixed core identity even for the GDDR overlay.
    // Gate the supported interface organizations, not that static string.
    if(kind=="gddr6")need(d.pseudo_channels_per_channel==1&&d.channel_width_bits==16&&d.burst_length==16,
        "gddr6 target requires the supported x16/1-PC/BL16 numeric overlay organization");
    if(kind=="hbm")need(d.pseudo_channels_per_channel==2&&d.channel_width_bits==64&&d.burst_length==8,
        "hbm target requires the supported x64/2-PC/BL8 HBM organization");
    if(kind=="hbf")need(declares_option(bytes,"hbf-standard"),
        "hbf target config must explicitly declare hbf-standard; implicit default HBF is not a target identity");
}
J target_identity(const hbfsim::physical::hbm::HbmConfig& cfg,const std::string& kind,const std::string& path){
    const J resolved=coupling::native_identity(cfg,path);J out;
    for(const auto* key:{"resolved_device","resolved_timing","resolved_controller","derived","address_mapping_scheme"})out[key]=resolved.at(key);
    out["kind"]=kind;out["native_config_file"]=path;out["raw_core_standard"]=std::string(cfg.standard);
    out["raw_core_type"]="hbfsim::physical::hbm::HbmDevice";
    out["qualification"]="EXPLICIT_UNCALIBRATED_MEMORY_CONFIGURATION";
    out["identity_policy"]="explicit requested family plus supported interface organization; raw core standard is preserved, not a JEDEC conformance qualification";
    out["hardware_timing_calibrated"]=false;return out;
}
J run(const J& spec){
    need(spec.at("schema")=="TILEGEN_TRACE_REPLAY_INPUT_V1","unknown replay input schema");
    const auto mode=spec.value("mode",std::string("memory-only-replay"));
    need(mode=="memory-only-replay"||mode=="stage-overlap-replay","unknown replay mode");
    const bool staged=mode=="stage-overlap-replay";
    const auto backend=spec.value("backend",std::string("source-gddr6"));
    need(backend=="source-gddr6"||backend=="gddr6"||backend=="hbm"||backend=="hbf","unknown target backend");
    need(backend!="hbf"||staged,"HBF logical backend requires explicit stage mode");
    const auto transport=J::parse(verified(spec,"control",1<<20));
    const auto source_bytes=verified(spec,"source_result",128<<20);
    const auto source=J::parse(source_bytes);
    const auto receipt=J::parse(verified(spec,"source_run_receipt",1<<20));
    const auto config_bytes=verified(spec,"native_config",1<<20);
    need(source.at("schema")=="NATIVE_MEMORY_FUNCTIONAL_DIRECT_V1"&&source.at("mode")=="direct"&&
        source.at("status")=="COMPLETED"&&source.at("batch_size")==1&&source.at("writeback_request_bytes")==32&&
        source.at("full_selected_trace_saved")==true,"a complete B1 direct source trace is required");
    need(receipt.at("status")=="PASS"&&receipt.at("mode")=="direct"&&receipt.at("batch_size")==1&&
        receipt.at("input_sha256")==spec.at("source_input_sha256"),"source run receipt is not a matching successful direct run");
    const auto& expected=source.at("trace");
    need(expected.at("status")=="PASS_CLOSED_TRACE_READBACK"&&expected.at("mode")==native_trace::mode_name(native_trace::Mode::FunctionalDirect),"source trace receipt must be closed direct trace");
    const auto context_sha=tiny_sha::sha256(transport.dump());
    need(context_sha==source.at("transport_control_sha256").get<std::string>()&&context_sha==expected.at("context_sha256").get<std::string>(),"direct transport context differs");
    const auto& control=transport.at("decoded_control");const auto& profile=control.at("memory_model");
    need(profile.at("schema")=="SG_FRAGMENT_MEMORY_PROFILE_V1"&&profile.at("backend")=="hbf_gddr6"&&
        profile.at("qualification")=="EXPLICIT_UNCALIBRATED_MODEL_PARAMETERS"&&profile.at("hbm_timing_scale")==1.0,
        "replay requires the explicit uncalibrated native GDDR6 profile");
    const auto config_path=spec.at("native_config").at("path").get<std::string>();
    const auto cfg=sg_hbf::native_config_file(config_path);
    need(read_file(config_path,1<<20)==config_bytes,"native config changed while resolving");
    coupling::match_reference(profile.at("gddr6"),cfg);
    auto target_cfg=cfg;hbfsim::app::SystemConfig target_system;
    std::string target_path=config_path,target_bytes=config_bytes;
    if(backend!="source-gddr6"){
        target_bytes=verified(spec,"target_config",1<<20);
        target_path=spec.at("target_config").at("path").get<std::string>();
        hbfsim::app::SystemConfigBuilder builder;builder.apply_file(target_path);target_system=builder.resolve();
        need(read_file(target_path,1<<20)==target_bytes,"target config changed while resolving");
        target_cfg=target_system.hbm;
        check_target_family(target_cfg,backend,target_bytes);
    }else need(!spec.contains("target_config"),"source backend cannot override sealed native config");
    const auto& cj=profile.at("clock");
    sg_hbf::Clock clock(coupling::natural(cj.at("period_ps_numerator")),coupling::natural(cj.at("period_ps_denominator")));
    const auto drain_name=spec.at("drain").get<std::string>();
    need(drain_name=="global"||drain_name=="independent","unsupported native drain");
    const auto drain=drain_name=="global"?sg_hbf::DrainMode::Global:sg_hbf::DrainMode::Independent;
    Identity identity;coupling::ServiceMapper mapper(control.at("service_address_map"),identity,cfg.device.capacity_bytes);
    const U call_count=coupling::natural(source.at("selected_source_count"));
    need(call_count>0&&call_count<=65536&&source.at("pipeline").size()==call_count,"invalid selected call count");
    need(control.at("selected_source_keys").size()==call_count,"selected source count differs from sealed control");
    for(U i=0;i<call_count;++i)need(source.at("pipeline").at(i).at("source_launch_key")==control.at("selected_source_keys").at(i),
        "pipeline call label/order differs from sealed control");
    need(source.at("cache").at("DRAM_read_bytes")==expected.at("read_bytes")&&
        source.at("cache").at("DRAM_write_bytes")==expected.at("write_bytes"),"source cache/trace byte census differs");
    J phases;
    if(staged){
        need(receipt.at("phase_profile_exported")==true&&receipt.at("result_sha256")==tiny_sha::sha256(source_bytes),
            "phase profile/result differs from generating run receipt SHA");
        phases=source.at("phase_profile");
        need(phases.at("schema")=="TILEGEN_CTA_STAGE_PROFILE_V1"&&
            phases.at("qualification")=="EXPLICIT_UNCALIBRATED_STAGE_APPROXIMATION",
            "stage replay requires an explicitly qualified direct phase profile");
        std::vector<U> covered(call_count,0);U previous_call=0;
        for(const auto& stage:phases.at("stages")){
            const U call=coupling::natural(stage.at("call_index"));
            need(call<call_count&&call>=previous_call,"invalid stage call order");
            const U begin=coupling::natural(stage.at("cta_begin")),end=coupling::natural(stage.at("cta_end"));
            need(begin==covered[call]&&end>begin&&end<=coupling::natural(source.at("pipeline").at(call).at("CTAs")),
                "stage CTA partition differs from source direct call");
            covered[call]=end;previous_call=call;
        }
        for(U i=0;i<call_count;++i)need(covered[i]==coupling::natural(source.at("pipeline").at(i).at("CTAs")),
            "stage profile does not cover every selected CTA");
    }
    std::unique_ptr<trace_replay::Replay> memory;
    std::unique_ptr<stage_replay::Replay> stages;
    J seed_receipt=nullptr;
    auto check_record=[&](const native_trace::Record& record){
        need(record.source_matrix_id==1&&record.call_index<call_count,"trace source/call context differs");
        const U base=mapper.map({1,record.source_line_address});
        need(record.service_address>=base&&record.service_address-base<128&&
            record.bytes<=128-(record.service_address-base),"trace service address differs from declared source map");
    };
    if(backend=="hbf"){
        need(spec.at("hbf_seed_policy")=="ALL_TOUCHED_SERVICE_PAGES_INITIALLY_RESIDENT_MUTABLE","explicit HBF initial image policy required");
        need(target_system.hbf.device.page_size_bytes==4096,"HBF replay requires native 4096 B logical pages");
        const U page_slots=(cfg.device.capacity_bytes+4095)/4096,limit=coupling::natural(spec.at("max_hbf_seed_pages"));
        need(page_slots<=8388608&&limit>0&&limit<=8388608,"HBF prepass bitmap/seed page bound exceeded");
        std::vector<bool> touched(std::size_t(page_slots),false);U count=0;
        const auto prepass=native_trace::validate(spec.at("trace_file").get<std::string>(),expected.at("file_sha256"),
            coupling::natural(spec.at("max_trace_bytes")),[&](const native_trace::Record& record){
                check_record(record);const U page=record.service_address/4096;
                need(page<page_slots&&record.bytes<=4096-record.service_address%4096,"HBF source request crosses seed page");
                if(!touched[std::size_t(page)]){need(count<limit,"HBF seed page budget exceeded");touched[std::size_t(page)]=true;++count;}
            });
        std::vector<hbf_replay_backend::PageRange> ranges;J rows=J::array();
        for(U page=0;page<page_slots;){if(!touched[std::size_t(page)]){++page;continue;}
            const U first=page;while(page<page_slots&&touched[std::size_t(page)])++page;
            ranges.push_back({first,page-first});rows.push_back({{"first_lpn",first},{"page_count",page-first}});
        }
        seed_receipt={{"policy",spec.at("hbf_seed_policy")},{"page_size_bytes",4096},{"pages",count},
            {"ranges",rows},{"range_sha256",tiny_sha::sha256(rows.dump())},{"trace_sha256",prepass.file_sha256},
            {"logical_address_mapping","unchanged packed source service byte address reinterpreted as HBF logical address"},
            {"qualification","all touched pages initially provisioned; allocation lifetime and real initial contents not recovered; no preload time charged"}};
        auto target=std::make_unique<hbf_replay_backend::Backend>(clock,target_system,ranges,coupling::natural(spec.at("hbf_max_live")));
        stages=std::make_unique<stage_replay::Replay>(clock,std::move(target),phases,
            coupling::natural(spec.at("prefetch_stages")),coupling::natural(spec.at("max_cycles")));
    }else if(staged)stages=std::make_unique<stage_replay::Replay>(clock,target_cfg,phases,
        coupling::natural(spec.at("prefetch_stages")),coupling::natural(profile.at("max_live")),
        coupling::natural(profile.at("credits_per_pc")),drain,coupling::natural(spec.at("max_cycles")));
    else memory=std::make_unique<trace_replay::Replay>(clock,target_cfg,coupling::natural(profile.at("max_live")),
        coupling::natural(profile.at("credits_per_pc")),drain,coupling::natural(spec.at("max_cycles")),call_count);
    // Validation is streaming. All simulation state is private and provisional
    // until whole-file SHA, footer, per-call census and final drain pass.
    const auto trace=native_trace::validate(spec.at("trace_file").get<std::string>(),expected.at("file_sha256"),
        coupling::natural(spec.at("max_trace_bytes")),[&](const native_trace::Record& record){
        check_record(record);
        if(stages)stages->accept(record);else memory->accept(record);
    });
    const auto checked=trace.to_json();
    for(const auto* key:{"mode","context_sha256","records","read_requests","write_requests","read_bytes","write_bytes",
                        "file_bytes","record_sha256","file_sha256","request_payload_fnv1a64"})
        need(checked.at(key)==expected.at(key),"source trace receipt census/hash differs from replay input");
    auto out=stages?stages->finish():memory->finish();
    need(out.at("requests")==trace.records&&out.at("request_payload_fnv1a64")==trace.request_payload_fnv1a64,"replay/input trace census differs");
    std::vector<bool> seen_calls(call_count,false);
    for(auto& row:out.at("calls")){
        const U index=row.at("call_index").get<U>();const auto& original=source.at("pipeline").at(index);
        need(row.at("read_bytes")==original.at("DRAM_read_bytes")&&row.at("write_bytes")==original.at("DRAM_write_bytes"),"replay per-call traffic differs from source direct run");
        need(!seen_calls[index],"duplicate replay call result");seen_calls[index]=true;
        row["source_launch_key"]=original.at("source_launch_key");row["family"]=original.at("family");row["phase"]=original.at("phase");
    }
    // Calls producing no DRAM records do not appear in replay.calls.
    for(U i=0;i<call_count;++i){const auto& original=source.at("pipeline").at(i);
        if(original.at("DRAM_read_bytes")==0&&original.at("DRAM_write_bytes")==0)seen_calls[i]=true;}
    need(std::all_of(seen_calls.begin(),seen_calls.end(),[](bool seen){return seen;}),"source nonempty call missing in replay");
    out["trace"]=checked;out["input_manifest"]=spec;out["batch_size"]=1;
    out["memory_model"]=coupling::native_identity(cfg,config_path);
    out["memory_model_scope"]="sealed source GDDR6/cache/address-map profile; execution target is target_backend";
    out["target_backend"]=target_identity(target_cfg,backend,target_path);
    out["target_backend"]["config_sha256"]=tiny_sha::sha256(target_bytes);
    if(backend=="hbf"){
        // Its typed native physical/FTL snapshot is supplied by the backend.
        out["target_backend"]["raw_core_type"]="hbfsim::host::HbfController + physical::hbf::HbfDevice";
        out["target_backend"]["raw_core_standard"]=std::string(target_system.hbf.device.standard);
        out["target_backend"]["HBM_parameters_scope"]="attached controller buffer only; not the HBF media geometry";
        out["hbf_initial_image"]=seed_receipt;
    }
    out["native_config_sha256"]=tiny_sha::sha256(config_bytes);
    out["source_declared_config_file"]=profile.at("native_hbfsim_config_file");
    out["original_direct_config_bytes_were_pinned"]=false;
    out["config_provenance_scope"]="replay cfg snapshot/hash and resolved profile; historical direct receipt pins cfg path, not original cfg bytes";
    out["cosim_timing_equivalent"]=false;out["source_final_dirty_flush"]=source.at("final_dirty_flush");
    out["selected_source_count"]=call_count;out["selected_CTAs"]=source.at("selected_CTAs");
    out["qualification"]=staged?"EXPLICIT_UNCALIBRATED_STAGE_APPROXIMATION":
        "MEMORY_ONLY_SATURATED_REPLAY_NOT_INFERENCE_LATENCY_OR_HARDWARE_CALIBRATION";
    if(staged){out["phase_profile"]=phases;out["phase_profile_sha256"]=tiny_sha::sha256(phases.dump());replay_phase_report::annotate(out,source);}
    return out;
}
}
int main(int argc,char** argv){
    try{need(argc==2,"usage: tilegen_replay replay-input.json");
        const auto result=run(J::parse(read_file(argv[1],1<<20)));
        std::cout<<result.dump(2)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"trace replay failed: "<<e.what()<<'\n';return 1;}
}
