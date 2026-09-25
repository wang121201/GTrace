#pragma once
// Original Fine scheduler for the smaller families, borrowing the same cache,
// native backend and monotonic clock as Tiny. Failure is fail-stop, not rollback.
namespace current_dispatch {
template<class Device,class SourceWork>
J fine_kernel(Device&d,g::CtaGraphStore&store,U id,const J&expected,unsigned resident,
              SourceWork source_work,g::L2RuntimeObserver*observer){
 need(d.cache.is_quiescent()&&d.backend.queue_depth()==0,"Fine starts after actual preceding completion");
 auto start=tiny_full::HostClock::now();auto dirty_before=d.cache.dirty_sector_snapshot();auto physical_before=d.backend.physical();
 g::Cycle cycle=g::checked_cycle(d.now);hybrid_full::FineContext fine(d.cfg,d.cache,cycle);fine.set_next_kernel_resident_cta_limit(resident);
 g::DAG dag;dag.cta_graph_store=&store;g::scheduler_observer::set_enabled(false);tiny_runtime::sp_event_mode=true;
 auto run=fine.run_kernel(&dag,2000000000,false,observer,nullptr,10000000);d.now=U(cycle);
 need(fine.is_quiescent()&&run.quiescent_end_cycle==cycle&&d.backend.queue_depth()==0,"Fine actual backend quiescence");
 auto storage=store.telemetry();J actual=source_work();
 for(auto key:{"CTAs","nodes","requested_read_bytes","requested_write_bytes"})need(actual.at(key)==expected.at(key),"Fine original actual-retirement work census");
 need(storage.at("retired_ctas")==expected.at("CTAs")&&storage.at("retired_nodes")==expected.at("nodes")&&storage.at("live_nodes")==0&&storage.at("live_ctas")==0,"Fine source graph retirement and reclamation");
 const auto&s=d.cache.runtime_statistics();need(s.accepted_transactions==s.processed_transactions&&s.dram_fill_requests==s.dram_fill_completions&&s.dram_writeback_requests==s.dram_writeback_completions&&d.cache.per_sm_l1_readiness().live_tickets==0,"Fine all cache/L1 callbacks close");
 auto dirty=d.cache.dirty_sector_snapshot();need(dirty.dirty_sector_ledger_closed&&dirty.writeback_byte_ledger_closed&&dirty_before.resident_dirty_sectors+dirty.dirty_sector_creations-dirty_before.dirty_sector_creations==dirty.evicted_dirty_sectors-dirty_before.evicted_dirty_sectors+dirty.resident_dirty_sectors,"Fine operation dirty I+C=E+F");
 auto delta=tiny_full::delta(tiny_full::stats(run.after),tiny_full::stats(run.before));auto physical=d.backend.physical();
 need(physical.at("read_bytes").template get<U>()-physical_before.at("read_bytes").template get<U>()==delta.at("dram_fill_bytes")&&physical.at("write_bytes").template get<U>()-physical_before.at("write_bytes").template get<U>()==delta.at("dram_writeback_bytes"),"Fine physical/cache byte accounting");
 // FineContext clears its observer on exit. Restore the caller's continuous
 // observer only after its own work has drained; never preserve a stale binding.
 d.cache.set_runtime_observer(observer);
 return {{"native_launch_id",id},{"execution_route","Fine"},{"CTAs",actual.at("CTAs")},{"nodes",actual.at("nodes")},{"start_cycle",run.start_cycle},{"kernel_end_cycle",run.kernel_end_cycle},{"quiescent_end_cycle",run.quiescent_end_cycle},{"counter_delta",delta},{"actual_source_work",actual},{"resident_graph",storage},{"source_resident_limit",resident},{"current_occupancy_observed",false},{"global_dirty",{{"I",dirty_before.resident_dirty_sectors},{"C",dirty.dirty_sector_creations-dirty_before.dirty_sector_creations},{"E",dirty.evicted_dirty_sectors-dirty_before.evicted_dirty_sectors},{"F",dirty.resident_dirty_sectors}}},{"physical_before",physical_before},{"physical_after",physical},{"host_seconds",tiny_full::elapsed(start)}};
}
}
