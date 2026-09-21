#include "../source/work/tilegen-full-r1/core-native-copy-r2/include/cache_geometry.h"
#include "../source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <utility>

using U = std::uint64_t;
using namespace GTSim;
static U checks = 0;
static void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
template<class F> static void rejects(F body, const char* message) {
    bool caught = false;
    try { body(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}
struct Key {
    int matrix;
    U address;
    bool operator==(const Key&) const = default;
    bool operator<(const Key& b) const { return std::tie(matrix,address) < std::tie(b.matrix,b.address); }
};

// Construct distinct full original VAs for a requested (slice,set,tag).
// This inverse construction also demonstrates every set and every way can be
// reached, without using the production index function to search for inputs.
static U address(U slice, U set, U tag) {
    U low = set ^ (tag % 1024);
    U local_line = tag * 1024 + low;
    return ((local_line / 2) * 20 + slice) * 256 + (local_line % 2) * 128;
}
static U oracle_group(U va) {
    U interleave = va / 256;
    U slice = interleave % 20;
    U local_line = (interleave / 20) * 2 + (va / 128) % 2;
    U set = 0;
    // Independent bit-by-bit version of the frozen two-chunk XOR surrogate.
    for (unsigned b = 0; b < 10; ++b)
        set += (((local_line / (U{1} << b)) % 2) !=
                ((local_line / (U{1} << (b + 10))) % 2)) ? U{1} << b : 0;
    return slice * 1024 + set;
}

static void geometry_cases() {
    const auto cfg = L2GeometryConfig::paper_ada_l2_v1();
    L2Geometry g(cfg, 41943040, 128);
    check(g.total_lines() == 327680 && g.group_count() == 20480 &&
          g.capacity_per_group() == 16 && g.line_bytes() == 128, "fixed Ada capacity");
    struct Golden { U va, slice, set; };
    const std::array<Golden,11> gold{{{0,0,0},{128,0,1},{256,1,0},
        {4864,19,0},{5120,0,2},{5248,0,3},{0x280000,0,1},
        {0x280080,0,0},{0x500000,0,2},{UINT64_MAX-127,15,1023},{UINT64_MAX,15,1023}}};
    for (const auto& x : gold)
        check(g.partition(x.va) == x.slice && g.set(x.va) == x.set &&
              g.group(x.va) == x.slice * 1024 + x.set, "frozen golden slice/set cases");
    for (U slice = 0; slice < 20; ++slice)
        for (U set = 0; set < 1024; ++set) {
            U va = address(slice,set,0);
            check(g.group(va) == slice * 1024 + set, "all groups reachable");
            check(g.group(va + 127) == g.group(va), "all byte offsets stay in the same128B line");
            for (U tag : {1ULL,16ULL,1023ULL,1024ULL,4096ULL})
                check(g.group(address(slice,set,tag)) == g.group(va), "higher tags collide in intended group");
        }
    U state = 0x1292fe789bdULL;
    for (unsigned i = 0; i < 10000; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        check(g.group(state) == oracle_group(state), "64bit independent index oracle");
    }
    check(g.group(0) != g.group(5120), "20 slices use quotient/remainder, not five-bit removal");
    L2Geometry old({}, 384, 128);
    check(old.group(UINT64_MAX) == 0 && old.capacity_per_group() == 3, "legacy fully associative group");
    rejects([&]{L2Geometry x(cfg,41943040-128,128);}, "wrong Ada capacity rejected");
    rejects([&]{L2Geometry x(cfg,41943040,64);}, "wrong Ada line rejected");
    rejects([]{L2Geometry x({},128,0);}, "zero line rejected");
    rejects([]{L2Geometry x({},384,96);}, "nonpower line rejected");
    rejects([]{L2Geometry x({},129,128);}, "fractional line rejected");
    rejects([]{L2GeometryConfig c;c.mode=static_cast<L2GeometryMode>(255);L2Geometry x(c,128,128);}, "unknown geometry rejected");
    L2GroupedLru<Key> disabled({},0,128);
    check(disabled.size() == 0 && disabled.victim(0) == nullptr, "legacy disabled cache has no victim");
    rejects([&]{disabled.insert_mru(0,{0,0});}, "disabled insertion rejected");
}

static void legacy_order() {
    L2GroupedLru<Key> lru({},384,128);
    auto a = lru.insert_mru(0,{1,0});
    auto b = lru.insert_mru(128,{1,128});
    check(lru.victim(256) == nullptr, "nonfull group no victim");
    lru.touch(0,a);
    auto c = lru.insert_mru(256,{1,256});
    check(*lru.victim(384) == Key{1,128}, "legacy A B touchA C evictsB");
    lru.touch(0,a);lru.touch(0,a);
    check(lru.size() == 3 && *lru.victim(384) == Key{1,128}, "repeated touch is idempotent");
    rejects([&]{lru.insert_mru(384,{1,384});}, "full insertion cannot silently evict");
    check(lru.size() == 3 && *b == Key{1,128}, "rejected insert leaves state and iterators intact");
    lru.erase(128,b);auto d = lru.insert_mru(384,{1,384});
    check(*lru.victim(512) == Key{1,256}, "replace former LRU preserves remaining order");
    check(*a == Key{1,0} && *c == Key{1,256}, "list iterators survive touches and other insertions");
    lru.erase(0,a);lru.erase(256,c);lru.erase(384,d);
    check(lru.empty() && !lru.victim(0), "erase closes complete membership ledger");
}

static void set_local_order_and_namespace() {
    L2GroupedLru<Key> lru(L2GeometryConfig::paper_ada_l2_v1(),41943040,128);
    std::array<L2GroupedLru<Key>::iterator,16> first{}, other{};
    for (U i = 0; i < 16; ++i) {
        U a = address(0,0,i), b = address(19,1023,i);
        first[i] = lru.insert_mru(a,{1,a});other[i] = lru.insert_mru(b,{1,b});
    }
    U newcomer = address(0,0,16), far = address(19,1023,16);
    check(lru.size() == 32 && lru.group_size(newcomer) == 16, "only target sets consume ways");
    check(*lru.victim(newcomer) == Key{1,address(0,0,0)}, "17th colliding line chooses group LRU");
    auto held_other = *lru.victim(far);
    lru.touch(address(0,0,0),first[0]);lru.touch(address(0,0,0),first[0]);
    check(*lru.victim(newcomer) == Key{1,address(0,0,1)}, "touch only changes requested set order");
    check(*lru.victim(far) == held_other, "other slice/set victim unchanged");
    lru.erase(address(0,0,1),first[1]);lru.insert_mru(newcomer,{1,newcomer});
    check(lru.size() == 32 && *other[0] == held_other, "cross-group insertion does not erase other state");
    U alias = address(7,14,0);
    auto m1 = lru.insert_mru(alias,{1,alias});auto m2 = lru.insert_mru(alias,{2,alias});
    check(lru.group_size(alias) == 2 && *m1 != *m2, "same VA in different matrix namespaces remains two tags");
}

static void capacity_and_reference_stream() {
    const auto cfg = L2GeometryConfig::paper_ada_l2_v1();
    {
        L2GroupedLru<Key> all(cfg,41943040,128);
        for (U way=0;way<16;++way) for(U p=0;p<20;++p) for(U s=0;s<1024;++s) {
            U va=address(p,s,way);all.insert_mru(va,{0,va});
        }
        check(all.size()==327680,"all20x1024x16 ways can coexist");
        for(U p=0;p<20;++p) for(U s=0;s<1024;++s) {
            U va=address(p,s,16);
            check(all.group_size(va)==16 && *all.victim(va)==Key{0,address(p,s,0)},
                  "every full group has its own oldest victim");
        }
    }
    L2GroupedLru<Key> actual(cfg,41943040,128);
    std::map<Key,L2GroupedLru<Key>::iterator> entries;
    std::map<U,std::vector<Key>> oracle;
    U random=19;
    for(unsigned i=0;i<20000;++i) {
        random=random*6364136223846793005ULL+1;
        U p=(random>>7)%3,s=(random>>13)%4,tag=(random>>27)%24;
        Key key{int((random>>49)%2),address(p,s,tag)};
        auto& list=oracle[oracle_group(key.address)];
        auto old=std::find(list.begin(),list.end(),key);
        if(old!=list.end()) {
            actual.touch(key.address,entries.at(key));list.erase(old);
        } else {
            if(list.size()==16) {
                auto v=actual.victim(key.address);
                check(v && *v==list.back(),"reference stream exact victim identity");
                Key doomed=*v;actual.erase(doomed.address,entries.at(doomed));entries.erase(doomed);list.pop_back();
            } else check(actual.victim(key.address)==nullptr,"reference stream no premature victim");
            entries.emplace(key,actual.insert_mru(key.address,key));
        }
        list.insert(list.begin(),key);
        check(actual.size()==entries.size() && actual.group_size(key.address)==list.size(),"reference membership counts");
        if(list.size()==16)check(*actual.victim(key.address)==list.back(),"reference per-access recency");
    }
}

static void l1_store_bypass_keeps_recency() {
    for (bool bypass : {false,true}) {
        PerSmL1Config cfg;
        cfg.mode=PerSmL1Mode::MODELED_SET_ASSOCIATIVE;cfg.num_sms=1;
        cfg.capacity_bytes_per_sm=256;cfg.line_bytes=128;cfg.ways=2;
        cfg.store_bypass=bypass;
        PerSmL1Cache cache(cfg);
        auto read = [&](U address) {
            auto d=cache.access({0,1,address,false,-1,false});
            check(d.outcome==PerSmL1Outcome::READ_MISS && d.read_ticket.valid,
                  "L1 fixture allocates read miss");
            check(cache.complete_read(d.read_ticket),"L1 fixture completes readiness");
        };
        read(0);read(128); //0 is LRU;128 is MRU.
        auto store=cache.access({0,1,0,true,-1,false});
        check(store.forwarded_to_l2 && !store.read_ticket.valid,"store still forwards to L2");
        check(store.outcome==(bypass?PerSmL1Outcome::BYPASS:PerSmL1Outcome::WRITE_HIT),
              "explicit store bypass vs historical write hit");
        read(256); //With bypass,0 is evicted; historical write hit touched0.
        check(cache.classify({0,1,0,false,-1,false}) ==
              (bypass?PerSmL1Outcome::READ_MISS:PerSmL1Outcome::READ_HIT),
              "bypassed store must not refresh resident L1 recency");
        check(cache.classify({0,1,128,false,-1,false}) ==
              (bypass?PerSmL1Outcome::READ_HIT:PerSmL1Outcome::READ_MISS),
              "store bypass preserves opposite victim choice");
        auto before=cache.statistics();
        auto absent=cache.access({0,1,512,true,-1,false});
        check(!absent.allocated && cache.classify({0,1,512,false,-1,false})==PerSmL1Outcome::READ_MISS,
              "store miss never allocates L1");
        auto stats=cache.statistics();
        check(stats.evictions==before.evictions && cache.live_read_tickets()==0,
              "bypass creates no L1 replacement or delivery obligations");
        check(stats.bypassed_transactions==(bypass?2U:0U) && stats.write_hits==(bypass?0U:1U),
              "store bypass counters identify actual filtering boundary");
    }
}

int main() {
    try {
        geometry_cases();legacy_order();set_local_order_and_namespace();capacity_and_reference_stream();l1_store_bypass_keeps_recency();
        std::cout << "{\"status\":\"PASS_CACHE_GEOMETRY\",\"checks\":" << checks
                  << ",\"hardware_hash_claim\":false,\"CPU_only\":true}\n";
    } catch(const std::exception& error) {
        std::cerr << "cache geometry test failed: " << error.what() << '\n';return 1;
    }
}
