#include "native_trace.h"
#include "work/tilegen-norm-shared-r1/adapter/backend.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using native_trace::U;
namespace nt=native_trace;
namespace fs=std::filesystem;
static U checks=0;
static void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}check(rejected,"expected rejection");}
static std::string hash(const std::string& data){tiny_sha::Sha256 h;h.add(data.data(),data.size());return h.hex();}
static std::string read(const std::string& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::string& path,const std::string& data){std::ofstream f(path,std::ios::binary);f.write(data.data(),data.size());check(bool(f),"fixture write");}
static nt::Record record(U id,bool native=true){
    nt::Record r;r.request_id=r.source_sequence=id;r.call_index=id/1000;
    r.source_matrix_id=7;r.source_line_address=0x100000000ULL+128*id;r.service_address=128*id;
    r.cause=id%3?nt::Cause::ReadFillOrRfo:nt::Cause::DirtyWriteback;r.bytes=id%3?128:32;
    r.node_id=id+1;r.sm_id=id%12;r.l2_subpartition_id=id%20;r.cta=id/32;r.warp=id%32;r.pc=0x800+16*id;
    if(native){r.issue_cycle=id;r.issue_ps=id*500;r.admission_cycle=id+10;r.admission_ps=(id+10)*500;}
    return r;
}
static void format_tests(const fs::path& dir){
    const std::string context=hash("closed source and config fixture");
    for(auto mode:{nt::Mode::NativeCosim,nt::Mode::FunctionalDirect}){
        const auto path=(dir/(mode==nt::Mode::NativeCosim?"native.tgn":"direct.tgn")).string();
        nt::Writer sink(path,mode,context,2<<20);
        check(!fs::exists(path)&&fs::exists(path+".partial"),"unclosed trace must stay partial");
        U reads=0,writes=0;for(U i=0;i<5000;++i){auto r=record(i,mode==nt::Mode::NativeCosim);sink.append(r);if(i%3)reads+=128;else writes+=32;}
        auto receipt=sink.finish();check(fs::exists(path)&&!fs::exists(path+".partial"),"publish closed trace");
        check(receipt.records==5000&&receipt.read_bytes==reads&&receipt.write_bytes==writes,"roundtrip byte census");
        check(receipt.file_bytes==nt::header_bytes+5000*nt::record_bytes+nt::footer_bytes,"exact format size");
        U visited=0;auto again=nt::validate(path,receipt.file_sha256,2<<20,[&](const nt::Record& r){check(nt::words(r)==nt::words(record(visited++,mode==nt::Mode::NativeCosim)),"roundtrip record differs");});
        check(visited==5000&&again.to_json()==receipt.to_json(),"roundtrip receipt differs");
        rejects([&]{sink.append(record(5000));});rejects([&]{sink.finish();});rejects([&]{nt::Writer duplicate(path,mode,context);});
        const auto original=read(path);check(hash(original)==receipt.file_sha256,"external file hash");
        rejects([&]{nt::validate(path,std::string(64,'0'));});
        for(int variant=0;variant<5;++variant){
            auto data=original;if(variant==0)data.pop_back();if(variant==1)data+='x';if(variant==2)data[0]='X';
            if(variant==3)data[nt::header_bytes+80]^=1;if(variant==4)data.back()='x';
            const auto bad=(dir/(std::string(nt::mode_name(mode))+"-bad"+std::to_string(variant))).string();write(bad,data);
            // Correct external hash still cannot bless a malformed census/footer.
            rejects([&]{nt::validate(bad,hash(data));});
        }
    }
    const auto empty=(dir/"empty.tgn").string();nt::Writer empty_writer(empty,nt::Mode::NativeCosim,context);
    check(empty_writer.finish().records==0,"empty closed trace");
    const auto limited=(dir/"limited.tgn").string();nt::Writer cap(limited,nt::Mode::NativeCosim,context,nt::header_bytes+nt::footer_bytes);
    rejects([&]{cap.append(record(0));});rejects([&]{cap.finish();});check(!fs::exists(limited),"quota failure published final");
    for(int variant=0;variant<9;++variant){
        const auto path=(dir/("invalid"+std::to_string(variant))).string();nt::Writer writer(path,nt::Mode::NativeCosim,context);auto r=record(0);
        if(variant==0)r.request_id=1;if(variant==1)r.source_sequence=1;if(variant==2)r.call_index=nt::unknown;
        if(variant==3)r.bytes=64;if(variant==4)r.service_address=1;if(variant==5)r.source_line_address=1;
        if(variant==6)r.issue_cycle=nt::unknown;if(variant==7)r.admission_cycle=0,r.issue_cycle=1;
        if(variant==8)r.cause=static_cast<nt::Cause>(2);
        rejects([&]{writer.append(r);});rejects([&]{writer.finish();});check(!fs::exists(path),"invalid record published final");
    }
    const auto direct_invalid=(dir/"invented-time.tgn").string();nt::Writer direct(direct_invalid,nt::Mode::FunctionalDirect,context);
    rejects([&]{direct.append(record(0,true));});
    const auto regression=(dir/"regression.tgn").string();nt::Writer ordered(regression,nt::Mode::NativeCosim,context);
    auto r0=record(0);r0.issue_cycle=10;r0.issue_ps=5000;ordered.append(r0);rejects([&]{ordered.append(record(1));});
    const auto target=(dir/"symlink.tgn").string();fs::create_symlink(empty,target);rejects([&]{nt::Writer link(target,nt::Mode::NativeCosim,context);});
    rejects([&]{nt::validate(target,hash(read(empty)));});
    // Another process winning the name race must never be overwritten.
    const auto collision=(dir/"collision.tgn").string();nt::Writer racing(collision,nt::Mode::NativeCosim,context);write(collision,"unrelated");
    rejects([&]{racing.finish();});check(read(collision)=="unrelated","published over existing file");
}
static nlohmann::json native_fixture(const fs::path& dir,bool queued,bool trace){
    sg_hbf::Clock clock(500,1);hbfsim::physical::hbm::HbmConfig cfg;
    cfg.controller.refresh_enabled=false;cfg.controller.same_bank_refresh=false;cfg.controller.replicate_symmetric_pseudo_channels=false;
    sg_hbf::PathConfig path;path.max_outer_live=4;if(queued){path.request_fixed_ps=2000;path.request_rate={true,1,500};}
    sg_hbf::MemoryPathBackend backend(clock,cfg,path,2,4,{sg_hbf::DrainMode::Independent,1});
    std::unique_ptr<nt::Writer> sink;
    if(trace){sink=std::make_unique<nt::Writer>((dir/(queued?"queued.tgn":"immediate.tgn")).string(),nt::Mode::NativeCosim,hash("backend fixture"));backend.set_trace_sink(sink.get());}
    backend.set_trace_context(9);U next=0,cycle=0;std::vector<std::array<U,6>> completions;
    while(next<64||backend.queue_depth()){
        check(cycle<100000,"native fixture failed to drain");
        while(next<64){
            GTSim::L2DramRequest r{};r.request_id=r.source_sequence=next;r.issue_cycle=r.issue_time_ps=0;
            r.key={7,0x100000000ULL+128*next};r.address=128*next;r.bytes=next%3?128:32;
            r.node_id=next+50;r.sm_id=next%12;r.l2_subpartition_id=next%20;
            r.cause=next%3?GTSim::L2DramRequestCause::FILL_READ:GTSim::L2DramRequestCause::DIRTY_WRITEBACK;
            if(!backend.try_enqueue(r,cycle))break;++next;
        }
        for(const auto& c:backend.step(cycle))completions.push_back({c.request_id,c.source_sequence,c.issue_cycle,c.issue_time_ps,c.completion_cycle,U(c.is_writeback)});
        ++cycle;
    }
    backend.finalize();backend.set_trace_context(10);
    const auto& a=backend.physical_admission_statistics();const auto& p=backend.path_statistics();auto physical=backend.physical_statistics();
    check(p.outer_rejected>0&&p.outer_accepted==64&&p.physical_admitted==64,"fixture must exercise retries and preserve census");
    if(trace){
        const auto receipt=sink->finish();check(receipt.records==64,"successful admissions only");
        check(receipt.request_payload_fnv1a64==a.actual_request_shape_fnv1a64,"trace/native admitted payload hash differs");
        U seen=0,delayed=0;nt::validate(receipt.path,receipt.file_sha256,nt::default_max_bytes,[&](const nt::Record& r){
            check(r.request_id==seen&&r.source_sequence==seen,"physical admission order not FIFO");
            check(r.call_index==9&&r.issue_cycle==0&&r.issue_ps==0,"original issue/context lost");
            check(r.source_matrix_id==7&&r.source_line_address==0x100000000ULL+128*seen&&r.service_address==128*seen,"source/service address lost");
            check(r.node_id==seen+50&&r.sm_id==seen%12&&r.l2_subpartition_id==seen%20,"source identifiers lost");
            if(r.admission_cycle)++delayed;++seen;
        });check(seen==64&&delayed>0,"retry/admission fixture was not delayed");
    }
    return {{"cycles",cycle},{"completions",completions},{"accepted",a.accepted},{"blocked",a.blocked},
        {"outer_rejected",p.outer_rejected},{"payload_hash",a.actual_request_shape_fnv1a64},
        {"binding_hash",p.inner_binding_fnv1a64},{"segments",p.delivered_segments_ps},
        {"read_bytes",physical.read_bytes},{"write_bytes",physical.write_bytes},{"row_hits",physical.row_hits},
        {"row_misses",physical.row_misses},{"row_conflicts",physical.row_conflicts},{"activations",physical.activations},
        {"precharges",physical.precharges},{"finish_ns",physical.finish_ns},{"bus_busy_ns",physical.bus_busy_ns}};
}
int main(int argc,char**argv){
    try{
        std::string pattern=(argc>1?argv[1]:"../native-trace-tests")+std::string("-XXXXXX");
        std::vector<char> chars(pattern.begin(),pattern.end());chars.push_back(0);check(::mkdtemp(chars.data())!=nullptr,"test output directory");fs::path dir(chars.data());
        format_tests(dir);nlohmann::json fixtures;
        for(bool queued:{false,true}){auto off=native_fixture(dir,queued,false);auto on=native_fixture(dir,queued,true);check(off==on,"trace changed native simulation result");fixtures[queued?"queued":"immediate"]=on;}
        std::cout<<nlohmann::json({{"status","PASS"},{"checks",checks},{"test_directory",dir.string()},{"native_trace_on_off",fixtures}}).dump(2)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
