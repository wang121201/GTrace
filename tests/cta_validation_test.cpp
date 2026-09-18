#include "cta_graph_store.h"
#include <climits>
#include <iostream>
#include <set>
#include <string>

namespace g=GTSim;
using Store=g::CtaGraphStore;
static std::uint64_t checks=0,cases=0;
static void check(bool ok,const std::string& message) {
    ++checks;if(!ok)throw std::runtime_error(message);
}
static void require(bool ok,const char* reason) {
    if(!ok)throw std::logic_error(reason);
}

// Independent copy of the three pre-optimization gates. Other production
// guards are exercised using fresh, valid SIMD nodes, not copied here.
static void old_gate(const Store::Spec& spec,int c,const Store::Owned& owned) {
    require(c>=0&&c<spec.cta_count,"CTA outside declared layout");
    const auto& span=spec.spans.at(c);
    require(owned.size()==std::size_t(span.node_count),"CTA node count");
    const int first=span.first_node;
    std::set<std::string> names;
    for(int i=0;i<span.node_count;++i) {
        const auto* n=owned[i].get();
        require(n&&n->id==first+i&&n->thread_block_id==c&&n->sm_id==c%spec.sm_count,
                "CTA global identity/placement");
        bool valid_warp=false;
        for(int local=0;local<spec.warps_per_cta;++local)
            valid_warp|=n->warp_id==g::cta_placement::token(c,local,
                spec.warps_per_cta,spec.sm_count,4,spec.per_sm_warp_placement);
        require(valid_warp,"CTA scheduler warp token identity");
        require(names.insert(n->name).second,"CTA duplicate node name");
        std::set<int> edges;
        for(int dep:n->depends_on)
            require(dep>=first&&dep<n->id&&edges.insert(dep).second,
                    "CTA nonlocal/non-topological/duplicate completion dependency");
        for(int dep:n->issue_depends_on)
            require(dep>=first&&dep<n->id&&edges.insert(dep).second,
                    "CTA nonlocal/non-topological/duplicate issue dependency");
    }
}
struct Outcome {bool accepted;std::string error;};
template<class F>static Outcome outcome(F&& f) {
    try {f();return {true,{}};}
    catch(const std::logic_error& error) {return {false,error.what()};}
}
static Store::Spec spec_for(int warps,bool per_sm) {
    Store::Spec spec{};
    spec.cta_count=97;spec.warps_per_cta=warps;spec.sm_count=48;
    spec.per_sm_warp_placement=per_sm;
    const int count=std::max(6,warps);
    spec.total_nodes=std::size_t(spec.cta_count)*count;
    for(int c=0;c<spec.cta_count;++c)spec.spans.push_back({c*count,count});
    return spec;
}
static Store::Owned fixture(const Store::Spec& spec,int c) {
    Store::Owned owned;const auto& span=spec.spans.at(c);
    for(int i=0;i<span.node_count;++i) {
        const int id=span.first_node+i;
        auto node=std::make_unique<g::DAGNode>(id,"node"+std::to_string(id),
            "SIMD","compute",g::cta_placement::token(c,i%spec.warps_per_cta,
                spec.warps_per_cta,spec.sm_count,4,spec.per_sm_warp_placement),
            std::vector<int>{},0,g::Tile(0,0,1,32),g::DataType::INT8);
        node->sm_id=c%spec.sm_count;node->thread_block_id=c;
        if(i>0)node->depends_on.push_back(id-1);
        if(i>1)node->issue_depends_on.push_back(id-2);
        node->remaining_deps=int(node->depends_on.size()+node->issue_depends_on.size());
        owned.push_back(std::move(node));
    }
    return owned;
}
template<class Edit>static void compare(const Store::Spec& spec,Store& store,int c,
        const std::string& name,bool accepted,Edit edit) {
    auto owned=fixture(spec,c);edit(owned);
    const auto before=store.telemetry();
    const auto old=outcome([&]{old_gate(spec,c,owned);});
    const auto now=outcome([&]{store.validate_cta(c,owned);});
    ++cases;
    const std::string label=name+" CTA="+std::to_string(c)+" warps="+
        std::to_string(spec.warps_per_cta)+(spec.per_sm_warp_placement?" per-SM":" linear");
    check(old.accepted==accepted,label+": fixture oracle outcome");
    check(now.accepted==old.accepted,label+": acceptance changed");
    check(now.error==old.error,label+": rejection reason changed");
    check(store.telemetry()==before,label+": validation changed store counters");
    // The same object can be validated twice; scratch state must not leak.
    const auto again=outcome([&]{store.validate_cta(c,owned);});
    check(again.accepted==now.accepted&&again.error==now.error,label+": scratch state leaked");
}
static void run(int warps,bool per_sm) {
    const auto spec=spec_for(warps,per_sm);
    Store store(spec,[](int){return Store::Owned{};},
                [](int,const std::vector<g::DAGNode*>&,g::Cycle){});
    for(int c:{0,1,47,48,49,95,96}) {
        const int first=spec.spans[c].first_node;
        auto test=[&](const char* label,bool accepted,auto edit){
            compare(spec,store,c,label,accepted,edit);
        };
        test("valid complete/issue edges",true,[](auto&){});
        test("same predecessor on different nodes",true,[&](auto& v){
            for(int i:{2,3}){v[i]->depends_on={first};v[i]->issue_depends_on.clear();}
        });
        test("multiple unique unordered edges",true,[&](auto& v){
            v[5]->depends_on={first+3,first,first+2};v[5]->issue_depends_on={first+4,first+1};
        });
        test("empty dependency sets",true,[](auto& v){
            for(auto& n:v){n->depends_on.clear();n->issue_depends_on.clear();}
        });
        test("duplicate completion",false,[&](auto& v){v[5]->depends_on={first,first};});
        test("duplicate issue",false,[&](auto& v){v[5]->issue_depends_on={first,first};});
        test("completion/issue duplicate",false,[&](auto& v){
            v[5]->depends_on={first};v[5]->issue_depends_on={first};
        });
        for(int dep:{INT_MIN,-1,first-1,first+5,first+6,INT_MAX}) {
            test("invalid completion boundary",false,[&](auto& v){v[5]->depends_on={dep};});
            test("invalid issue boundary",false,[&](auto& v){v[5]->issue_depends_on={dep};});
        }
        test("first node cannot depend on itself",false,[&](auto& v){v[0]->depends_on={first};});
        test("negative warp",false,[](auto& v){v[0]->warp_id=-1;});
        test("overflow warp",false,[](auto& v){v[0]->warp_id=INT_MAX;});
        test("next CTA warp",false,[&](auto& v){
            v[0]->warp_id=g::cta_placement::token(c+1,0,warps,48,4,per_sm);
        });
        if(c!=0)test("zero must not match unused array tail",false,[](auto& v){v[0]->warp_id=0;});
        test("duplicate name",false,[](auto& v){v[1]->name=v[0]->name;});
        test("long names sharing prefix",true,[](auto& v){
            v[0]->name=std::string(4096,'x')+"a";v[1]->name=std::string(4096,'x')+"b";
        });
        test("duplicate long name",false,[](auto& v){
            v[0]->name=std::string(4096,'x')+"a";v[1]->name=v[0]->name;
        });
        test("embedded NUL distinct suffixes",true,[](auto& v){
            v[0]->name=std::string("prefix\0a",8);v[1]->name=std::string("prefix\0b",8);
        });
        test("embedded NUL vs prefix",true,[](auto& v){
            v[0]->name="prefix";v[1]->name=std::string("prefix\0",7);
        });
        test("duplicate embedded NUL name",false,[](auto& v){
            v[0]->name=std::string("prefix\0a",8);v[1]->name=v[0]->name;
        });
        test("null node",false,[](auto& v){v[2].reset();});
        test("wrong node identity",false,[](auto& v){++v[2]->id;});
        test("wrong placement",false,[](auto& v){++v[2]->sm_id;});
    }
    auto owned=fixture(spec,0);
    for(int c:{-1,spec.cta_count}) {
        const auto old=outcome([&]{old_gate(spec,c,owned);});
        const auto now=outcome([&]{store.validate_cta(c,owned);});
        ++cases;check(!old.accepted&&!now.accepted&&old.error==now.error,"CTA layout bounds");
    }
    owned.pop_back();
    const auto old=outcome([&]{old_gate(spec,0,owned);});
    const auto now=outcome([&]{store.validate_cta(0,owned);});
    ++cases;check(!old.accepted&&!now.accepted&&old.error==now.error,"CTA node count bounds");
}
int main() {
    try {
        for(bool per_sm:{false,true})for(int warps:{1,2,4,31,32})run(warps,per_sm);
        std::cout<<"{\"status\":\"PASS_CTA_VALIDATION_EQUIVALENCE\",\"checks\":"<<checks
            <<",\"cases\":"<<cases<<",\"GPU_executed\":false,\"HBFSIM_executed\":false}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
