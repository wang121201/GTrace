#pragma once
#include "memory.h"
#include "sha256.h"
#include <array>
// Optional audit only: no expanded event stream is retained or serialized.
namespace host_range_audit {
struct L2Events final:GTSim::L2RuntimeObserver {
 tiny_sha::Sha256 digest;std::uint64_t decisions=0,fills=0;
 template<std::size_t N> void add(const std::array<std::uint64_t,N>& values){
  std::array<char,N*8> bytes{};for(std::size_t i=0;i<N;++i)for(unsigned b=0;b<8;++b)bytes[8*i+b]=char(values[i]>>(8*b));
  digest.add(bytes.data(),bytes.size());
 }
 void on_l2_decision(const GTSim::L2DecisionEvent& e)override {++decisions;add(std::array<std::uint64_t,18>{{
  1,e.decision_sequence,e.accept_sequence,e.accept_cycle,e.decision_cycle,std::uint64_t(e.node_id),std::uint64_t(e.matrix_id),e.line_addr,e.fill_sequence,std::uint64_t(e.is_write),std::uint64_t(e.outcome),e.read_queue_depth_after_accept,e.write_queue_depth_after_accept,e.mshr_entries_after_decision,e.resident_lines_after_decision,e.dram_queue_depth_after_decision,std::uint64_t(e.native_matrix_id),e.native_line_addr}});}
 void on_l2_fill(const GTSim::L2FillEvent& e)override {++fills;add(std::array<std::uint64_t,17>{{
  2,e.fill_sequence,e.allocate_decision_sequence,e.allocate_cycle,e.complete_cycle,std::uint64_t(e.matrix_id),e.line_addr,e.waiter_count,std::uint64_t(e.any_read),std::uint64_t(e.any_write),std::uint64_t(e.inserted),std::uint64_t(e.eviction_kind),std::uint64_t(e.victim_matrix_id),e.victim_line_addr,e.resident_lines_after_completion,e.mshr_entries_after_completion,e.dram_queue_depth_after_completion}});}
 nlohmann::json receipt(){return {{"schema","ORDERED_NATIVE_L2_CALLBACK_SHA256_V1"},{"encoding","event tag plus all declared fields in declaration order, fixed little-endian u64; signed fields bit-preserved"},{"decision_events",decisions},{"fill_events",fills},{"sha256",digest.hex()},{"scope","L2 decisions and fills in callback order; L1-filtered acks are covered by identical ordered subop inputs plus per-node times, not separately callback-observed"},{"expanded_events_saved",false}};}
};
}
