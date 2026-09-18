#define TILEGEN_NO_EXECUTABLE_MAIN
#include "../source/work/tilegen-full-r1/canonical-full-runtime-r4/streaming.cpp"
#include <iterator>
#include <sstream>

// Independent address oracle: do not use either updated fine Builder here.
// Feed the existing sealed compressed transport on stdin (decode-512.input).
namespace prepared_memory_test {
using namespace native_sequence;

struct MappingSpy final : g::L2DramAddressMapper {
    mutable std::vector<g::CacheLineKey> calls;
    U map(const g::CacheLineKey& key) const override {
        p::need(key.matrix_id==1,"prepared test source matrix identity");
        calls.push_back(key);return key.line_addr;
    }
};

void word(tiny_sha::Sha256& digest,U value) {
    std::array<char,8> bytes{};
    for(unsigned i=0;i<8;++i)bytes[i]=char(value>>(8*i));
    digest.add(bytes.data(),bytes.size());
}

struct Counts {
    U ctas=0,instructions=0,ranges=0,source_ordinals=0,source_bytes=0;
    U lane_formula_checks=0,mapper_checks=0,negative_checks=0;
    tiny_sha::Sha256 digest;
    J result() {
        return {{"CTAs",ctas},{"memory_instructions",instructions},{"ranges",ranges},
            {"source_ordinals",source_ordinals},{"source_bytes",source_bytes},
            {"lane_formula_checks",lane_formula_checks},{"mapper_checks",mapper_checks},
            {"negative_checks",negative_checks},{"ordered_subops_sha256",digest.hex()}};
    }
};

void compare(const g::ExplicitMemorySubop& actual,const g::ExplicitMemorySubop& expected,
             Counts& counts) {
    p::need(actual.requested_bytes==expected.requested_bytes&&
        actual.source_member_ordinals==expected.source_member_ordinals&&
        actual.ranges.size()==expected.ranges.size(),
        "prepared memory changed requested bytes, source ordinals or partition");
    word(counts.digest,actual.requested_bytes);
    word(counts.digest,actual.source_member_ordinals.size());
    for(auto ordinal:actual.source_member_ordinals)word(counts.digest,U(ordinal));
    word(counts.digest,actual.ranges.size());
    for(U i=0;i<actual.ranges.size();++i) {
        const auto& a=actual.ranges[i];const auto& e=expected.ranges[i];
        p::need(a.source_member_ordinal==e.source_member_ordinal&&
            a.offset_bytes==e.offset_bytes&&a.byte_count==e.byte_count,
            "prepared memory changed ordered range address, width or provenance");
        word(counts.digest,U(a.source_member_ordinal));
        word(counts.digest,a.offset_bytes);word(counts.digest,a.byte_count);
    }
    ++counts.instructions;counts.ranges+=actual.ranges.size();
    counts.source_ordinals+=actual.source_member_ordinals.size();
    counts.source_bytes=p::add(counts.source_bytes,actual.requested_bytes);
}

template<class F> void rejects(F&& operation,Counts& counts) {
    bool rejected=false;
    try {operation();}catch(const std::exception&){rejected=true;}
    p::need(rejected,"prepared memory accepted an out-of-domain request");
    ++counts.negative_checks;
}

// Reproduce the old GEMV Builder's exact grouped range construction, using
// Model::address (the JSON formula), not PreparedAddress or PreparedMemory.
g::ExplicitMemorySubop gemv_oracle(const canonical_gemv::Model& model,const J& call,
                                 U record,U cta) {
    const auto& source=model.source(call);
    const auto& memory=source.program.body(0).records.at(record);
    g::ExplicitMemorySubop result;
    result.requested_bytes=p::multiply(memory.lanes.size(),U(memory.width));
    for(const auto& lane:memory.lanes)result.source_member_ordinals.push_back(lane.lane);
    for(const auto& group:source.range_plan.records.at(record)) {
        const auto& lane=memory.lanes.at(group.first);
        result.ranges.push_back({group.count==1?lane.lane:-1,
            model.address(call,lane,group.bytes,cta),group.bytes});
    }
    return result;
}

void gemv_cta(const canonical_gemv::Model& model,const J& call,
              const canonical_gemv::PreparedMemory& prepared,U cta,
              MappingSpy& spy,Counts& counts) {
    const auto& records=model.source(call).program.body(0).records;
    ++counts.ctas;word(counts.digest,cta);
    for(U record=0;record<records.size();++record) {
        const auto expected=gemv_oracle(model,call,record,cta);
        spy.calls.clear();
        const auto actual=prepared.materialize(record,cta);
        std::vector<g::CacheLineKey> expected_mapping;
        for(const auto& range:expected.ranges) {
            p::need(range.byte_count>0&&range.offset_bytes<=UINT64_MAX-(range.byte_count-1),
                    "oracle range overflow");
            const U last=(range.offset_bytes+range.byte_count-1)/128*128;
            for(U line=range.offset_bytes/128*128;;line+=128) {
                expected_mapping.push_back({1,line});if(line==last)break;
            }
        }
        p::need(spy.calls==expected_mapping,
                "GEMV mapping check count, range order or repeated intersection changed");
        counts.mapper_checks+=spy.calls.size();word(counts.digest,record);
        compare(actual,expected,counts);
    }
}

// The old SiLU Builder merged 32 source lanes into one {-1, VA, 32*width}
// range. Check all original lane formulas, not only its two endpoint guards.
void silu_cta(const canonical_silu::Model& model,const J& call,
              const canonical_silu::PreparedMemory& prepared,U cta,Counts& counts) {
    ++counts.ctas;word(counts.digest,cta);
    p::need(prepared.records()==model.records.size(),"SiLU source record bijection");
    for(U record=0;record<model.records.size();++record) {
        const auto& formula=model.records.at(record);
        const U first=model.address(call,cta,formula,0);
        g::ExplicitMemorySubop expected;
        expected.requested_bytes=p::multiply(32,formula.width);
        expected.source_member_ordinals.push_back(-1);
        expected.ranges.push_back({-1,first,expected.requested_bytes});
        for(U lane=0;lane<32;++lane) {
            p::need(model.address(call,cta,formula,lane)==
                p::add(first,p::multiply(lane,formula.width)),
                "SiLU original lane formula is not the prepared contiguous range");
            ++counts.lane_formula_checks;
        }
        word(counts.digest,record);compare(prepared.materialize(cta,record),expected,counts);
    }
}

J run() {
    const auto started=std::chrono::steady_clock::now();
    std::string raw((std::istreambuf_iterator<char>(std::cin)),std::istreambuf_iterator<char>());
    p::need(!std::cin.bad()&&!raw.empty(),"read sealed test transport");
    const U input_bytes=raw.size();const auto input_sha256=tiny_sha::sha256(raw);
    std::istringstream input(std::move(raw));
    const auto transport=compressed_frame::read_control(input);
    const auto control=transport.at("decoded_control");
    compressed_frame::Cache frames(transport.at("frames"));
    for(U i=0;i<transport.at("frames").size();++i)frames.read_one(input);
    frames.finish(input);p::need(frames.size()==8,"test exact eight compressed frames");
    std::map<std::string,J> manifest;
    for(const auto& row:control.at("frames"))
        p::need(manifest.emplace(row.at("key"),row).second,"unique decoded frame identity");
    for(const auto& row:frames.decoded_manifest())
        p::need(manifest.at(row.at("key"))==row,"exact decoded frame SHA and length");
    auto owner=frames.with_decoded("Helpers",[&](const std::string& helper){
        return std::make_unique<canonical_full::Model>(control,helper);
    });
    auto& model=*owner;MappingSpy spy;
    const auto config=sg_hbf::native_config_file(
        control.at("memory_model").at("native_hbfsim_config_file").get<std::string>());
    coupling::ServiceMapper mapper(control.at("service_address_map"),spy,config.device.capacity_bytes);
    J selected=J::array();U gemv_calls=0,silu_calls=0;
    canonical_gemv::host_address::json_reference=false;
    for(const auto& target:model.calls) {
        if(target.family!="GEMV"&&target.family!="SiLU")continue;
        const canonical_full::Prepared kernel(model,target,false);
        Counts counts;J reference_branch=nullptr;
        if(target.family=="GEMV") {
            ++gemv_calls;
            const canonical_gemv::PreparedMemory prepared(*model.gemv,kernel.call,mapper);
            for(U cta=0;cta<kernel.executed;++cta)
                gemv_cta(*model.gemv,kernel.call,prepared,cta,spy,counts);
            rejects([&]{prepared.materialize(kernel.bundle->program.body(0).records.size(),0);},counts);
            // Both paths share the materializer; verify the optional reference
            // branch independently on the selected first/last CTA as well.
            Counts branch;canonical_gemv::host_address::json_reference=true;
            gemv_cta(*model.gemv,kernel.call,prepared,0,spy,branch);
            if(kernel.executed>1)gemv_cta(*model.gemv,kernel.call,prepared,kernel.executed-1,spy,branch);
            canonical_gemv::host_address::json_reference=false;reference_branch=branch.result();
        } else {
            ++silu_calls;
            const canonical_silu::PreparedMemory prepared(*model.legacy->silu,kernel.call);
            for(U cta=0;cta<kernel.executed;++cta)
                silu_cta(*model.legacy->silu,kernel.call,prepared,cta,counts);
            rejects([&]{prepared.materialize(prepared.ctas(),0);},counts);
            rejects([&]{prepared.materialize(0,prepared.records());},counts);
        }
        J row=counts.result();row["family"]=target.family;
        row["source_launch_key"]=kernel.call.at("source_launch_key");
        row["selected_CTAs"]=kernel.executed;row["full_grid_CTAs"]=kernel.call.at("grid")[0];
        row["json_reference_first_last_CTA"]=reference_branch;
        row["status"]="PASS_EXACT_ORIGINAL_FORMULA";selected.push_back(row);
        std::cerr<<J({{"family",target.family},{"source_launch_key",kernel.call.at("source_launch_key")},
            {"selected_CTAs",kernel.executed},{"status","PASS_EXACT_ORIGINAL_FORMULA"}}).dump()<<'\n';
    }
    p::need(gemv_calls>0&&silu_calls>0,"test input must select both GEMV and SiLU");
    Counts boundaries;J phases=J::object();U boundary_calls=0,extra_full_grid_ctas=0;
    for(const auto* call:model.legacy->silu->calls) {
        const canonical_silu::PreparedMemory prepared(*model.legacy->silu,*call);
        silu_cta(*model.legacy->silu,*call,prepared,0,boundaries);
        if(prepared.ctas()>1)
            silu_cta(*model.legacy->silu,*call,prepared,prepared.ctas()-1,boundaries);
        if(prepared.ctas()==32&&extra_full_grid_ctas==0)
            for(U cta=1;cta+1<prepared.ctas();++cta) {
                silu_cta(*model.legacy->silu,*call,prepared,cta,boundaries);++extra_full_grid_ctas;
            }
        rejects([&]{prepared.materialize(prepared.ctas(),0);},boundaries);
        rejects([&]{prepared.materialize(0,prepared.records());},boundaries);
        const auto phase=call->at("phase").get<std::string>();
        phases[phase]=phases.value(phase,U(0))+1;++boundary_calls;
    }
    model.finish();J boundary_result=boundaries.result();
    boundary_result["calls"]=boundary_calls;boundary_result["phase_call_counts"]=phases;
    boundary_result["additional_full_grid_CTAs"]=extra_full_grid_ctas;
    boundary_result["scope"]="Every loaded SiLU model call: full-grid first/last CTA, plus all32 CTAs of the first Prefill call; all744 records and32 lanes";
    boundary_result["status"]="PASS_EXACT_ORIGINAL_FORMULA_AND_DOMAIN_REJECTION";
    return {{"schema","TILEGEN_PREPARED_MEMORY_ORIGINAL_FORMULA_TEST_V1"},
        {"status","PASS_GEMV_SILU_SHARED_MATERIALIZERS_MATCH_ORIGINAL_FORMULAS"},
        {"input_sha256",input_sha256},{"input_bytes",input_bytes},
        {"transport_control_sha256",tiny_sha::sha256(transport.dump())},
        {"scope","All selected GEMV/SiLU CTAs and original memory instructions; no cache, DAG scheduling or simulation"},
        {"selected_calls",selected},{"all_silu_full_grid_boundaries",boundary_result},
        {"GPU_executed",false},{"HBFSIM_executed",false},
        {"host_wall_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()}};
}
} // namespace prepared_memory_test

int main() {
    try {std::cout<<prepared_memory_test::run().dump()<<'\n';return 0;}
    catch(const std::exception& error) {
        std::cerr<<nlohmann::json({{"status","FAIL"},{"reason",error.what()}}).dump()<<'\n';return 2;
    }
}
