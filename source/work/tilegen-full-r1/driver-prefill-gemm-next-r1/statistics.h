#pragma once
// Exact statistics field list from the frozen native-copy core probe.
J statistics(const g::L2RuntimeStatistics& s) {
 J j;
#define S(f) j[#f]=s.f
 S(pre_l1_transactions);S(pre_l1_reads);S(pre_l1_writes);S(l1_bypassed_transactions);S(l1_read_hits);S(l1_read_misses);S(l1_write_hits);S(l1_write_misses);S(l1_filtered_reads);S(l1_evictions);S(l1_kernel_flushes);S(l1_flushed_lines);S(peak_l1_resident_lines);S(final_l1_resident_lines);S(l1_decision_order_fnv1a64);
 S(accepted_transactions);S(accepted_reads);S(accepted_writes);S(processed_transactions);S(processed_reads);S(processed_writes);S(read_hits);S(write_hits);S(read_miss_allocates);S(write_miss_allocates);S(read_pending_fill_merges);S(write_pending_fill_merges);S(fill_completions);S(cache_inserts);S(clean_evictions);S(dirty_evictions);S(dram_fill_requests);S(dram_fill_completions);S(dram_writeback_requests);S(dram_writeback_completions);S(peak_l2_read_queue);S(peak_l2_write_queue);S(peak_l2_combined_queue);S(peak_completion_queue);S(peak_mshr_entries);S(peak_resident_lines);S(peak_dram_queue);S(final_l2_read_queue);S(final_l2_write_queue);S(final_completion_queue);S(final_mshr_entries);S(final_resident_lines);S(final_dram_queue);S(decision_order_fnv1a64);S(fill_order_fnv1a64);
#undef S
 return j;
}
