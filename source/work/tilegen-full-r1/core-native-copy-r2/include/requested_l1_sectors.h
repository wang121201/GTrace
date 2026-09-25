#pragma once
#include "dag_node.h"
#include <cstdint>
#include <span>
#include <stdexcept>
namespace GTSim::requested_l1_sectors {
using U=std::uint64_t;
constexpr U unknown=~U{0};
inline void need(bool v,const char*why){if(!v)throw std::invalid_argument(why);}
struct Coverage {bool known=false;std::uint8_t mask=0;unsigned reason=1;};
// Reasons: 0 exact; 1 no explicit byte ranges; 2 unsupported coalescing;
// 3 ambiguous subop; 4 nonidentity native/cache key. No source-hint inference.
inline Coverage clip(int source_matrix,int coalesce,std::span<const ExplicitMemorySubop>subops,
 int index,int native_matrix,U line,int line_bytes=128){
 if(line_bytes!=128||coalesce!=128)return {false,{},2};
 if(subops.empty())return {};
 need(native_matrix>=0&&native_matrix==source_matrix&&line%128==0,"coverage native key");
 if(index==-1){if(subops.size()!=1)return {false,{},3};index=0;}
 need(index>=0&&std::size_t(index)<subops.size(),"coverage exact subop index");
 const auto&sub=subops[std::size_t(index)];if(sub.ranges.empty())return {};
 std::uint8_t mask=0;U requested=0;const U last=line+127;
 for(const auto&r:sub.ranges){
  need(r.byte_count&&r.offset_bytes<=unknown-(r.byte_count-1)&&requested<=unknown-r.byte_count,
       "coverage range/payload overflow");requested+=r.byte_count;
  const U end=r.offset_bytes+r.byte_count-1;
  if(r.offset_bytes>last||end<line)continue;
  const U first=r.offset_bytes>line?r.offset_bytes:line, stop=end<last?end:last;
  for(unsigned sector=unsigned((first-line)/32);sector<=unsigned((stop-line)/32);++sector)mask|=std::uint8_t(1u<<sector);
 }
 need(requested==sub.requested_bytes&&mask!=0,"coverage payload or touched line mismatch");
 return {true,mask,0};
}
}
