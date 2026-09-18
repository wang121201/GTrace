#pragma once
#include "memory.h"
#include "native_memory_program.h"
namespace native_sequence {
namespace g=GTSim;namespace p=native_program;using J=p::J;using U=p::U;
J stats(const g::L2RuntimeStatistics& s){J j;
#define S(x) j[#x]=s.x
 S(pre_l1_transactions);S(pre_l1_reads);S(pre_l1_writes);S(l1_bypassed_transactions);S(l1_read_hits);S(l1_read_misses);S(l1_write_hits);S(l1_write_misses);S(l1_filtered_reads);S(l1_evictions);S(l1_kernel_flushes);S(l1_flushed_lines);S(accepted_transactions);S(accepted_reads);S(accepted_writes);S(processed_transactions);S(processed_reads);S(processed_writes);S(read_hits);S(write_hits);S(read_miss_allocates);S(write_miss_allocates);S(read_pending_fill_merges);S(write_pending_fill_merges);S(fill_completions);S(cache_inserts);S(clean_evictions);S(dirty_evictions);S(dram_fill_bytes);S(dram_writeback_bytes);S(dram_fill_completed_bytes);S(dram_writeback_completed_bytes);S(dram_fill_requests);S(dram_fill_completions);S(dram_writeback_requests);S(dram_writeback_completions);
#undef S
 return j;}
J gauges(const g::L2RuntimeStatistics& s){return {{"L1_resident_lines",s.final_l1_resident_lines},{"L2_resident_lines",s.final_resident_lines},{"read_queue",s.final_l2_read_queue},{"write_queue",s.final_l2_write_queue},{"completion_queue",s.final_completion_queue},{"MSHRs",s.final_mshr_entries},{"DRAM_queue",s.final_dram_queue},{"peak_read_queue_cumulative",s.peak_l2_read_queue},{"peak_write_queue_cumulative",s.peak_l2_write_queue},{"peak_MSHRs_cumulative",s.peak_mshr_entries},{"peak_DRAM_queue_cumulative",s.peak_dram_queue}};}
J delta(const J& after,const J& before){J d;for(auto i=after.begin();i!=after.end();++i){p::need(i.value().get<U>()>=before.at(i.key()).get<U>(),"cumulative counter reversed");d[i.key()]=i.value().get<U>()-before.at(i.key()).get<U>();}return d;}

}
