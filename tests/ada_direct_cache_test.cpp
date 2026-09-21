#include "../source/direct_cache.h"
#include <iostream>
using namespace direct_native;
static g::ExplicitMemorySubop range(U a,U n,int lane=0) {
    g::ExplicitMemorySubop s;s.requested_bytes=n;s.ranges.push_back({lane,a,n});
    s.source_member_ordinals.push_back(lane);return s;
}
static constexpr U capacity=20ULL*1024*16*128;
static U colliding(U k){return (1025*k)*10*256;}
static Context context(){Context c;c.call_index=0;c.cta=0;c.warp=0;c.sm_id=0;return c;}
static void check(bool value,const char* why){require(value,why);}
int main(){
    const auto geometry=g::L2GeometryConfig::accelsim_rtx4000_ada_v1();
    g::PerSmL1Config bypass;bypass.mode=g::PerSmL1Mode::BYPASS;
    auto mapper=[](int,U a){return a+4096;};
    std::vector<native_trace::Record> out;
    auto sink=[&](const native_trace::Record& r){out.push_back(r);};
    auto c=context();unsigned checks=0;
    {
        FunctionalCache cache(bypass,capacity,mapper,sink,geometry);
        cache.instruction(1,true,false,{range(5,1)},c);
        check(out.empty(),"lazy partial store must not issue RFO");
        auto s=cache.snapshot();
        check(s.at("dirty_sector_creations")==1&&s.at("resident_dirty_sectors")==1,"store dirty ledger");
        cache.instruction(1,false,false,{range(5,1)},c);
        check(out.size()==1&&out[0].bytes==32&&out[0].service_address==4096,"even written subset needs full sector merge while unreadable");
        check(cache.snapshot().at("L2_sector_merge_reads")==1,"merge read counted");
        cache.instruction(1,false,false,{range(6,2)},c);
        check(out.size()==1,"completed merge makes sector readable");
        cache.instruction(1,false,false,{range(64,1)},c);
        check(out.size()==2&&out.back().bytes==32&&out.back().service_address==4160,"other sector must miss despite same line tag");
        for(U k=1;k<=16;++k)cache.instruction(1,false,false,{range(colliding(k),1)},c);
        s=cache.snapshot();cache.verify_resident_ledger();
        check(s.at("DRAM_write_bytes")==32&&s.at("evicted_dirty_sectors")==1&&s.at("resident_dirty_sectors")==0,"partial-store/read/evict writes one dirty sector");
        check(s.at("DRAM_read_bytes")==18*32,"Ada reads use sector bytes");
        check(out[out.size()-2].cause==native_trace::Cause::ReadFillOrRfo &&
              out.back().cause==native_trace::Cause::DirtyWriteback,
              "read-miss queue precedes victim writeback like pinned rd_miss_base");
        bool seen=false;
        for(const auto& r:out){
            check(r.bytes==32&&r.l2_subpartition_id==g::AdaAddressMapping::sub_partition(r.source_line_address),"every record 32B and true source memory subpartition");
            check(r.issue_cycle==native_trace::unknown&&r.admission_ps==native_trace::unknown,"functional times remain unknown");
            if(r.cause==native_trace::Cause::DirtyWriteback){seen=true;check(r.service_address==4096,"victim mapper and dirty offset preserved");}
        }
        check(seen,"dirty victim record exists");checks+=11;
    }
    out.clear();
    {
        FunctionalCache cache(bypass,capacity,mapper,sink,geometry);
        cache.instruction(1,true,false,{range(0,16)},c);
        cache.instruction(1,true,false,{range(8,8)},c);
        cache.instruction(1,true,false,{range(16,16)},c);
        cache.instruction(1,false,false,{range(0,32)},c);
        check(out.empty(),"union of known byte masks completes full sector without read");
        cache.instruction(1,true,false,{range(31,2)},c);
        cache.instruction(1,false,false,{range(32,1)},c);
        check(out.size()==1&&out[0].service_address==4128,"cross-sector store preserves partial sibling coverage");
        const auto s=cache.snapshot();cache.verify_resident_ledger();
        check(s.at("dirty_sector_creations")==2&&s.at("resident_dirty_sectors")==2&&s.at("DRAM_write_bytes")==0,"overlap has unique dirty creations and no implicit final flush");
        checks+=3;
    }
    out.clear();
    {
        FunctionalCache cache(bypass,capacity,mapper,sink,geometry);
        cache.instruction(1,true,false,{range(0,1),range(64,1)},c);
        cache.instruction(1,false,false,{range(128,1)},c);
        check(out.back().l2_subpartition_id==1,"address128 selects channel0 subpartition1");
        cache.instruction(1,false,false,{range(256,1)},c);
        check(out.back().l2_subpartition_id==2,"address256 selects channel1 subpartition0");
        for(U k=1;k<=16;++k)cache.instruction(1,false,false,{range(colliding(k),1)},c);
        std::vector<U> wb;for(const auto&r:out)if(r.cause==native_trace::Cause::DirtyWriteback)wb.push_back(r.service_address);
        check(wb==std::vector<U>{4096,4160},"sparse dirty mask emits only correct two 32B sectors");
        check(cache.snapshot().at("DRAM_write_bytes")==64,"sparse write byte ledger");
        cache.verify_resident_ledger();checks+=4;
    }
    out.clear();
    {
        g::PerSmL1Config l1;l1.mode=g::PerSmL1Mode::MODELED_SET_ASSOCIATIVE;
        l1.capacity_bytes_per_sm=128;l1.ways=1;l1.sector32=true;l1.write_allocate=true;
        FunctionalCache cache(l1,capacity,mapper,sink,geometry);cache.begin_kernel();
        cache.instruction(1,true,false,{range(0,16)},c);
        cache.instruction(1,true,false,{range(16,16)},c);
        cache.instruction(1,false,false,{range(0,4)},c);
        check(out.empty()&&cache.snapshot().at("L1_read_hits")==1,"Ada L1 known-byte union filters readable store allocation");
        cache.instruction(1,false,false,{range(32,1)},c);
        check(out.size()==1&&out[0].service_address==4128,"L1 missing sibling must forward only sibling");
        cache.instruction(1,false,false,{range(31,2)},c);
        check(out.size()==1,"multi-sector read filters only after both sectors ready");
        cache.instruction(1,false,false,{range(31,34)},c);
        check(out.size()==2&&out.back().service_address==4160,
              "mixed L1 ready/missing read forwards only missing sector");
        cache.reconfigure_l1(256,2);cache.begin_kernel();
        const auto before=cache.snapshot();
        cache.instruction(1,false,false,{range(0,1)},c);
        check(out.size()==2&&cache.snapshot().at("L1_read_misses").get<U>()==
                before.at("L1_read_misses").get<U>()+1,
              "capacity reconfiguration invalidates L1 while preserving L2 data");
        cache.verify_resident_ledger();checks+=5;
    }
    std::cout<<J({{"status","PASS_ADA_FUNCTIONAL_DIRECT_CACHE"},{"checks",checks},
        {"scope","32B sector/lazy-write functional projection; no asynchronous timing or final flush"}}).dump()<<'\n';
}
