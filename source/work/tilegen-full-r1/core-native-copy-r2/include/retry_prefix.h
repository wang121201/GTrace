#pragma once
#include "cycle.h"
#include "retry_host_memo.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
namespace GTSim::retry_host {
struct PrefixStamp {
 const void* cache=nullptr;std::uint64_t domain=0,promotion=0,membership=0;int sm=-1;bool allowed=false;
 bool operator==(const PrefixStamp&)const=default;
};
// Only previously READY, validated, negative reads are certified. Future
// entries stop certification; append creates an uncertified suffix.
class NegativePrefix {
 std::size_t count_=0;PrefixStamp stamp_{};Cycle certified_at_=0;
public:
 std::size_t size()const{return count_;}
 void clear(){count_=0;stamp_={};}
 template<class Queue,class GetStamp,class Blocked,class Attempt>
 void run(Queue& queue,Cycle now,GetStamp stamp,Blocked blocked,Attempt attempt){
  const auto before=stamp();const bool active=prefix_enabled&&before.allowed;
  if(!active || now<certified_at_ || !(before==stamp_) || count_>queue.size()){
   if(count_)++counts.prefix_stamp_invalidations;
   clear();
  }
  std::size_t left=count_,skip=0,start=0,new_count=0;bool extend=active;
  // A valid prefix has no future entry. The original helper still handles
  // every admissible front and is the only source of accepted events.
  while(left){
   if(!(stamp()==before)){clear();left=0;extend=false;break;}
   if(blocked()){
    skip=left;new_count=left;start=left;counts.prefix_entries_skipped+=left;++counts.prefix_blocked_passes;break;
   }
   ++counts.prefix_front_attempts;
   const auto decision=attempt(queue.front());
   if(decision.first){queue.pop_front();--left;continue;}
   // Unexpected failure: this front was already attempted, so don't retry
   // it twice. Scan all later entries in original order without certification.
   start=1;new_count=decision.second?1:0;extend=active&&decision.second;left=0;break;
  }
  // A stable remove_if over only the uncertified suffix leaves the skipped
  // prefix intact and preserves the original order of every survivor.
  auto begin=queue.begin()+static_cast<typename Queue::difference_type>(start);
  const auto end=std::remove_if(begin,queue.end(),[&](auto& entry){
   const auto decision=attempt(entry);
   if(!decision.first){if(extend&&decision.second)++new_count;else extend=false;}
   return decision.first;
  });
  queue.erase(end,queue.end());
  if(active && stamp()==before){count_=new_count;stamp_=before;certified_at_=now;++counts.prefix_certification_passes;}
  else clear();
  (void)skip;
 }
};
} // namespace GTSim::retry_host
