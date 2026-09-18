#pragma once
#include "helper_class_gate.h"
namespace helper_bridge {
constexpr U FRAME_CAP=128U<<20;
struct Census {U nodes=0,ranges=0,read=0,write=0,edges=0,max_nodes=0,max_ranges=0;};
struct Model {
 std::string key,family;int warps,resident,ctas;J call,metadata,recipe;
 std::vector<std::unique_ptr<ControlClass>>classes;
 std::vector<int>class_for_cta;std::vector<g::CtaGraphStore::Span>spans;
 U template_nodes=0,modeled_nodes=0,address_checks=0;std::string generated_address_sha256;
 Model(const J&t,const J&expected):key(t.at("key")),family(t.at("family")),call(t.at("call")),recipe(t.at("address_recipe")){
  need(tiny_sha::sha256(t.dump())==expected.at("call_sha256").get<std::string>(),"sealed complete helper class call");
  need(std::set<std::string>{"HelperBasic","HelperStateIndex","HelperCastIndex","HelperScan","HelperCastIndexB","HelperReduceArgmax"}.count(family),"registered helper provider only");
  need(t.at("native_target_qualified")==false&&t.at("implicit_register_dependencies_complete")==false,"helper structural estimate only");
  U grid=1,threads=1;need(call.at("grid").size()==3&&call.at("block").size()==3,"three-dimensional native geometry");
  for(auto v:call.at("grid")){U q=integer(v,10000);need(q>0&&grid<=10000/q,"bounded positive helper grid");grid*=q;}
  for(auto v:call.at("block")){U q=integer(v,1024);need(q>0&&threads<=1024/q,"bounded positive native block");threads*=q;}
  ctas=int(grid);warps=int(integer(t.at("warps"),32));resident=int(integer(t.at("resources").at("max_active_blocks_per_sm"),32));
  need(threads==integer(t.at("resources").at("threads_per_cta"))&&warps==int((threads+31)/32)&&resident>0&&grid==integer(t.at("grid_CTAs")),"actual helper block/resources");
  class_for_cta.assign(ctas,-1);need(!t.at("classes").empty()&&t.at("classes").size()<=U(ctas),"bounded nonempty helper classes");std::set<std::string>ids;
  for(const auto&cl:t.at("classes")){
   std::string id=cl.at("class_id");need(!id.empty()&&ids.insert(id).second,"unique control class");int ci=int(classes.size()),rep=int(integer(cl.at("representative_cta"),ctas-1));bool found=false;int last=-1;
   for(auto v:cl.at("member_ctas")){int c=int(integer(v,ctas-1));need(c>last&&class_for_cta[c]==-1,"sorted disjoint control class members");last=c;class_for_cta[c]=ci;found|=c==rep;}need(found,"representative in control class");
   J merged=cl;for(auto i:t.items())if(i.key()!="classes")merged[i.key()]=i.value();classes.push_back(std::make_unique<ControlClass>(merged));template_nodes+=classes.back()->nodes.size();
  }
  for(int c=0;c<ctas;++c){need(class_for_cta[c]>=0,"entire native grid class coverage");U n=at(c).nodes.size();need(modeled_nodes+n<=INT_MAX,"graph ID capacity");spans.push_back({int(modeled_nodes),int(n)});modeled_nodes+=n;}
  need(template_nodes==integer(t.at("template_node_count"))&&modeled_nodes==integer(t.at("modeled_node_count")),"class and complete-grid node census");
  if(recipe.is_null())need(ctas==1&&classes.size()==1&&family!="HelperCastIndexB","singleton exact-address domain");
  else {
   need(family=="HelperCastIndexB"&&recipe.at("schema")=="B9_ADDRESS_RECIPE_V1"&&recipe.at("family")==call.at("family"),"sealed B9 recipe family");std::string f=recipe.at("family");
   need(std::set<std::string>{"EmbeddingPrefill","EmbeddingDecode","CastLogits","GatherLastHidden","RequestTokenIndex"}.count(f),"closed B9 semantic recipe");
   need(warps==4&&threads==128,"B9 actual128-thread geometry");
   if(f=="RequestTokenIndex")need(recipe.at("request_indices")==J::array({0})&&recipe.at("positions").size()==1&&ctas==1,"observed request row zero premise");else need(recipe.at("request_indices").is_null(),"request row premise limited to RequestTokenIndex");
   if(f=="EmbeddingPrefill")need(ctas==384&&recipe.at("input_ids").size()==32,"fixed Prefill embedding token extent");
   if(f=="EmbeddingDecode")need(ctas==32&&recipe.at("input_ids").size()==1,"fixed Decode embedding token extent");
   if(f=="CastLogits")need(ctas==251,"fixed logits vector domain");if(f=="GatherLastHidden")need(ctas==8,"fixed gather grid");
  }
  metadata=t;metadata.erase("classes");tiny_sha::Sha256 digest;
  for(int c=0;c<ctas;++c)for(const auto&n:at(c).nodes)if(n.kind=="global")for(const auto&r:n.global){auto got=range(c,n,r);for(U v:{U(c),U(n.warp),n.memory_ordinal,U(r.lane),got.address,got.width}){std::array<char,8>b{};for(int z=0;z<8;++z)b[z]=char(v>>(8*z));digest.add(b.data(),8);}++address_checks;}
  // Representatives must reproduce every originally sealed lane address, including scalar duplicates.
  for(const auto&cl:t.at("classes")){int c=int(integer(cl.at("representative_cta")));for(const auto&n:at(c).nodes)for(const auto&r:n.global){auto got=range(c,n,r);need(got.address==r.address&&got.width==r.width&&got.lane==r.lane,"recipe reproduces exact sealed representative address");}}
  generated_address_sha256=digest.hex();metadata=t;metadata.erase("classes");
 }
 const ControlClass&at(int c)const{need(c>=0&&c<ctas,"helper CTA in native grid");return *classes.at(class_for_cta.at(c));}
 Census census(U count)const{need(count>0&&count<=U(ctas),"bounded helper selected CTA count");Census s;for(U c=0;c<count;++c){const auto&t=at(int(c));s.nodes+=t.nodes.size();s.ranges+=t.ranges;s.read+=t.read;s.write+=t.write;s.edges+=t.edges;s.max_nodes=std::max(s.max_nodes,U(t.nodes.size()));s.max_ranges=std::max(s.max_ranges,t.ranges);}return s;}
 Range range(int c,const Node&n,const Range&r)const{
  Range out=r;if(!recipe.is_null()){
   const std::string f=recipe.at("family");const U tid=32*n.warp+r.lane,ord=n.memory_ordinal;U offset=0,width=0,pc=0;std::string role,op;
   if(f=="EmbeddingPrefill"){
    U loops=(131071-128*U(c))/49152+1;need(ord<3*loops,"Prefill per-CTA global ordinal");U e=128*U(c)+tid+(ord/3)*49152;need(e<131072,"Prefill embedding element domain");
    if(ord%3==0){role="indices";offset=e/4096*8;pc=0x220;width=4;op="R";}else if(ord%3==1){role="weight";offset=integer(recipe.at("input_ids").at(e/4096),128255)*8192+e%4096*2;pc=0x660;width=2;op="R";}else{role="output";offset=e*2;pc=0x830;width=2;op="W";}
   }else if(f=="EmbeddingDecode"){
    need(ord<3,"Decode embedding global ordinal");U e=128*U(c)+tid;
    if(ord==0){role="indices";pc=0x110;width=4;op="R";}else if(ord==1){role="weight";offset=integer(recipe.at("input_ids").at(0),128255)*8192+e*2;pc=0x560;width=2;op="R";}else{role="output";offset=e*2;pc=0x690;width=2;op="W";}
   }else if(f=="CastLogits"){
    U rounds=c==250?2:4;need(ord<2*rounds,"logits tail-aware global ordinal");U round=ord%rounds,e=512*U(c)+128*round+tid;need(e<128256,"logits actual tensor domain");
    const U loadpc[]={0x830,0x1640,0x2450,0x3250},storepc[]={0x3bd0,0x4a60,0x5900,0x67a0};bool load=ord<rounds;role=load?"input":"output";width=load?2:4;offset=e*width;pc=load?loadpc[round]:storepc[round];op=load?"R":"W";
   }else if(f=="GatherLastHidden"){
    need(ord<12,"gather global ordinal");U round=ord/3,e=512*U(c)+128*round+tid;const U pcs[][3]={{0x1ea0,0x2750,0x27a0},{0x45d0,0x4e90,0x4ed0},{0x6d00,0x75c0,0x7600},{0x9400,0x9cc0,0x9ce0}};pc=pcs[round][ord%3];
    if(ord%3==0){role="indices";width=8;op="R";}else if(ord%3==1){role="input";offset=31*8192+e*2;width=2;op="R";}else{role="output";offset=e*2;width=2;op="W";}
   }else if(f=="RequestTokenIndex"){
    need(c==0&&n.warp==0&&r.lane==0&&ord<4&&n.effective==1&&recipe.at("request_indices")==J::array({0}),"scalar fixed request row zero");const char*roles[]={"request_index","position_index","value_input","destination"};const U pcs[]={0x1ea0,0x21a0,0x2730,0x27a0};role=roles[ord];pc=pcs[ord];width=ord<2?8:4;op=ord==3?"W":"R";if(ord==3)offset=integer(recipe.at("positions").at(0),8195)*4;
   }else need(false,"unknown B9 address recipe");
   need(n.pc==int(pc)&&n.width==width&&r.width==width&&n.memop==op&&(f=="RequestTokenIndex"||n.effective==UINT32_MAX),"exact recipe PC/op/width/mask");out.address=add(integer(recipe.at("roles").at(role)),offset);
  }
  need(out.address<=UINT64_MAX-out.width,"request address overflow");bool found=false;for(const auto&q:metadata.at("service_ranges"))found|=integer(q[0])<=out.address&&out.address+out.width<=integer(q[1]);need(found,"whole generated request in current-call service window");
  return out;
 }
};
struct Catalog {
 std::map<std::string,std::unique_ptr<Model>>models;
 Catalog(const std::string&raw,const J&seal){
  need(raw.size()==integer(seal.at("bytes"),FRAME_CAP)&&tiny_sha::sha256(raw)==seal.at("sha256").get<std::string>(),"exact bounded helper class frame");J e=frame_bridge::parse(raw);
  need(e.at("schema")=="HELPER_CLASS_RUNTIME_BUNDLE_V2"&&e.at("calls").size()==seal.at("calls").size()&&e.at("target_process")==J({{"pid",1925216},{"start_ticks",857625100}})&&e.at("native_target_qualified")==false,"closed helper class package");
  for(const auto&t:e.at("calls")){std::string k=t.at("key");try{need(models.emplace(k,std::make_unique<Model>(t,seal.at("calls").at(k))).second,"unique helper target");}catch(const std::exception&x){throw std::runtime_error("Helper class call "+k+": "+x.what());}}
 }
 const Model&at(const std::string&k)const{return *models.at(k);}
 J receipt()const{J rows=J::array();U classes=0,templates=0,nodes=0,ctas=0,addresses=0;for(const auto&i:models){const auto&m=*i.second;auto n=m.census(m.ctas);classes+=m.classes.size();templates+=m.template_nodes;nodes+=n.nodes;ctas+=m.ctas;addresses+=m.address_checks;rows.push_back({{"key",m.key},{"family",m.family},{"classes",m.classes.size()},{"CTAs",m.ctas},{"template_nodes",m.template_nodes},{"modeled_nodes",n.nodes},{"requested_read_bytes",n.read},{"requested_write_bytes",n.write},{"generated_address_checks",m.address_checks},{"generated_address_sha256",m.generated_address_sha256},{"secondary_control_classes",[&]{J cs=J::array();for(const auto&c:m.classes)cs.push_back({{"node_sha256",c->metadata.at("node_sha256")},{"secondary_control_nodes",c->metadata.at("secondary_control_nodes")},{"secondary_control_metadata_sha256",c->metadata.at("secondary_control_metadata_sha256")}});return cs;}()}});}return {{"calls",models.size()},{"classes",classes},{"template_nodes",templates},{"modeled_nodes",nodes},{"CTAs",ctas},{"generated_address_checks",addresses},{"entries",rows},{"expanded_graph_saved",false}};}
};
struct Builder {
 const Model&m;bool fast_hash=false,merge_ranges=false;int kernel_index=0;U built_nodes=0,retired_nodes=0,retired_ctas=0,completion_edges=0,issue_edges=0,read=0,write=0;std::vector<g::CtaGraphStore::Span>spans;std::vector<std::string>retired_semantics,retired_kernel_semantics;std::vector<bool>built_ctas,done_ctas;tiny_sha::Sha256 addresses;Census expected;
 Builder(const Model&v,U count):m(v),spans(v.spans.begin(),v.spans.begin()+count),retired_semantics(count),retired_kernel_semantics(count),built_ctas(count),done_ctas(count),expected(v.census(count)){}
 void word(U v){std::array<char,8>b{};for(int i=0;i<8;++i)b[i]=char(v>>(8*i));addresses.add(b.data(),b.size());}
 g::CtaGraphStore::Owned build(int c){
  need(c>=0&&U(c)<spans.size()&&!built_ctas[c],"one materialization of selected helper CTA");built_ctas[c]=true;host_bound();const auto&model=m.at(c);const int first=spans[c].first_node;g::CtaGraphStore::Owned out;out.reserve(model.nodes.size());
  for(U i=0;i<model.nodes.size();++i){const auto&t=model.nodes[i];std::string pipe=t.pipe,op="compute";if(t.kind=="global"){pipe=t.memop=="R"?"LD":"ST";op=t.memop=="R"?"ld.dram2reg":"st.reg2dram";}else if(t.kind=="shared"){pipe=t.memop=="R"?"LD":"ST";op=t.memop=="R"?"ld.sram2reg":"st.reg2sram";}else if(t.kind=="barrier"){pipe="BARRIER";op="barrier";}
   std::vector<int>deps,issues;for(int d:t.deps)deps.push_back(first+d);for(int d:t.issues)issues.push_back(first+d);
   auto n=std::make_unique<g::DAGNode>(first+int(i),"helper."+std::to_string(first+i),pipe,op,g::cta_placement::token(c,t.warp,m.warps,48,4,true),deps,0,g::Tile(0,0,1,t.elements),g::DataType::FP32);n->sm_id=c%48;n->thread_block_id=c;n->compiler_cta_id=c;n->source_ordinal=t.old;n->graph_node_id=std::to_string(t.warp)+":"+std::to_string(t.ordinal)+":"+std::to_string(t.pc);n->semantic_role=t.opcode;n->matrix_id=1;n->issue_depends_on=issues;
   if(t.kind=="global"||t.kind=="shared"){g::ExplicitMemorySubop sub;for(const auto&original:t.kind=="global"?t.global:t.shared){Range r=t.kind=="global"?m.range(c,t,original):original;sub.ranges.push_back({r.lane,r.address,r.width});sub.source_member_ordinals.push_back(r.lane);sub.requested_bytes+=r.width;if(t.kind=="global"){word(c);word(first+i);word(r.lane);word(r.address);word(r.width);}}n->explicit_memory_subops.push_back(std::move(sub));n->memory_access_granularity_bytes=int(t.width);if(t.kind=="global")n->memory_coalesce_bytes=128;else n->explicit_sram_bank_service_v1=true;}
   out.push_back(std::move(n));++built_nodes;
  }return out;
 }
 void retire(int c,const std::vector<g::DAGNode*>&ns,g::Cycle cy){
  need(c>=0&&U(c)<spans.size()&&built_ctas[c]&&!done_ctas[c],"one selected helper CTA retirement");done_ctas[c]=true;const auto&model=m.at(c);const int first=spans[c].first_node;need(ns.size()==model.nodes.size(),"complete helper class retirement");tiny_sha::Sha256 h;U local_read=0,local_write=0,local_edges=0;
  for(U i=0;i<ns.size();++i){const auto*n=ns[i];const auto&t=model.nodes[i];need(n->id==first+int(i)&&n->finished&&n->issue_done&&n->issue_deps_resolved&&n->remaining_deps==0&&n->pending_transactions==(n->op_type==g::OpType::BARRIER?1:0)&&n->end<=cy,"helper whole node completion");
   for(int d:n->depends_on){need(d>=first&&d<first+int(i)&&n->start>=ns.at(d-first)->end+1,"helper completion edge timing");++completion_edges;++local_edges;}for(int d:n->issue_depends_on){need(d>=first&&d<first+int(i)&&n->start>=ns.at(d-first)->issue_cycle+1,"helper issue edge timing");++issue_edges;++local_edges;}
   for(const auto&r:t.global){if(t.memop=="R")local_read+=r.width;else local_write+=r.width;}std::array<g::Cycle,13>v{{n->id,t.old,t.warp,t.ordinal,t.pc,n->start,n->end,n->issue_cycle,n->ready_cycle,n->total_transactions,n->next_transaction_index,n->pending_transactions,n->remaining_deps}};for(auto x:v){std::array<char,8>b{};for(int z=0;z<8;++z)b[z]=char(U(x)>>(8*z));h.add(b.data(),8);}++retired_nodes;
  }
  need(local_read==model.read&&local_write==model.write&&local_edges==model.edges,"per-class traffic and typed edges closed");read+=local_read;write+=local_write;retired_semantics[c]=h.hex();tiny_sha::Sha256 kh;kh.add("kernel="+std::to_string(kernel_index)+":"+h.hex()+"\n");retired_kernel_semantics[c]=kh.hex();++retired_ctas;
  if(retired_ctas==spans.size())need(read==expected.read&&write==expected.write&&completion_edges+issue_edges==expected.edges&&retired_nodes==expected.nodes,"selected helper CTA-prefix byte and dependency closure");
 }
 J receipt()const{return {{"built_nodes",built_nodes},{"retired_nodes",retired_nodes},{"completion_edges_checked",completion_edges},{"issue_edges_checked",issue_edges},{"requested_read_bytes",read},{"requested_write_bytes",write},{"control_class_templates",m.classes.size()},{"full_grid_CTAs",m.ctas},{"selected_CTAs",spans.size()},{"known_implicit_dependencies_complete",false},{"actual_array_extent_qualified",false},{"indexed_constant_values_observed",false},{"B9_control_is_structural_estimate",!m.recipe.is_null()},{"full_graph_instantiated_without_EXIT_pruning",true}};}
};
}
