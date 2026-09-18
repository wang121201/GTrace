#include "trace_replay.h"
#include "work/tilegen-norm-shared-r1/adapter/integration.h"
#include <fstream>
#include <iostream>
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
J run(const J& spec){
    need(spec.at("schema")=="TILEGEN_TRACE_REPLAY_INPUT_V1","unknown replay input schema");
    const auto transport=J::parse(verified(spec,"control",1<<20));
    const auto source=J::parse(verified(spec,"source_result",128<<20));
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
    trace_replay::Replay replay(clock,cfg,coupling::natural(profile.at("max_live")),
        coupling::natural(profile.at("credits_per_pc")),drain,coupling::natural(spec.at("max_cycles")),call_count);
    // Validation is streaming. All simulation state is private and provisional
    // until whole-file SHA, footer, per-call census and final drain pass.
    const auto trace=native_trace::validate(spec.at("trace_file").get<std::string>(),expected.at("file_sha256"),
        coupling::natural(spec.at("max_trace_bytes")),[&](const native_trace::Record& record){
        need(record.source_matrix_id==1&&record.call_index<call_count,"trace source/call context differs");
        const U base=mapper.map({1,record.source_line_address});
        need(record.service_address>=base&&record.service_address-base<128&&
            record.bytes<=128-(record.service_address-base),"trace service address differs from declared source map");
        replay.accept(record);
    });
    const auto checked=trace.to_json();
    for(const auto* key:{"mode","context_sha256","records","read_requests","write_requests","read_bytes","write_bytes",
                        "file_bytes","record_sha256","file_sha256","request_payload_fnv1a64"})
        need(checked.at(key)==expected.at(key),"source trace receipt census/hash differs from replay input");
    auto out=replay.finish();
    need(out.at("requests")==trace.records&&out.at("request_payload_fnv1a64")==trace.request_payload_fnv1a64,"replay/input trace census differs");
    U seen_calls=0;
    for(auto& row:out.at("calls")){
        const U index=row.at("call_index").get<U>();const auto& original=source.at("pipeline").at(index);
        need(row.at("read_bytes")==original.at("DRAM_read_bytes")&&row.at("write_bytes")==original.at("DRAM_write_bytes"),"replay per-call traffic differs from source direct run");
        row["source_launch_key"]=original.at("source_launch_key");row["family"]=original.at("family");row["phase"]=original.at("phase");++seen_calls;
    }
    // Calls producing no DRAM records do not appear in replay.calls.
    for(U i=0;i<call_count;++i){const auto& original=source.at("pipeline").at(i);
        if(original.at("DRAM_read_bytes")==0&&original.at("DRAM_write_bytes")==0)++seen_calls;}
    need(seen_calls==call_count,"source nonempty call missing in replay");
    out["trace"]=checked;out["input_manifest"]=spec;out["batch_size"]=1;
    out["memory_model"]=coupling::native_identity(cfg,config_path);
    out["native_config_sha256"]=tiny_sha::sha256(config_bytes);
    out["source_declared_config_file"]=profile.at("native_hbfsim_config_file");
    out["original_direct_config_bytes_were_pinned"]=false;
    out["config_provenance_scope"]="replay cfg snapshot/hash and resolved profile; historical direct receipt pins cfg path, not original cfg bytes";
    out["cosim_timing_equivalent"]=false;out["source_final_dirty_flush"]=source.at("final_dirty_flush");
    out["selected_source_count"]=call_count;out["selected_CTAs"]=source.at("selected_CTAs");
    out["qualification"]="MEMORY_ONLY_SATURATED_REPLAY_NOT_INFERENCE_LATENCY_OR_HARDWARE_CALIBRATION";
    return out;
}
}
int main(int argc,char** argv){
    try{need(argc==2,"usage: tilegen_replay replay-input.json");
        const auto result=run(J::parse(read_file(argv[1],1<<20)));
        std::cout<<result.dump(2)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"memory-only replay failed: "<<e.what()<<'\n';return 1;}
}
