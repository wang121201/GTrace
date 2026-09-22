// Replays allocation-relative scalar-read offsets through the shared cache.
// No reference replacement/hash algorithm is implemented in this driver.
#include "ada_r4_profile.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <climits>
#include <ctime>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace g=GTSim;
using J=nlohmann::json;
using U=std::uint64_t;
namespace {
const char* model(bool r4) { return g::ada_r4_serial_model_id(r4); }
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool ok,const char* code) { if(!ok)throw Failure(code); }
struct Case { U id,nominal,cg,expected;std::string request_path; };
unsigned observed_for_nominal(U nominal) {
    if(nominal==98304)return 32768;
    if(nominal==65536)return 65536;
    if(nominal==28672)return 102400;
    throw Failure("unsupported_nominal_capacity");
}
Case parse_case(const std::string& line) {
    // Tokens are validated before conversion; signed/overflow values are not
    // silently accepted by unsigned stream extraction.
    std::istringstream in(line);std::vector<std::string> fields;std::string field;
    while(in>>field)fields.push_back(field);
    require(fields.size()==5,"case_row_requires_five_fields");
    auto integer=[](const std::string& s)->U {
        require(!s.empty()&&s.find_first_not_of("0123456789")==std::string::npos,"case_integer_invalid");
        try {std::size_t used=0;auto x=std::stoull(s,&used);require(used==s.size(),"case_integer_invalid");return x;}
        catch(const std::exception&){throw Failure("case_integer_invalid");}
    };
    Case c{integer(fields[0]),integer(fields[1]),integer(fields[2]),integer(fields[3]),fields[4]};
    require(c.id<=UINT32_MAX && c.cg<=1,"case_identity_or_cg_invalid");
    require(c.expected>0 && c.expected<=std::numeric_limits<std::size_t>::max()/4,"request_count_invalid");
    observed_for_nominal(c.nominal);return c;
}
std::vector<std::uint32_t> load_requests(std::istream& in,U bytes,U expected) {
    require(expected>0,"empty_request_stream");
    require(expected<=std::numeric_limits<std::size_t>::max()/4 && expected<=UINT64_MAX/4,"request_count_overflow");
    require(bytes==expected*4,"request_file_length_mismatch");
    std::vector<std::uint32_t> requests;requests.reserve(static_cast<std::size_t>(expected));
    // The export is little-endian u32. Decode explicitly, independent of host.
    std::array<unsigned char,65536> buffer{};
    U remaining=bytes;
    while(remaining) {
        const auto n=std::min<U>(remaining,buffer.size());
        in.read(reinterpret_cast<char*>(buffer.data()),static_cast<std::streamsize>(n));
        require(static_cast<U>(in.gcount())==n,"short_request_read");
        for(std::size_t i=0;i<n;i+=4) {
            const auto value=std::uint32_t(buffer[i])|(std::uint32_t(buffer[i+1])<<8)|
                (std::uint32_t(buffer[i+2])<<16)|(std::uint32_t(buffer[i+3])<<24);
            require(value%4==0,"unaligned_scalar_u32_offset");
            requests.push_back(value);
        }
        remaining-=n;
    }
    require(in.peek()==std::char_traits<char>::eof(),"request_file_grew_during_read");
    return requests;
}
J description(unsigned shared,bool r4) {
    const auto c=g::make_ada_r4_serial_l1(shared,r4);
    require(c.num_sms==1 && c.sector32 && c.line_bytes==128,"serial_shared_cache_config_mismatch");
    const U nominal=131072-shared;
    return J{{"model_id",model(r4)},{"observed_shared_bytes",shared},{"nominal_l1_bytes",nominal},
        {"capacity_bytes",c.capacity_bytes_per_sm},{"sets",c.capacity_bytes_per_sm/(128*c.ways)},
        {"ways",c.ways},{"policy",g::per_sm_l1_replacement_name(c.replacement)},{"allocation_unit",c.line_bytes},{"sector_bytes",32},
        {"hash",static_cast<unsigned>(c.hash_policy)},{"scale",r4?1062:1000}};
}
struct Counts {U hits=0,misses=0,requests=0,evictions=0;};
Counts replay(const std::vector<std::uint32_t>& offsets,unsigned shared,bool cg,bool r4) {
    g::PerSmL1Cache cache(g::make_ada_r4_serial_l1(shared,r4));
    Counts out;
    for(const auto offset:offsets) {
        g::PerSmL1Access a;
        a.sm_id=0;a.allocation_id=0;a.canonical_line=U(offset)/128*128;
        a.node_id=0;a.is_write=false;a.bypass_l1=cg;
        a.sector_mask=static_cast<std::uint8_t>(1U<<((offset%128)/32));
        a.allocation_relative_byte_offset=a.canonical_line;
        const auto d=cache.access(a);
        ++out.requests;
        if(d.forwarded_to_l2) {
            ++out.misses;
            require(d.forwarded_sector_mask==a.sector_mask,"forwarded_sector_coverage_mismatch");
            require(!d.reservation_failed,"serial_replay_unexpected_reservation_failure");
            if(d.read_ticket.valid)require(cache.complete_read(d.read_ticket),"synchronous_fill_failed");
            else require(cg,"nonbypass_miss_missing_fill_ticket");
        } else {
            ++out.hits;
            require(d.outcome==g::PerSmL1Outcome::READ_HIT && !cg,"invalid_serial_hit");
        }
    }
    const auto s=cache.statistics();out.evictions=s.evictions;
    require(out.hits+out.misses==out.requests && s.pre_l1_reads==out.requests &&
        s.read_hits==out.hits && s.l2_input_transactions==out.misses &&
        s.bypassed_transactions==(cg?out.requests:0) &&
        s.read_misses==(cg?0:out.misses),"source_hit_miss_counter_closure_failed");
    require(cache.live_read_tickets()==0,"unclosed_serial_fill_ticket");
    return out;
}
unsigned self_test() {
    unsigned checks=0;
    auto test=[&](bool ok){++checks;require(ok,"self_test_failure");};
    auto rejects=[&](auto fn){bool failed=false;try{fn();}catch(const Failure&){failed=true;}test(failed);};
    const std::vector<std::uint32_t> stream{0,4,28,32,36,128};
    for(unsigned shared:{32768U,65536U,102400U})for(bool r4:{false,true}) {
        auto c=replay(stream,shared,false,r4);test(c.requests==6&&c.hits==3&&c.misses==3);
        auto same=replay(stream,shared,false,r4);test(c.hits==same.hits&&c.misses==same.misses); // fresh cold instance
        auto cg=replay(stream,shared,true,r4);test(cg.hits==0&&cg.misses==6&&cg.evictions==0);
        const auto d=description(shared,r4);
        const U expected=r4?(shared==32768?102400:(shared==65536?67584:28672)):(131072-shared);
        test(d.at("capacity_bytes")==expected);
        test(d.at("sets")==U(r4?16:4));
    }
    test(parse_case("7 98304 0 6 unused").expected==6);
    rejects([]{parse_case("header nominal cg count path");});
    rejects([]{parse_case("-1 98304 0 6 unused");});
    rejects([]{parse_case("0 98304 2 6 unused");});
    rejects([]{parse_case("0 98304 0 0 unused");});
    rejects([]{parse_case("0 98304 0 6 unused extra");});
    rejects([]{parse_case("0 999 0 6 unused");});
    {std::istringstream empty("");rejects([&]{load_requests(empty,0,1);});}
    {std::istringstream empty("");rejects([&]{load_requests(empty,0,0);});}
    {std::istringstream bad(std::string(3,'\0'));rejects([&]{load_requests(bad,3,1);});}
    {std::istringstream short_file(std::string(3,'\0'));rejects([&]{load_requests(short_file,4,1);});}
    {std::istringstream trailing(std::string(8,'\0'));rejects([&]{load_requests(trailing,4,1);});}
    {std::istringstream good(std::string("\x00\x01\x00\x00",4));test(load_requests(good,4,1)==std::vector<std::uint32_t>{256});}
    {std::istringstream unaligned(std::string("\x01\x00\x00\x00",4));rejects([&]{load_requests(unaligned,4,1);});}
    return checks;
}
}
int main(int argc,char** argv) {
    std::optional<U> case_id;
    try {
        if(argc==2 && std::string(argv[1])=="--describe") {
            J profiles=J::array();for(bool r4:{false,true})for(unsigned shared:{32768U,65536U,102400U})profiles.push_back(description(shared,r4));
            std::cout<<J({{"schema","ADA_R4_SHARED_SERIAL_DESCRIPTION_V1"},{"profiles",profiles},
                {"scope","allocation-relative synchronous serial read sector filter; no timing/writes/GPU"},
                {"cache_implementation","shared PerSmL1Cache"}}).dump()<<'\n';return 0;
        }
        if(argc==2 && std::string(argv[1])=="--self-test") {
            const auto n=self_test();std::cout<<J({{"status","PASS_ADA_R4_SHARED_SERIAL_SELF_TEST"},{"checks",n}}).dump()<<'\n';return 0;
        }
        require(argc==2,"usage_requires_cases_tsv_only");
        std::ifstream cases(argv[1]);require(bool(cases),"cases_file_unreadable");
        std::string line;std::set<U> ids;
        while(std::getline(cases,line)) {
            if(line.find_first_not_of(" \t\r")==std::string::npos)continue;
            case_id.reset();const auto c=parse_case(line);case_id=c.id;
            require(ids.insert(c.id).second,"duplicate_case_id");
            const auto observed=observed_for_nominal(c.nominal);
            std::ifstream file(c.request_path,std::ios::binary|std::ios::ate);require(bool(file),"request_file_unreadable");
            const auto length=file.tellg();require(length>=0,"request_file_size_failed");file.seekg(0);
            auto requests=load_requests(file,static_cast<U>(length),c.expected);
            for(bool r4:{false,true}) {
                const auto wall_begin=std::chrono::steady_clock::now();const auto cpu_begin=std::clock();
                const auto cts=replay(requests,observed,c.cg,r4);
                const auto cpu_end=std::clock();require(cpu_begin!=std::clock_t(-1)&&cpu_end!=std::clock_t(-1),"process_cpu_clock_unavailable");
                const double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-wall_begin).count();
                J row=description(observed,r4);
                row.update({{"schema","ADA_R4_SHARED_SERIAL_CASE_V1"},{"status","PASS"},
                    {"case_id",c.id},{"cg",bool(c.cg)},{"hits",cts.hits},{"misses",cts.misses},
                    {"requests",cts.requests},{"expected_requests",c.expected},{"L2_read_sectors",cts.misses},
                    {"evictions",cts.evictions},{"CPU_seconds",double(cpu_end-cpu_begin)/CLOCKS_PER_SEC},
                    {"wall_seconds",wall},{"timing_scope","cold cache construction and replay; excludes input IO"},
                    {"source_kind","allocation_relative_u32_scalar_read_offsets"},{"error",nullptr}});
                std::cout<<row.dump()<<'\n'<<std::flush;
            }
        }
        require(!cases.bad(),"cases_file_read_failed");require(!ids.empty(),"empty_cases_file");return 0;
    } catch(const Failure& e) {
        J row={{"schema","ADA_R4_SHARED_SERIAL_ERROR_V1"},{"status","FAILED"},{"error",e.what()}};
        row["case_id"]=case_id?J(*case_id):J(nullptr);std::cerr<<row.dump()<<'\n';return 2;
    } catch(const std::exception&) {
        // Never echo exceptions potentially containing an address or input path.
        J row={{"schema","ADA_R4_SHARED_SERIAL_ERROR_V1"},{"status","FAILED"},{"error","cache_or_runtime_failure"}};
        row["case_id"]=case_id?J(*case_id):J(nullptr);std::cerr<<row.dump()<<'\n';return 3;
    }
}
