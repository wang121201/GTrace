#pragma once
// Decode a bounded HBServe program; never retain expanded grid instructions.
#include <nlohmann/json.hpp>
#include "sha256.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <charconv>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace native_program {
using J = nlohmann::json;
using U = std::uint64_t;
using I = std::int64_t;
inline void need(bool ok, const std::string& why) { if (!ok) throw std::invalid_argument(why); }
template<std::size_t N> inline void need(bool ok,const char (&why)[N]) { if (!ok) throw std::invalid_argument(why); }
inline U natural(const J& x, U cap=UINT64_MAX) {
    need(x.is_number_integer() && !x.is_boolean(), "integer required");
    if (!x.is_number_unsigned()) need(x.get<I>() >= 0, "negative integer");
    U n=x.get<U>(); need(n<=cap, "integer budget exceeded"); return n;
}
inline I signed_integer(const J& x) {
    need(x.is_number_integer() && !x.is_boolean(), "signed integer required");
    if (x.is_number_unsigned()) need(x.get<U>()<=INT64_MAX,"signed integer overflow");
    return x.get<I>();
}
inline U add(U a,U b) { need(b<=UINT64_MAX-a,"unsigned sum overflow");return a+b; }
inline U multiply(U a,U b) { need(!a||b<=UINT64_MAX/a,"unsigned product overflow");return a*b; }
inline U product3(const J& v,U cap) {
    need(v.is_array() && v.size()==3,"3D launch required");U n=1;
    for (const auto& x:v) { U d=natural(x,cap);need(d>0,"zero launch dimension");n=multiply(n,d); }
    need(n<=cap,"launch extent exceeds budget");return n;
}
struct Object { U base,bytes; };
struct Rule {
    int kind; I stride; U period=1;std::vector<I> phase;
    __int128 delta(U cta) const {
        return kind==0 ? __int128(cta)*stride : __int128(cta/period)*stride+phase.at(cta%period);
    }
    std::pair<__int128,__int128> extent(U ctas)const {
        __int128 lo=delta(0),hi=lo;
        if(kind==0){auto d=delta(ctas-1);return {std::min(lo,d),std::max(hi,d)};}
        for(U p=0;p<std::min(ctas,period);++p){U last=p+(ctas-1-p)/period*period;
            for(auto c:{p,last}){auto d=delta(c);lo=std::min(lo,d);hi=std::max(hi,d);}}
        return {lo,hi};
    }
};
struct Lane { int lane,reference,object,rule;U offset; };
struct Record {
    int warp;U pc,function;int opcode;std::uint32_t mask;int width;char op;
    std::vector<Lane> lanes;
};
struct Body { int source_cta;std::vector<Record> records;U nodes=0,lanes=0,read=0,write=0; };
struct Counts { U records=0,nodes=0,lanes=0,read=0,write=0; };
struct Program {
    J identity,qualification,oracle_receipt;
    std::string kind;U ctas=0;int warps=0;
    std::vector<Object> objects;std::vector<Rule> rules;
    std::vector<std::string> opcodes;std::vector<Body> bodies;
    std::map<int,std::size_t> exact;
    Counts totals;

    explicit Program(const J& j) {
        need(j.at("schema")=="TILEGEN_NATIVE_CTA_MEMORY_PROGRAM_V1","native program schema");
        identity=j.at("source");qualification=j.at("qualification");
        need(identity.is_object()&&qualification.is_object(),"program source and qualification required");
        const auto& begin=identity.at("begin");
        need(begin.at("grid")==j.at("grid")&&begin.at("block")==j.at("block"),"program/source launch mismatch");
        need(begin.at("code_sha256").is_string()&&begin.at("code_sha256").get<std::string>().size()==64,"source code SHA required");
        ctas=product3(j.at("grid"),10000000);U threads=product3(j.at("block"),1024);warps=int((threads+31)/32);
        kind=j.at("generation_kind").get<std::string>();
        need(kind=="affine_periodic"||kind=="exact_anchors","unknown CTA generation kind");
        const auto& os=j.at("objects");need(os.is_array()&&!os.empty()&&os.size()<=65536,"bounded objects required");
        for(const auto& o:os){need(natural(o.at("id"))==objects.size(),"dense object ids required");
            U base=natural(o.at("base")),bytes=natural(o.at("bytes"));
            need(bytes>0&&bytes<=UINT64_MAX-base,"object extent overflow");objects.push_back({base,bytes});}
        const auto& rs=j.at("rules");need(rs.is_array()&&rs.size()<=1000000,"bounded rules required");
        for(const auto& a:rs){need(a.is_array()&&a.size()>=2,"invalid rule");Rule r; r.kind=int(natural(a[0],1));
            if(r.kind==0){need(a.size()==2,"affine rule arity");r.stride=signed_integer(a[1]);}
            else{need(a.size()>=4,"periodic rule arity");r.period=natural(a[1],4096);need(r.period>0&&a.size()==r.period+3,"periodic phases missing");r.stride=signed_integer(a[2]);for(U i=0;i<r.period;++i)r.phase.push_back(signed_integer(a[i+3]));}
            rules.push_back(std::move(r));}
        need(!rules.empty(),"at least a zero rule required");
        const auto& ops=j.at("opcodes");need(ops.is_array()&&!ops.empty()&&ops.size()<=4096,"bounded opcode table");
        std::set<std::string> opcode_set;
        for(const auto& s:ops){auto op=s.get<std::string>();need(!op.empty()&&op.size()<=256&&opcode_set.insert(op).second,"invalid/duplicate opcode");opcodes.push_back(op);}
        const auto& ps=j.at("programs");need(ps.is_array()&&!ps.empty()&&ps.size()<=4096,"bounded CTA bodies");
        U input_records=0,input_lanes=0;
        for(const auto& p:ps){Body b;b.source_cta=int(natural(p.at("source_cta"),ctas-1));
            const auto& recs=p.at("records");need(recs.is_array()&&!recs.empty(),"nonempty CTA body required");
            for(const auto& r:recs){Record a;a.warp=int(natural(r.at("warp"),warps-1));a.pc=natural(r.at("pc"));a.function=natural(r.at("function"));
                a.opcode=int(natural(r.at("opcode"),opcodes.size()-1));a.mask=std::uint32_t(natural(r.at("effective_mask"),UINT32_MAX));a.width=int(natural(r.at("width"),16));
                need(a.width==1||a.width==2||a.width==4||a.width==8||a.width==16,"memory width");
                auto op=r.at("op").get<std::string>();need(op=="R"||op=="W","memory operation");a.op=op[0];
                need(r.at("space_metadata")==1,"global reference metadata required");
                const auto& ls=r.at("lanes");need(ls.is_array()&&ls.size()<=32,"at most32 source lanes");
                std::set<std::pair<int,int>> seen;int previous=-1;
                for(const auto& l:ls){need(l.is_array()&&l.size()==5,"lane tuple arity");Lane x;
                    x.lane=int(natural(l[0],31));x.reference=int(natural(l[1],1));x.object=int(natural(l[2],objects.size()-1));x.offset=natural(l[3]);x.rule=int(natural(l[4],rules.size()-1));
                    need(x.lane>previous&&seen.insert({x.lane,x.reference}).second,"unique ascending active lanes required");previous=x.lane;
                    need((a.mask>>x.lane)&1U,"source lane outside effective instruction mask");
                    auto span=kind=="affine_periodic"?rules[x.rule].extent(ctas):std::make_pair(rules[x.rule].delta(b.source_cta),rules[x.rule].delta(b.source_cta));
                    const auto& o=objects[x.object];
                    need(__int128(x.offset)+span.first>=0&&__int128(x.offset)+span.second+a.width<=o.bytes,"generated lane escapes object extent");
                    need(__int128(o.base)+x.offset+span.first>0,"null generated source address");
                    a.lanes.push_back(x);}
                if(!a.lanes.empty())++b.nodes;b.lanes+=a.lanes.size();
                (a.op=='R'?b.read:b.write)+=a.lanes.size()*a.width;
                b.records.push_back(std::move(a));
                need(++input_records<=2000000,"compact record budget");input_lanes+=ls.size();need(input_lanes<=16000000,"compact lane budget");}
            need(b.nodes>0,"CTA with no modeled memory must be handled explicitly");
            need(exact.emplace(b.source_cta,bodies.size()).second,"duplicate CTA body");bodies.push_back(std::move(b));}
        if(kind=="affine_periodic"){need(bodies.size()==1&&bodies[0].source_cta==0,"translation needs CTA0 source");
            const auto& b=bodies[0];totals={multiply(b.records.size(),ctas),multiply(b.nodes,ctas),multiply(b.lanes,ctas),multiply(b.read,ctas),multiply(b.write,ctas)};}
        else{need(bodies.size()==ctas,"exact anchors must cover complete grid");
            for(U c=0;c<ctas;++c){need(exact.count(int(c)),"exact anchor grid gap");const auto& b=body(c);totals.records=add(totals.records,b.records.size());totals.nodes=add(totals.nodes,b.nodes);totals.lanes=add(totals.lanes,b.lanes);totals.read=add(totals.read,b.read);totals.write=add(totals.write,b.write);}}
        // Node IDs are per kernel; never require a full-model dense node table.
        need(totals.nodes<=INT32_MAX,"native per-kernel node id domain exceeded");
        validate_oracles(j.at("oracles"));
    }
    const Body& body(U cta)const { need(cta<ctas,"CTA outside grid");return kind=="affine_periodic"?bodies[0]:bodies.at(exact.at(int(cta))); }
    U address(const Lane& lane,int width,U cta)const {
        const auto& o=objects.at(lane.object);__int128 off=__int128(lane.offset)+rules.at(lane.rule).delta(cta);
        need(off>=0&&off+width<=o.bytes,"expanded address outside object");return add(o.base,U(off));
    }
    J semantics(const Record& r,U cta)const {
        J lanes=J::array();for(const auto& l:r.lanes)lanes.push_back({{"lane",l.lane},{"ref_id",l.reference},{"addr",address(l,r.width,cta)},{"is_local",0},{"local_offset",0}});
        return {{"pc",r.pc},{"opcode",opcodes.at(r.opcode)},{"mask",r.mask},{"mem_width",r.width},{"op",int(r.op)},{"has_space_metadata",1},{"cta_warp",r.warp},{"function_id",r.function},{"lanes",lanes}};
    }

    // Exact bytes of semantics(r,cta).dump()+"\n". Preserve the original JSON
    // key order and integer formatting; only the transient JSON DOM is removed.
    // Opcode strings are escaped once by the same nlohmann serializer.
    template<class Integer> static void append_semantic_integer(std::string& out,Integer x) {
        char bytes[32];const auto result=std::to_chars(bytes,bytes+sizeof bytes,x);
        need(result.ec==std::errc(),"semantic integer encoding");out.append(bytes,result.ptr);
    }
    void append_semantics_bytes(const Record& r,U cta,const std::string& encoded_opcode,std::string& out)const {
        out.clear();out+="{\"cta_warp\":";append_semantic_integer(out,r.warp);
        out+=",\"function_id\":";append_semantic_integer(out,r.function);
        out+=",\"has_space_metadata\":1,\"lanes\":[";
        bool first=true;for(const auto& l:r.lanes){if(!first)out+=',';first=false;
            out+="{\"addr\":";append_semantic_integer(out,address(l,r.width,cta));
            out+=",\"is_local\":0,\"lane\":";append_semantic_integer(out,l.lane);
            out+=",\"local_offset\":0,\"ref_id\":";append_semantic_integer(out,l.reference);out+='}';}
        out+="],\"mask\":";append_semantic_integer(out,r.mask);
        out+=",\"mem_width\":";append_semantic_integer(out,r.width);
        out+=",\"op\":";append_semantic_integer(out,int(r.op));
        out+=",\"opcode\":";out+=encoded_opcode;
        out+=",\"pc\":";append_semantic_integer(out,r.pc);out+="}\n";
    }

    void validate_oracles(const J& oracles) {
        need(oracles.is_array()&&!oracles.empty()&&oracles.size()<=131072,"bounded source oracles required");
        std::map<int,std::map<int,J>> wanted;
        for(const auto& o:oracles){int c=int(natural(o.at("cta"),ctas-1)),w=int(natural(o.at("warp"),warps-1));need(wanted[c].emplace(w,o).second,"duplicate CTA/warp oracle");}
        const auto& begin=identity.at("begin");std::set<int> declared;
        for(const auto* group:{"fit_ctas","holdout_ctas"}){need(begin.at(group).is_array(),"source sample plan");for(const auto& c:begin.at(group))need(declared.insert(int(natural(c,ctas-1))).second,"overlapping fit/holdout coordinates");}
        std::set<int> available;U verified=0,records=0;
        std::vector<std::string> encoded_opcodes;encoded_opcodes.reserve(opcodes.size());
        for(const auto& opcode:opcodes)encoded_opcodes.push_back(J(opcode).dump());
        std::string semantic_scratch;semantic_scratch.reserve(4096);
        for(const auto& [cta, ws]:wanted){available.insert(cta);std::map<int,tiny_sha::Sha256> digests;std::map<int,Counts> counts;
            for(const auto& r:body(cta).records){append_semantics_bytes(r,cta,encoded_opcodes.at(r.opcode),semantic_scratch);digests[r.warp].add(semantic_scratch);auto& c=counts[r.warp];++c.records;c.lanes+=r.lanes.size();(r.op=='R'?c.read:c.write)+=r.lanes.size()*r.width;}
            need(digests.size()==ws.size(),"source oracle warp coverage differs");
            for(const auto& [warp,o]:ws){need(digests.count(warp),"source oracle missing generated warp");const auto& c=counts.at(warp);
                need(natural(o.at("records"))==c.records&&o.at("sha256")==digests.at(warp).hex(),"source semantic count/SHA mismatch");
                for(const auto& [field,value]:std::vector<std::pair<std::string,U>>{{"lane_references",c.lanes},{"read_bytes",c.read},{"write_bytes",c.write}})if(o.contains(field))need(natural(o.at(field))==value,"source oracle traffic mismatch");
                ++verified;records+=c.records;}}
        need(available==declared,"oracles differ from complete declared fit/holdout plan");
        if(kind=="exact_anchors")need(available.size()==ctas,"exact anchor source oracle gap");
        else need(!begin.at("holdout_ctas").empty(),"modeled full grid requires independent holdouts");
        oracle_receipt={{"status","PASS_NATIVE_DECODER_SOURCE_SEMANTICS"},{"selected_ctas",wanted.size()},{"warp_programs",verified},{"records",records},{"oracle_encoding","original codec.semantic_bytes; sorted compact JSON + newline"},{"full_grid_dynamic_observed",kind=="exact_anchors"},{"timing_or_SM_order_proven",false}};
    }
    J census()const { return {{"ctas",ctas},{"warps_per_cta",warps},{"memory_records",totals.records},{"memory_nodes",totals.nodes},{"lane_references",totals.lanes},{"requested_read_bytes",totals.read},{"requested_write_bytes",totals.write},{"compact_records",[&](){U n=0;for(const auto& b:bodies)n+=b.records.size();return n;}()}}; }
};
}
