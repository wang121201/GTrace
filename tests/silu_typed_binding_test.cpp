#include "native_sequence_source_pool.inc"
#include "work/tilegen-full-r1/canonical-silu-driver-r1/typed_binding.h"
#include <functional>
using namespace native_sequence;
using canonical_silu::TypedCall;

static U negative_checks=0;
template<class F>void rejects(F operation){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}p::need(rejected,"typed SiLU negative accepted");++negative_checks;}
static void word(tiny_sha::Sha256& hash,U n){char bytes[8];for(int i=0;i<8;++i)bytes[i]=char(n>>(8*i));hash.add(bytes,8);}

TypedCall typed(const J& call,const J& evidence_pin){
    TypedCall result;result.pid=p::natural(call.at("process").at("pid"));result.start_ticks=p::natural(call.at("process").at("start_ticks"));
    result.context_id=p::natural(call.at("context_id"));result.stream=p::natural(call.at("stream_u64"));
    result.native_launch_id=p::natural(call.at("native_launch_binding").at("native_launch_id"));
    result.source_launch_key=call.at("source_launch_key");result.phase=call.at("phase");
    const auto& key=result.source_launch_key;const auto split=key.find("-launch-");
    result.epoch=std::stoull(key.substr(6,split-6));result.ordinal=std::stoull(key.substr(split+8));
    result.code_sha256=call.at("code_sha256");result.parameter_layout_sha256=call.at("parameter_layout_sha256");
    result.argument_record_sha256=call.at("argument_line_sha256");result.capture_receipt_sha256=evidence_pin.at("sha256");
    for(int i=0;i<3;++i){result.grid[i]=p::natural(call.at("grid")[i]);result.block[i]=p::natural(call.at("block")[i]);}
    const auto&r=call.at("native_resources");result.registers=p::natural(r.at("registers"));result.local_bytes=p::natural(r.at("local_bytes_per_thread"));
    result.static_shared=p::natural(r.at("static_shared_bytes"));result.dynamic_shared=p::natural(r.at("dynamic_shared_bytes"));
    for(U i=0;i<3;++i){const auto&a=call.at("typed_arguments")[i];p::need(p::natural(a.at("index"))==i,"real typed argument index");
        U value=p::natural(a.at("value")),size=p::natural(a.at("size_bytes"));for(U n=0;n<size;++n)result.parameters[i].bytes+=char(value>>(8*n));
        result.parameters[i].sha256=a.at("raw_bytes_sha256");p::need(tiny_sha::sha256(result.parameters[i].bytes)==result.parameters[i].sha256,"real captured bytes reconstructed and SHA checked");}
    auto object=[&](const char* role){const auto&o=call.at("objects").at(role);canonical_silu::TypedObjectView v;
        v.pid=result.pid;v.start_ticks=result.start_ticks;v.pointer=p::natural(o.at("pointer"));v.bytes=p::natural(o.at("bytes"));
        v.root_base=p::natural(o.at("root").at("base_address"));v.root_bytes=p::natural(o.at("root").at("storage_nbytes"));
        v.storage_offset=p::natural(o.at("storage_offset_bytes"));v.root_id=o.at("root").at("id");v.evidence_sha256=evidence_pin.at("sha256");return v;};
    result.input=object("input");result.out=object("out");return result;
}

int main(int argc,char**argv){try{
    p::need(argc==2,"one independently generated legacy envelope");
    canonical_silu::Model model(strict_parse(read_bounded(argv[1],2U<<20)));
    canonical_silu::TypedSourceLease lease(model.source(),canonical_silu::seals().at("template_file"));
    U calls=0,ctas=0,records=0,lanes=0,typed_calls=0,typed_records=0;J phases=J::object();tiny_sha::Sha256 legacy_hash,typed_hash;
    TypedCall example;bool have_example=false;
    for(const J*cp:model.calls){const J&call=*cp;canonical_silu::PreparedMemory legacy(model,call);++calls;
        const auto phase=call.at("phase").get<std::string>();phases[phase]=phases.value(phase,U(0))+1;
        auto candidate=typed(call,canonical_silu::seals().at("plan_file"));
        std::unique_ptr<canonical_silu::TypedBinding>binding;
        if(phase=="Prefill")rejects([&]{canonical_silu::TypedBinding bad(lease,candidate);});
        else {binding=std::make_unique<canonical_silu::TypedBinding>(lease,candidate);++typed_calls;example=candidate;have_example=true;
            p::need(binding->evidence().at("native_model_admitted")==false&&binding->evidence().at("dynamic_memory_witness")==false,"candidate qualification preserved");}
        for(U cta=0;cta<legacy.ctas();++cta){++ctas;
            for(U record=0;record<744;++record){const auto&formula=model.records.at(record);const auto a=legacy.materialize(cta,record);
                p::need(a.ranges.size()==1&&a.source_member_ordinals==std::vector<int>{-1}&&a.ranges[0].source_member_ordinal==-1&&
                    a.requested_bytes==32*formula.width&&a.ranges[0].byte_count==a.requested_bytes&&legacy.direction(record)==formula.op,
                    "old merged range width/direction/provenance");
                word(legacy_hash,cta);word(legacy_hash,record);word(legacy_hash,U(formula.op));word(legacy_hash,formula.warp);word(legacy_hash,formula.ordinal);
                for(U lane=0;lane<32;++lane){const U address=model.address(call,cta,formula,lane);
                    p::need(address==p::add(a.ranges[0].offset_bytes,p::multiply(lane,formula.width)),"all legacy lanes equal unchanged Model oracle");word(legacy_hash,address);++lanes;}
                ++records;
                if(binding){const auto b=binding->memory().materialize(cta,record);
                    p::need(b.requested_bytes==a.requested_bytes&&b.source_member_ordinals==a.source_member_ordinals&&b.ranges.size()==a.ranges.size()&&
                        b.ranges[0].source_member_ordinal==a.ranges[0].source_member_ordinal&&b.ranges[0].offset_bytes==a.ranges[0].offset_bytes&&
                        b.ranges[0].byte_count==a.ranges[0].byte_count&&binding->memory().direction(record)==formula.op,"typed/legacy full ordered record equality");
                    word(typed_hash,record);word(typed_hash,U(formula.op));word(typed_hash,b.ranges[0].offset_bytes);word(typed_hash,b.requested_bytes);++typed_records;}
            }
        }
        rejects([&]{legacy.materialize(legacy.ctas(),0);});rejects([&]{legacy.materialize(0,744);});
    }
    p::need(calls==96&&ctas==1088&&typed_calls==64&&have_example,"real 96-call closed corpus and Decode-only typed count");
    auto bad=[&](const std::function<void(TypedCall&)>&mutate){auto c=example;mutate(c);rejects([&]{canonical_silu::TypedBinding b(lease,c);});};
    bad([](TypedCall&c){c.pid=0;});bad([](TypedCall&c){++c.input.pid;});bad([](TypedCall&c){++c.out.start_ticks;});
    bad([](TypedCall&c){c.phase="Decode32";});bad([](TypedCall&c){c.source_launch_key="epoch-2-launch-0";});bad([](TypedCall&c){c.stream=1;});
    bad([](TypedCall&c){c.grid[0]=32;});bad([](TypedCall&c){c.grid[0]=1024;});bad([](TypedCall&c){c.block[0]=512;});
    bad([](TypedCall&c){++c.registers;});bad([](TypedCall&c){c.dynamic_shared=16;});bad([](TypedCall&c){c.local_bytes=8;});
    bad([](TypedCall&c){c.code_sha256[0]='0';});bad([](TypedCall&c){c.parameter_layout_sha256.clear();});
    bad([](TypedCall&c){c.argument_record_sha256.clear();});bad([](TypedCall&c){c.capture_receipt_sha256.clear();});
    bad([](TypedCall&c){c.parameters[0].bytes.pop_back();});bad([](TypedCall&c){c.parameters[1].sha256.clear();});
    bad([](TypedCall&c){c.parameters[2].bytes[0]=char(1);c.parameters[2].sha256=tiny_sha::sha256(c.parameters[2].bytes);});
    bad([](TypedCall&c){--c.input.bytes;});bad([](TypedCall&c){--c.out.root_bytes;});bad([](TypedCall&c){++c.input.storage_offset;});
    bad([](TypedCall&c){c.input.root_base=UINT64_MAX;c.input.storage_offset=1;});bad([](TypedCall&c){c.out.evidence_sha256.clear();});
    bad([](TypedCall&c){c.parameters[0]=c.parameters[1];c.out=c.input;c.out.bytes=28672;});
    auto changed_pin=canonical_silu::seals().at("template_file");changed_pin["sha256"]=std::string(64,'0');
    rejects([&]{canonical_silu::TypedSourceLease other(model.source(),changed_pin);});
    const auto&first=*model.calls.front();for(U count:{U(0),U(2),U(1024)}){J c=first;c["grid"][0]=count;rejects([&]{canonical_silu::PreparedMemory b(model,c);});}
    canonical_silu::TypedBinding one(lease,example);rejects([&]{one.memory().materialize(1,0);});rejects([&]{one.memory().direction(744);});
    // A new phase identity is preserved explicitly, never renamed to Decode2.
    auto d32=example;d32.epoch=33;d32.phase="Decode32";d32.source_launch_key="epoch-33-launch-"+std::to_string(d32.ordinal);
    canonical_silu::TypedBinding last(lease,d32);p::need(last.evidence().at("actual_capture").at("phase")=="Decode32","real phase preserved as supplied candidate identity");
    lease.finish();model.catalog->seals.finish();
    std::cout<<J({{"status","PASS_SILU_TYPED_MEMORY_CANDIDATE"},{"legacy_calls",calls},{"legacy_phase_counts",phases},{"legacy_CTAs",ctas},
        {"legacy_memory_records",records},{"legacy_lane_oracle_checks",lanes},{"typed_decode_calls",typed_calls},{"typed_memory_records",typed_records},
        {"typed_lane_addresses_equal",typed_records*32},{"negative_checks",negative_checks},{"legacy_ordered_lane_sha256",legacy_hash.hex()},
        {"typed_ordered_records_sha256",typed_hash.hex()},{"new_phase_test_is_synthetic_identity_only",true},{"native_model_admitted",false},
        {"dynamic_memory_witness",false},{"GPU_executed",false},{"HBFSIM_executed",false}}).dump()<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
