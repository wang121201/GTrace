#pragma once
// Use only with one consistently rebuilt isolated writer_observer.h definition.
#include <cstddef>
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/tilegen-full-r1/core-native-copy-r2/include/writer_observer.h"
namespace current_history_write {
template<class Cache> class Session {
 Cache& cache_;
 int count_,next_=0;
 bool closed_=false;
 decode_writer::Observation observation_;
 decode_writer::Scope scope_;
 static int checked(std::size_t count) {
  decode_writer::need(count>0&&count<=3020,"current timeline exceeds sealed attribution domain");
  return static_cast<int>(count);
 }
public:
 Session(Cache& cache,std::size_t selected_timeline_nodes)
  :cache_(cache),count_(checked(selected_timeline_nodes)),
   observation_(0,count_-1,false,nullptr,3020),scope_(observation_,&cache_) {
  decode_writer::need(cache_.is_quiescent(),"history attribution requires quiescent entry");
  cache_.decode_writer_begin();
 }
 // Call exactly once before EVERY selected timeline node, including metadata.
 // Keep this label throughout kernel/API drain; do not advance on enqueue alone.
 void set_operation(std::size_t index) {
  decode_writer::need(!closed_&&index==static_cast<std::size_t>(next_)&&next_<count_&&cache_.is_quiescent(),"history operation identity/order/quiescence");
  observation_.set_call(next_++);
 }
 void finish() {
  decode_writer::need(!closed_&&next_==count_&&cache_.is_quiescent(),"history attribution incomplete/nonquiescent exit");
  cache_.decode_writer_end();closed_=true;
 }
 template<class J> J report()const{return observation_.template report<J>();}
 Session(const Session&)=delete;
 Session& operator=(const Session&)=delete;
};
} // namespace current_history_write
