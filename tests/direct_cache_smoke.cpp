#include "../source/direct_cache.h"
#include <iostream>

using namespace direct_native;
static g::ExplicitMemorySubop range(U address,U bytes,int lane=0) {
    g::ExplicitMemorySubop s;s.requested_bytes=bytes;s.ranges.push_back({lane,address,bytes});
    s.source_member_ordinals.push_back(lane);return s;
}
int main() {
    g::PerSmL1Config l1;l1.mode=g::PerSmL1Mode::MODELED_SET_ASSOCIATIVE;
    l1.num_sms=2;l1.capacity_bytes_per_sm=128;l1.ways=1;
    Context context;context.call_index=0;context.cta=0;context.warp=0;context.sm_id=0;
    std::vector<native_trace::Record> records;
    auto mapper=[](int matrix,U line){require(matrix==1,"fixture matrix");return line+4096;};
    auto sink=[&](const native_trace::Record& r){records.push_back(r);};
    FunctionalCache cache(l1,256,mapper,sink);cache.begin_kernel();
    auto access=[&](U addr,U bytes,bool write=false,bool bypass=false){
        cache.instruction(1,write,bypass,{range(addr,bytes)},context);
    };
    access(0,4);access(0,4);access(4,1,true);access(65,1,true);access(128,4);access(256,4);
    require(records.size()==5,"read fills plus sparse dirty32 record count");
    for(U i=0;i<records.size();++i) {
        const auto& r=records[i];require(r.request_id==i&&r.source_sequence==i,"contiguous IDs");
        require(r.issue_cycle==native_trace::unknown&&r.admission_ps==native_trace::unknown,"no synthetic time");
    }
    require(records[0].service_address==4096&&records[1].service_address==4224&&records[2].service_address==4352,
        "source to service address mapping");
    require(records[3].service_address==4096&&records[4].service_address==4160&&
        records[3].bytes==32&&records[4].bytes==32,"sparse mask only writes two dirty sectors");
    const auto s=cache.snapshot();
    require(s.at("source_read_bytes")==16&&s.at("source_write_bytes")==2,"source bytes preserved");
    require(s.at("L1_read_hits")==1&&s.at("L1_write_hits")==2&&s.at("L1_evictions")==2,"native L1 decisions");
    require(s.at("DRAM_read_bytes")==384&&s.at("DRAM_write_bytes")==64&&
        s.at("dirty_sector_creations")==2&&s.at("resident_dirty_sectors")==0,"sparse dirty conservation");

    records.clear();FunctionalCache second(l1,128,mapper,sink);second.begin_kernel();
    second.instruction(1,true,false,{range(31,2)},context); // two dirty sectors, one128B RFO
    second.instruction(1,true,false,{range(31,2)},context); // no duplicate dirty creation
    second.instruction(1,false,false,{range(0,8)},context); // L1 write-no-allocate => read miss, L2 hit
    const auto before=second.snapshot();
    require(before.at("L1_write_misses")==2&&before.at("L1_read_misses")==1&&
        before.at("DRAM_read_bytes")==128&&before.at("DRAM_write_bytes")==0,
        "write-through no-allocate and no final dirty flush");
    second.instruction(1,false,true,{range(128,4)},context);
    require(records.size()==4&&records[2].bytes==32&&records[3].bytes==32&&
        records[2].service_address==4096&&records[3].service_address==4128,
        "cross-sector store emits individual adjacent dirty32 on eviction");
    require(second.snapshot().at("dirty_sector_creations")==2,"overlap stores preserve dirty creation ledger");

    records.clear();FunctionalCache broadcast(l1,128,mapper,sink);broadcast.begin_kernel();
    auto sub=range(0,8);for(int lane=1;lane<32;++lane){sub.ranges.push_back({lane,0,8});sub.source_member_ordinals.push_back(lane);sub.requested_bytes+=8;}
    broadcast.instruction(1,false,true,{sub},context);
    require(records.size()==1&&broadcast.snapshot().at("source_read_bytes")==256,
        "broadcast source multiplicity preserved while coalescing one line");
    bool rejected=false;
    try{auto bad=sub;bad.requested_bytes=8;broadcast.instruction(1,true,false,{bad},context);}
    catch(const std::exception&){rejected=true;}
    require(rejected,"mismatched byte provenance must reject");
    cache.verify_resident_ledger();second.verify_resident_ledger();broadcast.verify_resident_ledger();
    records.clear();g::PerSmL1Config bypass;
    FunctionalCache grouped(bypass,40ULL*1024*1024,mapper,sink,g::L2GeometryConfig::paper_ada_l2_v1());
    auto collision=[](U k){U index=128*1025*k;return ((index>>8)*20<<8)|(index&255);};
    grouped.begin_kernel();grouped.instruction(1,true,false,{range(65,1)},context);
    for(U k=1;k<16;++k)grouped.instruction(1,false,false,{range(collision(k),4)},context);
    grouped.instruction(1,false,false,{range(256,4)},context);
    require(grouped.snapshot().at("DRAM_write_bytes")==0,"other PAPER group preserves dirty set0");
    grouped.instruction(1,false,false,{range(collision(16),4)},context);
    require(grouped.snapshot().at("DRAM_write_bytes")==32&&records.back().service_address==4096+64,
        "grouped Direct evicts correct sparse sector below total capacity");
    grouped.verify_resident_ledger();
    std::cout<<J({{"status","PASS_FUNCTIONAL_DIRECT_CACHE_SMOKE"},
        {"checks",{"native_L1_read_filter","native_L1_write_no_allocate","128B_fill_RFO",
                   "individual_dirty32_sparse_and_adjacent","no_final_flush","dirty_conservation",
                   "broadcast_source_multiplicity","explicit_range_rejection","unknown_timestamps",
                   "PAPER_group_conflict_and_independence"}}}).dump()<<'\n';
}
