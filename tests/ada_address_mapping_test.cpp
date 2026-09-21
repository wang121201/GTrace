#include "../source/work/tilegen-full-r1/core-native-copy-r2/include/ada_address_mapping.h"
#include "../source/work/tilegen-full-r1/core-native-copy-r2/include/cache_geometry.h"
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using U = std::uint64_t;
using GTSim::AdaAddressMapping;
#ifdef TILEGEN_TEST_UPSTREAM_XOR
// Link the unedited function extracted from the pinned hashing.cc. This does
// not claim to compile the complete upstream translation unit or simulator.
unsigned bitwise_hash_function(U higher_bits, unsigned index, unsigned count);
#endif
static U checks = 0, addresses = 0;
static void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class F> static void rejects(F f, const char* message) {
    bool caught = false;
    try { f(); } catch (const std::invalid_argument&) { caught = true; }
    check(caught, message);
}

// Independent configuration-driven oracle, not a second call to the new
// decoder: parse all 64 letters, gather selected bits, derive the lowest bank
// bit, then explicitly pack its complement. The set hash is calculated one
// output bit at a time. This follows the pinned addrdec.cc gap + CONSECUTIVE
// path and gpu-cache.cc::l2_cache_config::set_index / X path.
struct Oracle {
    U channels = 0, subparts = 0;
    unsigned dram_bit = 0;
    U bank_mask = 0, row_mask = 0, col_mask = 0, burst_mask = 0, sub_mask = 0;
    explicit Oracle(const char* config) {
        std::ifstream f(config);
        check(bool(f), "reference config must be readable");
        std::map<std::string, std::string> opts;
        std::string line;
        while (std::getline(f, line)) {
            line = line.substr(0, line.find('#'));
            std::istringstream in(line);
            std::string key, value;
            if (in >> key >> value) opts[key] = value;
        }
        channels = std::stoull(opts.at("-gpgpu_n_mem"));
        subparts = std::stoull(opts.at("-gpgpu_n_sub_partition_per_mchannel"));
        check(channels == 10 && subparts == 2, "reference is exactly10 channels x2 subpartitions");
        check(opts.at("-gpgpu_memory_partition_indexing") == "0", "only CONSECUTIVE partition indexing qualified");
        check(opts.at("-gpgpu_cache:dl2") == "S:1024:128:16,L:B:m:L:X,A:192:4,32:0,32", "qualified L2 config and X indexing");
        const auto map = opts.at("-gpgpu_mem_addr_mapping");
        check(map.rfind("dramid@",0) == 0, "dramid mapping syntax");
        const auto semi = map.find(';');
        dram_bit = static_cast<unsigned>(std::stoul(map.substr(7, semi - 7)));
        check(dram_bit == 8, "dramid@8 qualified");
        std::string bits;
        for (char c : map.substr(semi+1)) if (c != '.') bits += c;
        check(bits.size() == 64, "mapping has64 bit positions");
        for (unsigned i = 0; i < bits.size(); ++i) {
            const U bit = U{1} << (63-i);
            switch (bits[i]) {
            case 'B': bank_mask |= bit; break;
            case 'R': row_mask |= bit; break;
            case 'S': burst_mask |= bit; col_mask |= bit; break;
            case 'C': col_mask |= bit; break;
            case '0': break;
            default: throw std::invalid_argument("unsupported reference mapping letter");
            }
        }
        // init() selects log2(2)==1 lowest BANK bit, not a channel bit.
        for (unsigned i=0;i<64;++i) if (bank_mask & (U{1}<<i)) { sub_mask=U{1}<<i; break; }
        check(bank_mask==0x7080 && row_mask==0x0fff8000 && col_mask==0x0f7f && burst_mask==31 && sub_mask==128,
              "parsed reference masks");
    }
    static U gather(U value, U mask) {
        U result=0, output_bit=1;
        for (unsigned i=0;i<64;++i) {
            const U input_bit=U{1}<<i;
            if (!(mask & input_bit)) continue;
            if (value & input_bit) result |= output_bit;
            output_bit <<= 1;
        }
        return result;
    }
    AdaAddressMapping::Decoded decode(U address) const {
        const U granule=U{1}<<dram_bit;
        const U chip=(address/granule)%channels;
        const U rest=((address/granule)/channels)*granule+(address%granule);
        const U bank=gather(rest,bank_mask);
        const U sub=chip*subparts+(bank%subparts);
        const U part=gather(rest,~sub_mask);
        U set=0;
        for (unsigned bit=0;bit<10;++bit)
            if (((part/(U{1}<<(7+bit)))%2) != ((part/(U{1}<<(17+bit)))%2))
                set |= U{1}<<bit;
        return {chip,bank,gather(rest,row_mask),gather(rest,col_mask),gather(rest,burst_mask),sub,rest,part,set,sub*1024+set};
    }
};

static void compare(U va,const Oracle& oracle,const GTSim::L2Geometry& geometry) {
    ++addresses;
    auto a=AdaAddressMapping::decode(va), b=oracle.decode(va);
    check(a.chip==b.chip && a.bank==b.bank && a.row==b.row && a.column==b.column && a.burst==b.burst,
          "DRAM decoded fields match independent parsed-map oracle");
    check(a.rest_of_address==b.rest_of_address && a.partition_address==b.partition_address,
          "nonpow2 quotient and packed partition address match oracle");
    check(a.sub_partition==b.sub_partition && a.l2_set==b.l2_set && a.group==b.group,
          "subpartition and set match oracle");
#ifdef TILEGEN_TEST_UPSTREAM_XOR
    check(a.l2_set==bitwise_hash_function(a.partition_address>>17,
          static_cast<unsigned>((a.partition_address>>7)&1023),1024),
          "set hash matches compiled unedited upstream XOR helper");
#endif
    check(a.chip<10 && a.bank<16 && a.sub_partition<20 && a.l2_set<1024 && a.group<20480,
          "field bounds");
    check(geometry.partition(va)==a.sub_partition && geometry.set(va)==a.l2_set && geometry.group(va)==a.group,
          "new geometry delegates to exact mapping");
    check(AdaAddressMapping::channel(va)==a.chip && AdaAddressMapping::sub_partition(va)==a.sub_partition &&
          AdaAddressMapping::group(va)==a.group,"standalone decoder helpers consistent");
    const U restored=((a.partition_address/128)*10+a.chip)*256+(a.bank%2)*128+(a.partition_address%128);
    check(restored==va,"channel+subpartition+partition address preserve all64 address bits");
}

static U inverse(U sub,U set,U tag) {
    const U local_line=tag*1024+(set^(tag%1024));
    return (local_line*10+sub/2)*256+(sub%2)*128;
}

int main(int argc,char** argv) {
    try {
        check(argc==2,"usage: ada_address_mapping_test /path/to/pinned/gpgpusim.config");
        const Oracle oracle(argv[1]);
        const auto cfg=GTSim::L2GeometryConfig::accelsim_rtx4000_ada_v1();
        const GTSim::L2Geometry geometry(cfg,41943040,128);
        check(geometry.group_count()==20480 && geometry.capacity_per_group()==16 && geometry.total_lines()==327680,
              "exact40MiB total L2,20 local caches each1024x16x128");
        const struct {U address,chip,sub,set,part;} gold[] = {
            {0,0,0,0,0},{127,0,0,0,127},{128,0,1,0,0},{255,0,1,0,127},
            {256,1,2,0,0},{2304,9,18,0,0},{2432,9,19,0,0},
            {2560,0,0,1,128},{2688,0,1,1,128},{2621440,0,0,1,131072}};
        for(const auto& x:gold) {
            auto d=AdaAddressMapping::decode(x.address);
            check(d.chip==x.chip && d.sub_partition==x.sub && d.l2_set==x.set && d.partition_address==x.part,
                  "hand-derived nonpow2/bank7 golden boundary");
        }
        for(U a=0;a<5120;++a) compare(a,oracle,geometry);
        for(U sub=0;sub<20;++sub) for(U set=0;set<1024;++set) {
            for(U tag:{0ULL,1ULL,15ULL,16ULL,1023ULL}) {
                const U va=inverse(sub,set,tag);
                compare(va,oracle,geometry);
                check(geometry.group(va)==sub*1024+set,"all20x1024 groups and higher-tag collision coverage");
            }
            const U base=inverse(sub,set,0);
            for(U offset:{0ULL,31ULL,32ULL,63ULL,64ULL,95ULL,96ULL,127ULL})
                check(geometry.group(base+offset)==geometry.group(base),"all32B sectors stay in their original128B line group");
        }
        for(unsigned bit=0;bit<64;++bit) {
            U a=U{1}<<bit;compare(a,oracle,geometry);compare(a-1,oracle,geometry);
            if(a!=UINT64_MAX) compare(a+1,oracle,geometry);
        }
        U state=0x1292fe789bdULL;
        const GTSim::L2Geometry paper(GTSim::L2GeometryConfig::paper_ada_l2_v1(),41943040,128);
        for(unsigned i=0;i<100000;++i) {
            state=state*6364136223846793005ULL+1442695040888963407ULL;
            compare(state,oracle,geometry);
            const U old_part=(state>>8)%20;
            const U old_block=(((((state>>8)/20)<<8)|(state&255))>>7);
            check(paper.partition(state)==old_part && paper.set(state)==((old_block^(old_block>>10))&1023),
                  "PAPER frozen formula remains unchanged");
        }
        compare(UINT64_MAX,oracle,geometry);compare(UINT64_MAX-127,oracle,geometry);
        check(geometry.partition(128)==1 && paper.partition(128)==0 && geometry.set(128)==0 && paper.set(128)==1,
              "new10x2 mapping explicitly differs from legacy20@8");
        GTSim::L2Geometry fa({},384,128);
        check(fa.group(UINT64_MAX)==0 && fa.capacity_per_group()==3,"legacy fully-associative unchanged");
        rejects([&]{GTSim::L2Geometry x(cfg,41943040-128,128);},"wrong capacity rejected");
        rejects([&]{GTSim::L2Geometry x(cfg,41943040,64);},"wrong line rejected");
        GTSim::L2GroupedLru<U> lru(cfg,41943040,128);
        std::vector<GTSim::L2GroupedLru<U>::iterator> same;
        for(U tag=0;tag<16;++tag) {U a=inverse(19,1023,tag);same.push_back(lru.insert_mru(a,a));}
        const U incoming=inverse(19,1023,16), other=inverse(18,1023,16);
        check(lru.victim(incoming) && *lru.victim(incoming)==inverse(19,1023,0),"17th line selects only local16way victim");
        check(lru.victim(other)==nullptr,"adjacent subpartition does not share ways");
        lru.touch(inverse(19,1023,0),same[0]);
        check(*lru.victim(incoming)==inverse(19,1023,1),"new geometry retains existing LRU behavior");
        std::cout<<"{\"status\":\"PASS_ADA_ADDRESS_MAPPING\",\"checks\":"<<checks<<",\"addresses\":"<<addresses
                 <<",\"oracle\":\"independent_config_parsed_bit_formula\",\"upstream_translation_unit_compiled\":false,"
#ifdef TILEGEN_TEST_UPSTREAM_XOR
                   "\"upstream_XOR_helper_compiled\":true,"
#else
                   "\"upstream_XOR_helper_compiled\":false,"
#endif
                   "\"hardware_hash_claim\":false,\"channels\":10,\"subpartitions_per_channel\":2,\"CPU_only\":true}\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
