#pragma once
#include "../canonical-prefillcopy-driver-r1/modeled_builder.h"
namespace canonical_prefillcopy {
struct ModeledPrepared {
 const SourceBundle& bundle;const p::Program& program;const Registers& registers;const J& call;U executed;int resident=12;g::CtaGraphStore::Limits limits;J budget;
 ModeledPrepared(const SourceBundle& s,const J& c,bool eager):bundle(s),program(s.program),registers(s.registers),call(c),executed(p::natural(c.at("grid")[0],256)){
  limits.max_live_nodes=5000000;const U ranges=32,frontier=std::min(executed,U(1152)),nodes=p::multiply(frontier,registers.nodes.size());
  p::need(nodes<=limits.max_live_nodes&&ranges<=limits.max_explicit_ranges_per_cta&&p::multiply(frontier,ranges)<=limits.max_live_explicit_ranges,"bounded target residency");if(eager)p::need(p::multiply(executed,registers.nodes.size())<=200000,"bounded eager reference");
  budget={{"nodes_per_cta",registers.nodes.size()},{"stored_ranges_per_cta",ranges},{"conservative_live_ctas",frontier},{"conservative_live_nodes",nodes},{"conservative_live_ranges",frontier*ranges},{"native_resource_cap",resident},{"live_node_limit",limits.max_live_nodes}};
 }
};
}
namespace canonical_mixed611 {
inline native_sequence::J copy_pool_receipt(const canonical_prefillcopy::CopyCatalog&catalog){
 using namespace native_sequence;J r=catalog.receipt();U bytes=0,records=0,lanes=0,nodes=0,edges=0;
 for(const auto&path:catalog.seals.retained_sources)bytes=p::add(bytes,p::natural(catalog.seals.pins.at(path).at("bytes")));
 for(const auto&item:catalog.bundles){const auto&b=*item.second;nodes=p::add(nodes,b.registers.nodes.size());for(const auto&n:b.registers.nodes)edges=p::add(edges,n.completion.size()+n.issue.size());for(const auto&body:b.program.bodies)for(const auto&record:body.records){records=p::add(records,1);lanes=p::add(lanes,record.lanes.size());}}
 r["encoded_source_bytes"]=bytes;r["memory_records"]=records;r["memory_lane_tuples"]=lanes;r["register_template_nodes"]=nodes;r["typed_register_dependency_edges"]=edges;r["oracle_validations"]=catalog.bundles.size();r["source_bytes_are_RSS"]=false;return r;
}
}
