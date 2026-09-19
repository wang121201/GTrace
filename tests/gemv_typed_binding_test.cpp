#define main unused_frozen_sequence_main
#include "../source/work/tilegen-full-r1/driver-pooled-fusednorm-r1/streaming.cpp"
#undef main
#include "../source/work/tilegen-full-r1/canonical-gemv-host-r2/prepared_memory.h"
#include "../source/work/tilegen-packet-integration-r1/binding.h"

namespace gemv_typed_test {
using namespace native_sequence;
namespace typed=canonical_gemv::typed;
struct Spy final:g::L2DramAddressMapper {
    mutable std::vector<g::CacheLineKey> lines;
    U map(const g::CacheLineKey& key)const override{lines.push_back(key);return key.line_addr;}
};
U negatives=0;
template<class F> void rejects(F operation) {
    bool failed=false;try{operation();}catch(const std::exception&){failed=true;}
    p::need(failed,"typed GEMV negative case accepted");++negatives;
}
void compare(const g::ExplicitMemorySubop& a,const g::ExplicitMemorySubop& b) {
    p::need(a.requested_bytes==b.requested_bytes&&a.source_member_ordinals==b.source_member_ordinals&&
            a.ranges.size()==b.ranges.size(),"typed GEMV source shape/provenance mismatch");
    for(U i=0;i<a.ranges.size();++i)p::need(a.ranges[i].source_member_ordinal==b.ranges[i].source_member_ordinal&&
        a.ranges[i].offset_bytes==b.ranges[i].offset_bytes&&a.ranges[i].byte_count==b.ranges[i].byte_count,
        "typed GEMV ordered address/width/lane mismatch");
}
std::string unhex(const std::string& text) {
    p::need(text.size()==304,"test real GEMV152B");std::string result;
    for(U i=0;i<text.size();i+=2)result.push_back(char(std::stoul(text.substr(i,2),nullptr,16)));
    return result;
}
typed::Parameters from_capture(const J& raw,const J& launch,const std::string& receipt_sha) {
    typed::Parameters result;const auto& bind=raw.at("native_launch_binding");
    result.capture={p::natural(bind.at("process").at("pid")),p::natural(bind.at("process").at("start_ticks")),
        p::natural(bind.at("native_launch_id")),raw.at("source_launch_key"),raw.at("phase"),
        "",receipt_sha};
    p::need(raw.at("arguments").size()==1,"test single actual opaque parameter");
    const auto& arg=raw.at("arguments").at(0);const auto bytes=unhex(arg.at("raw_bytes_hex"));
    p::need(tiny_sha::sha256(bytes)==arg.at("sha256").get<std::string>(),"test raw152 captured SHA");
    for(U i=0;i<38;++i)for(U byte=0;byte<4;++byte)
        result.words[i]|=std::uint32_t(static_cast<unsigned char>(bytes[4*i+byte]))<<(8*byte);
    result.code_sha256=raw.at("code_sha256");result.parameter_layout_sha256=raw.at("parameter_layout_sha256");
    for(U i=0;i<3;++i){result.grid[i]=p::natural(launch.at("grid").at(i));result.block[i]=p::natural(launch.at("block").at(i));}
    result.static_shared_bytes=p::natural(launch.at("static_shared_bytes"));
    result.dynamic_shared_bytes=p::natural(launch.at("dynamic_shared_bytes"));
    result.registers=p::natural(launch.at("registers"));result.local_bytes_per_thread=p::natural(launch.at("local_bytes_per_thread"));
    return result;
}
J old_input(const J& plan) {
    const auto fixed=canonical_gemv::seals();J selected=J::array();std::set<std::string> templates;
    std::vector<std::pair<U,U>> spans;
    for(const auto& call:plan.at("calls"))if(templates.insert(call.at("template_key")).second) {
        selected.push_back(call.at("source_launch_key"));
        for(auto role:{"weight","input","output"}){
            const auto& o=call.at("objects").at(role);U lo=p::natural(o.at("pointer")),hi=p::natural(o.at("end_exclusive"));
            spans.emplace_back(lo/128*128,p::add(hi,127)/128*128);
        }
    }
    std::sort(spans.begin(),spans.end());std::vector<std::pair<U,U>> merged;
    for(auto span:spans){if(!merged.empty()&&span.first<=merged.back().second)merged.back().second=std::max(merged.back().second,span.second);else merged.push_back(span);}
    J rows=J::array();U offset=0;
    for(auto [lo,hi]:merged){rows.push_back({{"source_base",lo},{"bytes",hi-lo},{"service_base",offset}});offset=p::add(offset,hi-lo);}
    return {{"schema","CANONICAL_GEMV_MODELED_SEQUENCE_V1"},{"plan_file",fixed.at("plan_file")},
        {"memory_model",fixed.at("memory_model")},{"service_address_map",{{"schema","SG_SOURCE_TO_SERVICE_MAP_V1"},
        {"qualification","PACKED_SOURCE_GENERATED_128B_LINES_NOT_PHYSICAL_HARDWARE_ADDRESSES"},{"spans",rows}}},
        {"selected_source_keys",selected},{"prefix_ctas",0},{"max_kernel_cycles",2000000000},{"aggregate_observations",false}};
}
J run(const std::string& fixture_path) {
    const auto fixed=canonical_gemv::seals();const auto plan=strict_parse(check_pin(fixed.at("plan_file")));
    const auto fixture_raw=read_bounded(fixture_path,1<<20);
    const auto fixture_sha=tiny_sha::sha256(fixture_raw);
    p::need(fixture_sha=="867e05bd340335d33bd937d611e543ae5b2fd95fe01bf6a831b731fc7414d0cf",
            "real captured independent regression fixture pin");
    const auto fixture=strict_parse(fixture_raw);std::map<std::string,const J*> examples;
    for(const auto& example:fixture.at("examples"))if(example.at("family")=="GEMV")
        p::need(examples.emplace(example.at("launch").at("source_launch_key"),&example).second,"unique captured example");
    const auto input=old_input(plan);canonical_gemv::Model model(input);Spy spy;
    J capture_pin;
    for(const auto& pin:plan.at("input_pins"))if(pin.at("path").get<std::string>().ends_with("captured-allargs-norm-r1/controller.json"))capture_pin=pin;
    p::need(!capture_pin.is_null(),"old actual capture controller pin");(void)check_pin(capture_pin);
    coupling::ServiceMapper mapper(input.at("service_address_map"),spy,21474836480ULL);
    J rows=J::array();U lane_checks=0,record_checks=0,packet_checks=0,total_source_bytes=0;
    tiny_sha::Sha256 digest;
    for(const auto* call:model.calls) {
        const auto& example=*examples.at(call->at("source_launch_key"));
        p::need(example.at("argument_line_sha256")==call->at("argument_line_sha256"),"captured payload/old call join");
        auto args=from_capture(example.at("raw_record"),example.at("launch"),capture_pin.at("sha256"));
        args.capture.argument_payload_sha256=example.at("argument_line_sha256");
        const auto template_id=call->at("template_key").get<std::string>();
        typed::TemplateLease lease(*model.catalog,template_id);typed::Binding binding(lease,args);
        canonical_gemv::PreparedAddress typed_address(binding);
        canonical_gemv::PreparedMemory new_memory(binding,mapper),old_memory(model,*call,mapper);
        packet_binding::CallBinding old_packet(model,*call,mapper);
        const auto& source=binding.source();std::set<U> ctas{0,binding.ctas()/2,binding.ctas()-1};
        U record_count=0,bytes_read=0,bytes_write=0;
        for(U cta:ctas) {
            for(U index=0;index<source.program.body(0).records.size();++index) {
                const auto& record=source.program.body(0).records[index];
                canonical_gemv::host_address::json_reference=true;spy.lines.clear();
                const auto expected=old_memory.materialize(index,cta);const auto expected_map=spy.lines;
                canonical_gemv::host_address::json_reference=false;spy.lines.clear();
                const auto actual=new_memory.materialize(index,cta);compare(actual,expected);
                p::need(spy.lines==expected_map,"typed GEMV mapper ordering mismatch");
                for(const auto& lane:record.lanes){
                    const auto a=typed_address.address(lane,U(record.width),cta);
                    p::need(a==model.address(*call,lane,U(record.width),cta),"typed GEMV every original lane formula");
                    digest.add(std::to_string(cta)+":"+std::to_string(index)+":"+std::to_string(lane.lane)+":"+std::to_string(a)+"\n");
                    ++lane_checks;
                }
                ++record_checks;++record_count;total_source_bytes+=actual.requested_bytes;
                (record.op=='R'?bytes_read:bytes_write)+=actual.requested_bytes;
            }
            for(U member=0;member<source.registers.order.size();++member) {
                const auto& node=old_packet.node(member);if(node.kind!="global")continue;
                canonical_gemv::host_address::json_reference=true;const auto expected=old_packet.memory(cta,member);
                canonical_gemv::host_address::json_reference=false;
                const auto actual=new_memory.materialize(node.memory,cta);
                p::need(expected.logical_bytes==actual.requested_bytes&&expected.write==(node.op=='W'),"typed/old packet byte direction");
                if(expected.zero_lane_compute)p::need(actual.requested_bytes==0&&actual.ranges.empty(),"zero-lane source preserved");
                else{p::need(expected.subops.size()==1,"old packet exactly one instruction");compare(actual,expected.subops[0]);}
                ++packet_checks;
            }
        }
        p::need(bytes_read==ctas.size()*source.program.body(0).read&&
                bytes_write==ctas.size()*source.program.body(0).write,"typed GEMV traffic census");
        // Each nonpointer word is independently perturbed, including opaque
        // controls unrelated to the easily visible K/N dimensions.
        for(U word=0;word<38;++word)if(word!=0&&word!=1&&word!=4&&word!=5&&word!=8&&word!=9&&word!=12&&word!=13) {
            auto bad=args;bad.words[word]^=1;rejects([&]{typed::Binding x(lease,bad);});
        }
        for(int kind=0;kind<10;++kind){auto bad=args;
            if(kind==0)bad.code_sha256=std::string(64,'0');if(kind==1)bad.parameter_layout_sha256=std::string(64,'0');
            if(kind==2)++bad.grid[0];if(kind==3)++bad.registers;if(kind==4)bad.words[12]^=128;
            if(kind==5)bad.words[0]^=1;if(kind==6){bad.words[0]=0;bad.words[1]=0;}
            if(kind==7){bad.words[0]=0xffffff80;bad.words[1]=UINT32_MAX;}
            if(kind==8)bad.capture.argument_payload_sha256.clear();if(kind==9)bad.capture.phase.clear();
            rejects([&]{typed::Binding x(lease,bad);});
        }
        rejects([&]{new_memory.materialize(0,binding.ctas());});
        rejects([&]{new_memory.materialize(source.program.body(0).records.size(),0);});
        canonical_gemv::host_address::json_reference=true;
        rejects([&]{new_memory.materialize(0,0);});canonical_gemv::host_address::json_reference=false;
        auto bad_old=*call;bad_old["process"]["pid"]=1;rejects([&]{model.validate_call(bad_old);});
        // New phase/provenance can be recorded only on the separate typed
        // domain. This does not insert it into Model or validate its dynamics.
        auto future=args;future.capture.phase="Decode32";future.capture.pid=42;
        typed::Binding future_binding(lease,future);const auto ev=future_binding.evidence();
        p::need(ev.at("host_capture").at("phase")=="Decode32"&&ev.at("host_capture").at("pid")==42&&
                ev.at("native_model_admitted")==false&&ev.at("dynamic_memory_validated")==false&&
                ev.at("host_capture_files_verified_by_this_constructor")==false,"no qualification/identity laundering");
        rows.push_back({{"template",template_id},{"source_launch_key",call->at("source_launch_key")},
            {"CTAs",ctas},{"full_grid_CTAs",binding.ctas()},{"memory_records_compared",record_count},
            {"source_read_bytes",bytes_read},{"source_write_bytes",bytes_write},{"evidence",binding.evidence()}});
        std::cerr<<J({{"template",template_id},{"status","PASS_ALL_LANES_AND_RANGES_FIRST_MIDDLE_LAST_CTA"}}).dump()<<'\n';
    }
    p::need(rows.size()==11,"all eleven sealed GEMV templates");
    rejects([&]{typed::TemplateLease bad(*model.catalog,"unknown-template");});
    auto bad_input=input;bad_input["plan_file"]["sha256"]=std::string(64,'0');
    rejects([&]{canonical_gemv::Model bad(bad_input);});model.catalog->seals.finish();
    return {{"schema","GEMV_TYPED_ADDRESS_CONSTRUCTOR_TEST_V1"},
        {"status","PASS_11_SEALED_TEMPLATES_TYPED_ADDRESSES_MATCH_OLD_BINDING"},
        {"fixture_sha256",fixture_sha},{"templates",rows},{"lane_formula_checks",lane_checks},
        {"memory_records_compared",record_checks},{"old_packet_instructions_compared",packet_checks},
        {"source_bytes_compared",total_source_bytes},{"ordered_lane_addresses_sha256",digest.hex()},
        {"negative_cases",negatives},{"tested_CTA_scope","first, middle, last of each full sealed template grid; every record/lane"},
        {"new_capture_qualified",false},{"GPU_executed",false},{"HBFSIM_executed",false}};
}
}
int main(int argc,char** argv){try{if(argc!=2)throw std::runtime_error("expected frozen regression fixture path");
    std::cout<<gemv_typed_test::run(argv[1]).dump(2)<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
