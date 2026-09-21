#include "memory.h"
#include <iostream>
#include <set>
#include <string>

namespace g=GTSim;
using U=std::uint64_t;
static U checks=0;
void check(bool ok,const char* message) { ++checks; if(!ok)throw std::runtime_error(message); }

struct Backend final:g::L2DramCompletionBackend {
    struct Pending { g::L2DramRequest request; U due; };
    std::vector<Pending> live;
    // Tiny test fixture only, bounded to64 parents. No native/full trace input.
    std::vector<g::L2DramRequest> accepted;
    g::L2DramRuntimeStatistics stats;
    U capacity,latency,blocked=0,completion_gate=0,max_completion_batch=0;
    explicit Backend(U cap=8,U delay=7):capacity(cap),latency(delay){}
    U issue_cycle_to_ps(U cycle)const override{return cycle;}
    std::size_t admission_capacity()const override{return capacity;}
    void enqueue(const g::L2DramRequest& r)override {check(try_enqueue(r,r.issue_cycle),"test nonretry enqueue");}
    bool try_enqueue(const g::L2DramRequest& r,U cycle)override {
        check(r.request_id==accepted.size()&&r.source_sequence==r.request_id,"FIFO identity");
        if(live.size()==capacity){++blocked;return false;}
        check(accepted.size()<64,"bounded test record set");
        accepted.push_back(r);live.push_back({r,std::max(cycle+latency,completion_gate)});
        if(r.cause==g::L2DramRequestCause::DIRTY_WRITEBACK){++stats.writeback_requests;stats.writeback_bytes+=r.bytes;}
        else{++stats.fill_requests;stats.fill_bytes+=r.bytes;check(r.bytes==128&&r.address%128==0,"read fill remains128B");}
        stats.peak_queue_depth=std::max<U>(stats.peak_queue_depth,live.size());return true;
    }
    std::vector<g::L2DramCompletion> step(U cycle)override {
        std::vector<g::L2DramCompletion> done;std::vector<Pending> next;
        for(const auto& p:live){
            if(p.due>cycle){next.push_back(p);continue;}
            const auto&r=p.request;bool write=r.cause==g::L2DramRequestCause::DIRTY_WRITEBACK;
            if(write){++stats.writeback_completions;stats.writeback_completed_bytes+=r.bytes;}
            else{++stats.fill_completions;stats.fill_completed_bytes+=r.bytes;}
            done.push_back({r.request_id,r.source_sequence,r.issue_cycle,r.issue_time_ps,cycle,r.key,write});
        }
        max_completion_batch=std::max<U>(max_completion_batch,done.size());
        live=std::move(next);return done;
    }
    const g::L2DramRuntimeStatistics& statistics()const override{return stats;}
    std::size_t queue_depth()const override{return live.size();}
};
struct Mapper:g::L2DramAddressMapper {
    U base=4096;
    U map(const g::CacheLineKey& key)const override{return base+key.line_addr;}
};
struct Rig {
    Backend backend;Mapper mapper;g::L2Cache cache;g::Cycle cycle=0;int next_id=0;unsigned epoch_span=0;
    static g::MemoryModelSemantics semantics(unsigned span){
        g::MemoryModelSemantics s;
        if(span){s.l2_service=g::L2ServiceSemantics::EXACT_RATIONAL_BYTES_PER_CORE_CYCLE;
            s.l2_total_rate={128,1};s.l2_write_rate={128,1};}
        return s;
    }
    Rig(int lines=1,U cap=8,U latency=7,unsigned span=0,
        const g::L2GeometryConfig& geometry=g::L2GeometryConfig()):backend(cap,latency),
        cache(lines*128,128,1,128,128,32,false,128,7,1000,1000,
              semantics(span),g::PerSmL1Config(),&backend,&mapper,geometry),epoch_span(span){}
    g::DAGNode node(bool write,const std::vector<std::pair<U,U>>& spans){
        g::DAGNode n(next_id++,"direct","LS",write?"st.reg2dram":"ld.dram2reg",0,{},0,{});
        n.sm_id=0;n.matrix_id=3;
        g::ExplicitMemorySubop sub;
        for(auto [a,b]:spans){sub.ranges.push_back({-1,a,b});sub.requested_bytes+=b;}
        n.explicit_memory_subops.push_back(std::move(sub));return n;
    }
    void tick(){
        if(epoch_span){cycle+=epoch_span;auto completed=backend.step(cycle);
            cache.service_epoch(cycle,epoch_span,backend,completed);}
        else cache.step(cycle++);
        check(cycle<10000,"bounded test cycles");
        if constexpr(g::kTilegenDirtySectorMode!=0){auto s=cache.dirty_sector_snapshot();
            check(s.dirty_sector_ledger_closed,"dirty-sector conservation on every modeled tick");
            check(s.writeback_byte_ledger_closed,"outstanding/unadmitted/completed byte closure on every tick");}}
    void submit(g::DAGNode& n,bool write,U line,int subop=0){
        while(!cache.enqueue_transaction_key(n,{n.matrix_id,line},write,cycle,0,true,nullptr,nullptr,subop))tick();
        tick();
    }
    void access(bool write,const std::vector<std::pair<U,U>>& spans,U line){auto n=node(write,spans);submit(n,write,line);}
    void drain(){while(!cache.is_quiescent())tick();
        check(cache.pending_dram_admission_count()==0,"pending closed");
        auto s=cache.runtime_statistics();
        check(s.dram_fill_bytes==s.dram_fill_completed_bytes&&s.dram_writeback_bytes==s.dram_writeback_completed_bytes,"actual byte completion closure");
        check(s.accepted_transactions==s.processed_transactions,"L2 transaction closure");}
    std::vector<g::L2DramRequest> writes()const{std::vector<g::L2DramRequest> out;for(auto&r:backend.accepted)if(r.cause==g::L2DramRequestCause::DIRTY_WRITEBACK)out.push_back(r);return out;}
    void emit(const std::string& label)const {
        auto s=cache.runtime_statistics();
        std::cout<<label<<' '<<cycle<<' '<<s.dram_fill_bytes<<' '<<s.dram_writeback_bytes<<' '
            <<s.dram_fill_requests<<' '<<s.dram_writeback_requests<<' '<<s.decision_order_fnv1a64<<' '<<s.fill_order_fnv1a64<<'\n';
    }
};

void coverage_tests(){
    for(unsigned mask=1;mask<16;++mask){
        Rig r;std::vector<std::pair<U,U>> spans;
        std::set<U> expected_sectors;
        for(unsigned s=0;s<4;++s)if(mask&(1U<<s)){spans.push_back({32*s+5,1});expected_sectors.insert(4096+32*s);}
        // Repeating a source byte preserves requested-byte multiplicity but
        // cannot invent an extra dirty sector.
        spans.push_back(spans.front());r.access(true,spans,0);r.drain();
        r.access(false,{{128,4}},128);r.drain();auto wb=r.writes();
        std::set<U> seen;
        for(const auto& p:wb){
            check(p.key.line_addr==0,"WB completion key is parent128B line");
            check(p.address%32==0&&p.bytes%32==0&&p.bytes<=128,"WB aligned legal run");
            for(U a=p.address;a<p.address+p.bytes;a+=32)check(seen.insert(a).second,"no duplicated writeback sector");
        }
        if constexpr(g::kTilegenDirtySectorMode==2) {
            check(seen==expected_sectors,"exact evicted dirty-sector coverage");
            check(wb.size()==expected_sectors.size(),"one parent for each dirty sector");
            for (const auto& p:wb) check(p.bytes==32,"every writeback request exactly32B");
            if(mask==15)check(wb.size()==4,"full mask emits four32B requests");
            const auto& stats=r.cache.dirty_sector_statistics();
            check(stats.eviction_run_counts.at(expected_sectors.size())==1,"writeback-count histogram includes indices1..4");
            check(stats.writeback_run_lengths.at(1)==expected_sectors.size(),"all emitted write lengths fall in32B bucket");
            for(unsigned length=2;length<=4;++length)
                check(stats.writeback_run_lengths.at(length)==0,"no64/96/128B writebacks hidden in histogram");
        }else check(wb.size()==1&&wb[0].bytes==128,"baseline and shadow retain128B");
        if constexpr(g::kTilegenDirtySectorMode!=0){auto&s=r.cache.dirty_sector_statistics();check(s.eviction_masks[mask]==1,"retained exact eviction mask");check(s.store_mask_requests==1,"one accepted mask request");}
        r.emit("mask"+std::to_string(mask));
    }
}
void rewrite_and_pending(){
    Rig r;
    r.access(false,{{0,4}},0); // outstanding read fill
    r.access(true,{{4,4}},0);r.access(true,{{70,4}},0);r.access(true,{{70,4}},0);
    r.drain();auto before=r.cache.runtime_statistics();
    check(before.write_pending_fill_merges==3,"store masks merge into read MSHR");
    r.access(true,{{7,1}},0);r.drain();check(r.cache.runtime_statistics().write_hits==1,"resident dirty rewrite");
    r.access(false,{{128,4}},128);r.drain();auto wb=r.writes();
    check(r.cache.runtime_statistics().dram_fill_bytes==256,"pending writes create no extra fill");
    check(r.cache.runtime_statistics().dram_writeback_bytes==(g::kTilegenDirtySectorMode==2?64:128),"rewrites do not multiply dirty bytes");
    if constexpr(g::kTilegenDirtySectorMode!=0)check(r.cache.dirty_sector_statistics().eviction_masks[5]==1,"MSHR/hit OR mask preserved");
    r.emit("pending_rewrite");
}
void offsets(){
    Rig r(2);auto n=r.node(true,{{120,24}});
    r.submit(n,true,0);r.submit(n,true,128);r.drain();
    r.access(false,{{256,4}},256);r.drain();r.access(false,{{384,4}},384);r.drain();auto wb=r.writes();
    check(wb.size()==2,"two crossed lines evicted");
    if constexpr(g::kTilegenDirtySectorMode==2){
        check(wb[0].address==4192&&wb[0].bytes==32,"mapped base plus upper-sector offset");
        check(wb[1].address==4224&&wb[1].bytes==32,"next line low-sector offset");
    }
    if constexpr(g::kTilegenDirtySectorMode!=0){check(r.cache.dirty_sector_statistics().eviction_masks[8]==1,"tail mask");check(r.cache.dirty_sector_statistics().eviction_masks[1]==1,"next line head mask");}
    r.emit("offset_crossline");
}
void subops(){
    Rig r;auto n=r.node(true,{{1,1}});g::ExplicitMemorySubop s;s.ranges.push_back({-1,66,1});s.requested_bytes=1;n.explicit_memory_subops.push_back(s);
    r.submit(n,true,0,0);r.submit(n,true,0,1);r.drain();r.access(false,{{128,4}},128);r.drain();
    if constexpr(g::kTilegenDirtySectorMode!=0)check(r.cache.dirty_sector_statistics().eviction_masks[5]==1,"exact selected subop mask");
    r.emit("two_subops");
}
void backpressure(){
    Rig r(1,3,20);
    for(U line: {0U,128U,256U,384U})r.access(true,{{line+2,1},{line+66,1}},line);
    r.drain();r.access(false,{{512,4}},512);r.drain();
    check(r.backend.blocked>0&&r.cache.dram_admission_rejections()>0,"forced finite-credit backpressure");
    check(r.cache.runtime_statistics().dram_writeback_bytes==(g::kTilegenDirtySectorMode==2?256:512),"all dirty obligations preserved across retries");
    if constexpr(g::kTilegenDirtySectorMode==2){
        check(r.cache.peak_pending_dram_admissions()<=16&&r.cache.pending_dram_admission_capacity()==16,"4(B+1) bound");
        check(r.writes().size()==8,"disjoint sector run parents retained across retries");
    }
    r.emit("backpressure");
    Rig full(1,1,20);
    for(U line: {0U,128U,256U,384U})full.access(true,{{line,128}},line);
    full.drain();full.access(false,{{512,4}},512);full.drain();
    check(full.backend.blocked>0,"full-mask backpressure exercised");
    if constexpr(g::kTilegenDirtySectorMode==2){
        check(full.writes().size()==16,"four independent writes per full dirty line");
        for(const auto& w:full.writes())check(w.bytes==32,"32B retained under capacity1 retries");
        check(full.cache.pending_dram_admission_capacity()==8&&full.cache.peak_pending_dram_admissions()<=8,
              "finite4(B+1) bound with capacity1");
    }else{
        check(full.writes().size()==4,"reference full-mask write remains one128B parent");
        for(const auto& w:full.writes())check(w.bytes==128,"reference128B retained under capacity1 retries");
    }
    full.emit("full_mask_backpressure");
}
void completion_burst(){
    // A backend may return all B old fills together. Each may evict a fully
    // dirty line while one later fill is already waiting for admission.
    for(unsigned span:{0U,1U,4U,8U})for(U capacity:{1U,2U,3U,4U,7U,8U}){
        Rig r(1,capacity,9,span);
        r.access(true,{{0,128}},0);r.drain();
        r.backend.max_completion_batch=0;
        r.backend.completion_gate=r.cycle+256;
        for(U i=1;i<=capacity+1;++i)r.access(true,{{i*128,128}},i*128);
        check(r.cache.pending_dram_admission_count()>0,"one later fill rejected before completion burst");
        r.drain();
        check(r.backend.max_completion_batch>=capacity,"all admitted old fills complete in one batch");
        r.access(false,{{(capacity+2)*128,4}},(capacity+2)*128);r.drain();
        const U dirty_lines=capacity+2;
        auto writes=r.writes();
        check(r.cache.runtime_statistics().dram_writeback_bytes==dirty_lines*128,"burst preserves every full dirty line byte");
        check(r.cache.runtime_statistics().dram_fill_requests==capacity+3,"burst admits each rejected fill once");
        if constexpr(g::kTilegenDirtySectorMode==2){
            check(writes.size()==4*dirty_lines,"completion batch retains four write IDs per victim");
            check(r.cache.pending_dram_admission_capacity()==4*(capacity+1),"configured bound for varying backend capacity");
            check(r.cache.peak_pending_dram_admissions()<=4*(capacity+1),"completion-burst FIFO bounded");
            check(r.cache.peak_pending_dram_admissions()>=capacity,"completion burst exercises nontrivial pending FIFO");
            std::set<U> sectors;
            for(const auto&w:writes){
                check(w.bytes==32,"completion-burst requests stay32B");
                check(sectors.insert(w.address).second,"completion-burst sector emitted once");
            }
            for(U line=0;line<dirty_lines;++line)for(U sector=0;sector<4;++sector)
                check(sectors.count(4096+128*line+32*sector)==1,"every expected victim sector survives retry");
            check(r.cache.dirty_sector_statistics().eviction_run_counts.at(4)==dirty_lines,"full-mask histogram handles batched evictions");
        }else check(writes.size()==dirty_lines,"reference burst keeps128B parent semantics");
        r.emit("completion_burst_B"+std::to_string(capacity)+"_epoch"+std::to_string(span));
    }
}
void rejects(){
    Rig r;auto n=r.node(true,{{1,1}});
    auto expect=[&](auto&&f,const char*m){bool caught=false;try{f();}catch(const std::invalid_argument&){caught=true;}check(caught,m);};
    if constexpr(g::kTilegenDirtySectorMode!=0){
        expect([&]{r.cache.enqueue_transaction_key(99,{3,0},true,0,0);},"key-only stores fail closed");
        n.explicit_memory_subops.clear();n.has_explicit_global_line_span=true;n.explicit_line_count=1;
        expect([&]{r.cache.enqueue_transaction_key(n,{3,0},true,0,0);},"line-span store coverage unknown");
        check(r.cache.runtime_statistics().accepted_transactions==0,"unsupported stores cause no L2 mutation");
        check(r.cache.dirty_sector_statistics().unknown_store_rejections==2,"unknown rejection accounting");
    }
    g::DirtySectorEvalStatistics stats;auto x=r.node(true,{{UINT64_MAX-2,4}});
    expect([&]{g::explicit_store_sector_mask(x,3,UINT64_MAX-127,0,stats);},"range overflow rejected");
    x=r.node(true,{{0,1}});x.explicit_memory_subops.front().requested_bytes=2;
    expect([&]{g::explicit_store_sector_mask(x,3,0,0,stats);},"payload mismatch rejected");
    x=r.node(true,{{UINT64_MAX-1,2}});
    check(g::explicit_store_sector_mask(x,3,UINT64_MAX-127,0,stats)==8,"inclusive final address handled without overflow");
}
void grouped_geometry(){
    // Independently construct17 addresses in PAPER slice0/set0; another slice
    // must not evict this set even though the global cache has ample space.
    auto address=[](U k){U index=128*1025*k;return ((index>>8)*20<<8)|(index&255);};
    Rig r(40*1024*1024/128,8,7,0,g::L2GeometryConfig::paper_ada_l2_v1());
    r.access(true,{{65,1}},0);r.drain();
    for(U k=1;k<16;++k){U a=address(k);r.access(false,{{a,4}},a);r.drain();}
    r.access(false,{{256,4}},256);r.drain();
    check(r.writes().empty(),"another PAPER slice cannot evict dirty set0");
    U a=address(16);r.access(false,{{a,4}},a);r.drain();
    const auto writes=r.writes();
    check(writes.size()==1&&writes[0].key.line_addr==0,"17th colliding fill evicts set-local LRU below total capacity");
    if constexpr(g::kTilegenDirtySectorMode==2)
        check(writes[0].bytes==32&&writes[0].address==4096+64,"grouped victim preserves sparse dirty32");
}
int main(){try{coverage_tests();rewrite_and_pending();offsets();subops();backpressure();completion_burst();rejects();grouped_geometry();
    std::cout<<"PASS mode="<<g::kTilegenDirtySectorMode<<" checks="<<checks<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
