#include "replay_backend.h"
#include "hbf_replay_backend.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace backend_test {
using U=std::uint64_t;using J=nlohmann::json;namespace g=GTSim;
namespace hbm=hbfsim::physical::hbm;
static U checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"expected backend rejection");}
g::L2DramRequest request(U id,U address,bool write=false){
    g::L2DramRequest r;r.request_id=r.source_sequence=id;r.address=address;r.bytes=write?32:128;
    r.key={1,0x100000000ULL+address/128*128};r.node_id=id+10;r.sm_id=id%2;r.l2_subpartition_id=id%4;
    r.cause=write?g::L2DramRequestCause::DIRTY_WRITEBACK:g::L2DramRequestCause::FILL_READ;return r;
}
std::array<U,8> identity(const g::L2DramCompletion& c){return {c.request_id,c.source_sequence,c.issue_cycle,c.issue_time_ps,
    c.completion_cycle,U(c.key.matrix_id),c.key.line_addr,U(c.is_writeback)};}
hbm::HbmConfig hbm_config(){
    hbm::HbmConfig c;c.device.capacity_bytes=128ULL<<20;c.device.stacks=1;
    c.device.channels_per_stack=2;c.device.pseudo_channels_per_channel=1;
    c.device.bank_groups_per_pseudo_channel=2;c.device.banks_per_group=2;
    c.device.channel_row_size_bytes=1024;c.device.channel_width_bits=32;c.device.burst_length=8;
    c.controller.interleave_bytes=256;c.controller.queue_depth=4;c.controller.refresh_enabled=false;
    c.controller.same_bank_refresh=false;c.controller.replicate_symmetric_pseudo_channels=false;return c;
}
void hbm_regression(sg_hbf::DrainMode drain){
    const sg_hbf::Clock clock(500,1);const auto cfg=hbm_config();
    sg_hbf::HbfCreditBackend legacy(clock,cfg,2,4,1,drain);
    replay_backend::HbmBackend adapter(clock,cfg,2,4,drain);
    const std::vector<g::L2DramRequest> rows={request(0,0),request(1,1024),request(2,256),
        request(3,0),request(4,4096,true),request(5,4128,true),request(6,8192),request(7,256)};
    U next=0,cycle=0,completed=0;
    for(;;){
        while(next<rows.size()){
            const bool old=legacy.try_enqueue(rows[next],cycle),now=adapter.try_enqueue(rows[next],cycle);
            check(old==now,"HBM wrapper admission changed");if(!old)break;++next;
        }
        const auto expected=legacy.step(cycle),actual=adapter.step(cycle);
        check(expected.size()==actual.size(),"HBM wrapper delivery count changed");
        for(U i=0;i<actual.size();++i){check(identity(expected[i])==identity(actual[i]),"HBM wrapper completion/order/time changed");++completed;}
        check(adapter.queue_depth()==legacy.queue_depth(),"HBM wrapper live depth changed");
        check(adapter.next_completion_cycle()==legacy.next_completion_cycle(),"HBM wrapper due cycle changed");
        const auto& ad=legacy.admission_statistics();
        check(adapter.all_live_schedules_known()==(ad.accepted==ad.native_completions_posted),"HBM wrapper known-schedule guard changed");
        if(next==rows.size()&&!adapter.queue_depth())break;
        check(++cycle<100000,"HBM regression cycle budget");
    }
    check(completed==rows.size(),"HBM regression lost request");check(adapter.drain_cycle(cycle)==cycle,"HBM gained persistence tail");
    adapter.finalize();legacy.finalize();const auto s=adapter.snapshot();const auto& a=legacy.admission_statistics();
#define AD(x) check(s.admission.x==a.x,"HBM admission " #x)
    AD(accepted);AD(completed);AD(blocked);AD(native_completions_posted);AD(actual_request_shape_fnv1a64);AD(peak_live);AD(last_completion_ps);
#undef AD
    const auto& t=legacy.statistics();
#define TR(x) check(s.traffic.x==t.x,"HBM logical traffic " #x)
    TR(fill_bytes);TR(writeback_bytes);TR(fill_completed_bytes);TR(writeback_completed_bytes);TR(fill_requests);
    TR(fill_completions);TR(writeback_requests);TR(writeback_completions);TR(peak_queue_depth);
#undef TR
    const auto p=legacy.physical_statistics();
#define PH(x) check(s.physical.at(#x)==p.x,"HBM native physical " #x)
    PH(read_bytes);PH(write_bytes);PH(row_hits);PH(row_misses);PH(row_conflicts);PH(activations);PH(precharges);
    PH(bus_busy_ns);PH(finish_ns);PH(first_arrival_ns);PH(controller_buffer_read_bytes);PH(controller_buffer_write_bytes);
#undef PH
    check(s.source_bytes_equal_physical&&s.physical_read_bytes==p.read_bytes&&s.physical_write_bytes==p.write_bytes,"HBM physical/source identity changed");
    check(s.admission.blocked>0&&s.admission.peak_live==2,"HBM finite-credit fixture did not exercise backpressure");
}
hbfsim::app::SystemConfig hbf_config(){
    hbfsim::app::SystemConfig c;c.hbm=hbm_config();auto& d=c.hbf.device;
    d.stacks=d.channels_per_stack=d.dies_per_channel=d.planes_per_die=1;
    d.blocks_per_plane=32;d.pages_per_block=16;d.page_size_bytes=4096;
    auto& h=c.hbf.host;h.logical_capacity_bytes=16*4096;h.auto_gc_enabled=false;
    h.mapping_mode=hbfsim::host::MappingMode::FullResident;h.write_coalescing_enabled=true;
    h.write_buffer_pages=4;h.write_buffer_flush_threshold_pages=0;h.write_buffer_completion_requires_flush=false;return c;
}
U deliver_next(replay_backend::Backend& backend,U cycle,U& completed){
    check(backend.all_live_schedules_known(),"HBF admitted schedule should be known");
    const U due=backend.next_completion_cycle();check(due!=std::numeric_limits<U>::max()&&due>cycle,"HBF invalid future due cycle");
    check(backend.step(due-1).empty(),"HBF completed before its due cycle");
    const auto rows=backend.step(due);check(!rows.empty(),"HBF due completion missing");
    for(const auto& c:rows){check(c.completion_cycle==due&&c.issue_cycle==0&&c.issue_time_ps==0,"HBF completion identity/timing changed");++completed;}
    return due;
}
void hbf_seeded(){
    const sg_hbf::Clock clock(1000,1);const auto cfg=hbf_config();
    hbf_replay_backend::Backend backend(clock,cfg,{{0,8}},2);
    check(backend.try_enqueue(request(0,0),0),"HBF seeded read rejected");
    check(backend.try_enqueue(request(1,4096+32,true),0),"HBF seeded partial write rejected");
    check(!backend.try_enqueue(request(2,8192),0),"HBF parent credit failed to block");
    check(backend.step(0).empty(),"HBF future completion delivered at admission");
    U completed=0,cycle=deliver_next(backend,0,completed);
    check(backend.try_enqueue(request(2,8192),cycle),"HBF parent credit did not reopen");
    while(backend.queue_depth())cycle=deliver_next(backend,cycle,completed);
    check(completed==3,"HBF source request completion count differs");
    const auto before=backend.snapshot();
    check(before.traffic.fill_bytes==256&&before.traffic.writeback_bytes==32&&before.admission.accepted==3&&before.admission.completed==3,"HBF source byte/request ledger differs");
    check(before.traffic.fill_completed_bytes==256&&before.traffic.writeback_completed_bytes==32,"HBF completed source bytes differ");
    check(!before.source_bytes_equal_physical&&before.physical_read_bytes>256,"HBF NAND page reads confused with 128B logical reads");
    const U durable=backend.drain_cycle(cycle);check(durable>cycle,"HBF dirty buffered page lacks EOF persistence tail");
    backend.finalize();const auto after=backend.snapshot();
    check(after.admission.accepted==3&&after.admission.completed==3&&after.admission.native_completions_posted==3,"HBF maintenance created source parents");
    check(after.traffic.fill_bytes==256&&after.traffic.writeback_bytes==32,"HBF maintenance changed source traffic");
    check(after.physical_read_bytes>before.physical_read_bytes&&after.physical_write_bytes>before.physical_write_bytes,"HBF EOF partial-page read/modify/write missing");
    check(after.physical_write_bytes>=4096&&after.finish_ns<=double(durable),"HBF NAND drain completion outside durable time");
    check(after.physical.at("logical_read_bytes")==256&&after.physical.at("logical_write_bytes")==32,"HBF native logical counters differ");
    check(after.physical.at("data_program_payload_bytes")==4096&&after.physical.at("write_buffer_flushes")==1,"HBF 32B write was not one 4096B data-page flush");
    check(after.physical.at("host_hbm_read_bytes").get<U>()>0&&after.physical.at("host_hbm_write_bytes").get<U>()>0,"HBF shared controller HBM traffic missing");
    check(after.diagnostics.at("HBF_final_drain_begin_cycle")==cycle&&after.diagnostics.at("HBF_final_drain_end_cycle")==durable&&
        after.diagnostics.at("HBF_final_drain_physical_bytes").get<U>()>0,"HBF EOF maintenance was not separately accounted");
    check(after.physical.at("final_quiescent")==true&&after.physical.at("initial_logical_data_pages")==8,"HBF seeded image/final quiescence differs");
    check(after.admission.blocked>0&&after.admission.peak_live==2,"HBF bounded source parent fixture did not exercise backpressure");
}
void hbf_empty_and_invalid(){
    const sg_hbf::Clock clock(1000,1);const auto cfg=hbf_config();
    hbf_replay_backend::Backend empty(clock,cfg,{{0,8}},2);
    check(empty.step(0).empty()&&empty.queue_depth()==0&&empty.drain_cycle(0)==0,"HBF empty run generated work");
    empty.finalize();const auto s=empty.snapshot();
    check(s.admission.accepted==0&&s.traffic.fill_bytes==0&&s.traffic.writeback_bytes==0&&s.physical_read_bytes==0&&s.physical_write_bytes==0,"HBF seed charged synthetic traffic");
    rejects([&]{hbf_replay_backend::Backend b(clock,cfg,{},2);b.try_enqueue(request(0,0),0);});
    rejects([&]{hbf_replay_backend::Backend b(clock,cfg,{{0,1}},2);b.try_enqueue(request(0,4096),0);});
    rejects([&]{hbf_replay_backend::Backend b(clock,cfg,{{0,1}},2);b.try_enqueue(request(0,4096,true),0);});
    rejects([&]{hbf_replay_backend::Backend b(clock,cfg,{{0,2},{1,1}},2);});
    rejects([&]{hbf_replay_backend::Backend b(clock,cfg,{{0,8}},0);});
    rejects([&]{hbf_replay_backend::Backend b(clock,cfg,{{0,8}},2);auto r=request(0,0);r.bytes=32;b.try_enqueue(r,0);});
    rejects([&]{hbf_replay_backend::Backend b(clock,cfg,{{0,8}},2);b.try_enqueue(request(0,1),0);});
}
} // namespace backend_test
int main(){try{using namespace backend_test;hbm_regression(sg_hbf::DrainMode::Global);hbm_regression(sg_hbf::DrainMode::Independent);
    hbf_seeded();hbf_empty_and_invalid();std::cout<<J({{"status","PASS"},{"checks",checks}}).dump()<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
