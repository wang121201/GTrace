#define TILEGEN_NO_EXECUTABLE_MAIN
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/tilegen-full-r1/canonical-full-runtime-r4/streaming.cpp"
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-loader-r3/catalog.h"
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-execution-r1/tiny_dispatch-r2.h"
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-execution-r1/fine_dispatch.h"
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-execution-r1/api_port-r2.h"
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-write-attribution-r1/history_attribution.h"
#include "/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/ada-cosim-alignment-20260922-r1/structure-adapter/config.h"
namespace history_case {
using namespace native_sequence;U checks=0;
void need(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
J read(const std::string&p){std::ifstream f(p);need(bool(f),"input");J j;f>>j;return j;}
struct Backend final:g::L2DramCompletionBackend {
 sg_hbf::Clock clock;sg_hbf::MemoryPathBackend inner;U polls=0;
 explicit Backend(const J&p):clock(p.at("clock").at("period_ps_numerator"),p.at("clock").at("period_ps_denominator")),inner(clock,sg_hbf::native_config_file(p.at("native_hbfsim_config_file")),coupling::path_config(p.at("memory_path")),4096,32,{sg_hbf::DrainMode::Independent,1}){}
 U issue_cycle_to_ps(U c)const override{return inner.issue_cycle_to_ps(c);}
 void enqueue(const g::L2DramRequest&r)override{inner.enqueue(r);}
 bool try_enqueue(const g::L2DramRequest&r,U at)override{return inner.try_enqueue(r,at);}
 std::vector<g::L2DramCompletion>step(U at)override{++polls;return inner.step(at);}
 const g::L2DramRuntimeStatistics&statistics()const override{return inner.statistics();}
 std::size_t queue_depth()const override{return inner.queue_depth();}
 std::size_t admission_capacity()const override{return inner.admission_capacity();}
 J physical()const{auto p=inner.physical_statistics();return {{"read_bytes",p.read_bytes},{"write_bytes",p.write_bytes},{"finish_ns",p.finish_ns}};}
 J close(){inner.finalize();auto a=inner.physical_admission_statistics();auto x=inner.service_diagnostics();return {{"physical",physical()},{"accepted",a.accepted},{"completed",a.completed},{"blocked",a.blocked},{"peak_live",a.peak_live},{"reserved_bursts_at_end",a.reserved_bursts},{"native_enqueue_service_violations",a.native_enqueue_service_violations},{"physical_calls",a.physical_calls},{"independent_fast_drains",x.independent_fast_drains},{"pc_service_one_calls",x.pc_service_one_calls},{"no_trace_sink",true}};}
};

struct Device {
 g::SimulatorConfig cfg;Backend backend;g::L2Cache cache;U now=0,callbacks=0;
 static g::SimulatorConfig config(const J&p){auto c=g::make_ada_cosim_alignment_config(p.at("ada_alignment_profile").get<std::string>());c.per_sm_l1.sector_validity=true;c.silence_mode=true;c.workload_type=g::WorkloadType::Llama3Elementwise;return c;}
 Device(const J&plan,const coupling::ServiceMapper&map):cfg(config(plan)),backend(plan.at("profile")),cache(cfg.l2_cache_size_bytes,cfg.l2_line_size_bytes,cfg.l2_hit_latency_cycles,cfg.l2_bandwidth_bytes_per_cycle,cfg.l2_write_bandwidth_bytes_per_cycle,cfg.l2_queue_depth,cfg.l2_bypass_cache,0,cfg.l2_miss_penalty_cycles,cfg.core_frequency_mhz,cfg.dram_frequency_mhz,cfg.memory_model_semantics,cfg.per_sm_l1,&backend,&map,cfg.l2_geometry){need(std::abs(1e6*plan.at("profile").at("clock").at("period_ps_denominator").get<double>()/plan.at("profile").at("clock").at("period_ps_numerator").get<double>()-cfg.core_frequency_mhz)<1e-8,"same core and backend clock");cache.enable_shared_cache(288);cache.begin_kernel();}
 std::vector<int>service(U at,unsigned span,g::L2Cache::EpochServiceStatistics&s){cache.require_memory_epoch_span(g::checked_cycle(at),span,backend);U before=backend.polls;auto old=backend.step(at);auto out=cache.service_epoch(g::checked_cycle(at),span,backend,old,&s);need(backend.polls==before+1,"single backend pump");callbacks+=out.size();return out;}
};

int run(const char*input,const char*output){
 const auto start_all=tiny_full::HostClock::now();J plan=read(input);need(plan.at("schema")=="CURRENT_CONTINUOUS_HISTORY_EXECUTION_V1","fixed history execution plan");
 const std::string variant=plan.at("initialization_work");need(variant=="min"||variant=="max","explicit conditional initializer work");
 if(plan.value("full_measured",false)){
 need(!plan.at("full_history").get<bool>()&&!plan.at("complete_prefix_from_process_start").get<bool>()&&plan.at("initial_cache_state")=="EMPTY_EXPLICIT_DIAGNOSTIC","explicit cold measured entry");
 need(plan.at("counts")==J({{"native_kernel",1138},{"memory_api_submission",29},{"epoch_begin",3},{"epoch_end",3}})&&plan.at("timeline").size()==1173,"complete measured event census");
 need(plan.at("kernel_work").at("CTAs")==772512&&plan.at("kernel_work").at("nodes")==4712768255ULL,"complete measured source work");
 U id=1288;std::map<std::string,U> phases;for(const auto&e:plan.at("timeline")){need(e.at("epoch").get<int>()>=4&&e.at("epoch").get<int>()<=6,"only measured epochs");if(e.at("kind")=="native_kernel"){need(e.at("native_launch_id")==id++,"complete consecutive native measured calls");++phases[e.at("phase").get<std::string>()];}}
 need(id==2426&&J(phases)==J({{"Measured/Prefill",408},{"Measured/Decode1",365},{"Measured/Decode2",365}}),"all three measured phases");
 }
 Mapper base;auto cfg=Device::config(plan);coupling::ServiceMapper map(plan.at("service_address_map"),base,cfg.dram_size_bytes);Device d(plan,map);
 current_history_loader_v3::Options options;options.initialization_work=variant=="min"?current_history_loader_v3::InitializationWork::Min:current_history_loader_v3::InitializationWork::Max;
 current_history_loader_v3::Catalog catalog(plan.at("dispatch"),base,map,options);g::retry_host::prefix_enabled=true;g::retry_host::ready_front_enabled=true;
 current_history_write::Session<g::L2Cache> writer(d.cache,plan.at("timeline").size());
 std::string origin="none";U enters=0,leaves=0;
 current_history::ApiPort api(d.cache,d.now,[&](U at){g::L2Cache::EpochServiceStatistics stats;return d.service(at,1,stats);},[&](const J&){need(origin=="none","no stale kernel identity before API");origin="API";++enters;},[&](){need(origin=="API","matching API source scope");origin="none";++leaves;});
 std::ofstream journal(std::string(output)+".operations.jsonl");need(bool(journal),"summary-only operation journal");
 J rows=J::array();U kernels=0,apis=0,ctas=0,nodes=0,logical_read=0,logical_write=0,api_read=0,api_write=0;std::map<std::string,U>kinds;
 const double preparation=tiny_full::elapsed(start_all);auto start=tiny_full::HostClock::now();
 for(const auto&e:plan.at("timeline")){
  writer.set_operation(rows.size());auto operation_start=tiny_full::HostClock::now();U begin=d.now;need(d.cache.is_quiescent()&&d.backend.queue_depth()==0&&origin=="none","ordered operations hand off actual empty queues");const std::string kind=e.at("kind");J row;
  if(kind=="native_kernel"){
   U id=e.at("native_launch_id");const auto&entry=catalog.entry(id);need(entry.at("submission_event")==e.at("submission_event")&&entry.at("phase")==e.at("phase")&&entry.at("code_sha256")==e.at("code_sha256")&&entry.at("grid")==e.at("grid"),"exact timeline native binding");J expected=catalog.runtime_expected(id);origin="kernel";
   if(entry.at("execution_route")=="Tiny"){auto b=catalog.bindTiny(id);row=current_dispatch::tiny_kernel(d,*b,id,expected,entry.at("owner")=="gemv");}
   else{auto b=catalog.bindFine(id);auto store=b->make_store();row=current_dispatch::fine_kernel(d,*store,id,expected,b->resident(),[&](){return b->source_work();},nullptr);}
   need(d.cache.is_quiescent()&&d.backend.queue_depth()==0,"kernel closes before binding destruction");origin="none";row["owner"]=entry.at("owner");row["route"]=entry.at("execution_route");
   row["initialization_summary"]=entry.contains("selected_work_envelope");if(entry.contains("selected_work_envelope"))row["selected_work_envelope"]=entry.at("selected_work_envelope");
   ctas+=expected.at("CTAs").get<U>();nodes+=expected.at("nodes").get<U>();logical_read+=expected.at("requested_read_bytes").get<U>();logical_write+=expected.at("requested_write_bytes").get<U>();++kernels;
  }else if(kind=="memory_api_submission"){
   auto before=tiny_full::stats(d.cache.runtime_statistics());auto db=d.cache.dirty_sector_snapshot();auto pb=d.backend.physical();row=api.run(e);auto da=d.cache.dirty_sector_snapshot();auto pa=d.backend.physical();auto delta=tiny_full::delta(tiny_full::stats(d.cache.runtime_statistics()),before);
   need(db.resident_dirty_sectors+da.dirty_sector_creations-db.dirty_sector_creations==da.evicted_dirty_sectors-db.evicted_dirty_sectors+da.resident_dirty_sectors,"API dirty conservation");need(pa.at("read_bytes").get<U>()-pb.at("read_bytes").get<U>()==delta.at("dram_fill_bytes")&&pa.at("write_bytes").get<U>()-pb.at("write_bytes").get<U>()==delta.at("dram_writeback_bytes"),"API actual physical bytes");row["counter_delta"]=delta;row["global_dirty"]={{"I",db.resident_dirty_sectors},{"C",da.dirty_sector_creations-db.dirty_sector_creations},{"E",da.evicted_dirty_sectors-db.evicted_dirty_sectors},{"F",da.resident_dirty_sectors}};row["direction"]=e.at("direction");for(const auto&effect:e.at("device_effect_ranges")){if(effect.at("operation")=="WRITE")api_write+=effect.at("bytes").get<U>();else api_read+=effect.at("bytes").get<U>();}++apis;
  }else{
   need(kind=="allocation_API_observation"||kind=="epoch_begin"||kind=="epoch_end","retained metadata kind");row={{"modeled_cycle",d.now},{"cache_flush",false}};
   if(kind=="allocation_API_observation"){need(e.at("cache_flush_implied")==false&&e.at("observation").at("cuda_status")==0,"allocation remains metadata");row["observation"]=e.at("observation");}
   else{row["physical_cumulative"]=d.backend.physical();row["dirty_cumulative"]=tiny_full::dirty_stats(d.cache);}
  }
  ++kinds[kind];row["kind"]=kind;row["source_submission_event"]=e.at("submission_event");row["phase"]=e.at("phase");row["source_epoch"]=e.at("epoch");if(e.contains("layer"))row["layer"]=e.at("layer");if(e.contains("module"))row["module"]=e.at("module");row["operation_start_cycle"]=begin;row["operation_end_cycle"]=d.now;row["operation_host_seconds"]=tiny_full::elapsed(operation_start);
  rows.push_back(row);journal<<row.dump()<<'\n';journal.flush();need(bool(journal),"operation summary persisted");
  const auto progress=J({{"status","IN_PROGRESS_NOT_FINAL"},{"completed_timeline_nodes",rows.size()},{"selected_timeline_nodes",plan.at("timeline").size()},{"kernels",kernels},{"APIs",apis},{"last_submission_event",e.at("submission_event")},{"phase",e.at("phase")},{"cycles",d.now},{"host_execution_seconds",tiny_full::elapsed(start)},{"physical",d.backend.physical()}});
  std::string temporary=std::string(output)+".progress.tmp",destination=std::string(output)+".progress.json";std::ofstream(temporary)<<progress.dump(2)<<'\n';need(std::rename(temporary.c_str(),destination.c_str())==0,"atomic progress publication");
 }
 const auto&w=plan.at("kernel_work");need(kernels==plan.at("counts").at("native_kernel")&&apis==plan.at("counts").value("memory_api_submission",U(0))&&enters==apis&&leaves==apis&&ctas==w.at("CTAs")&&nodes==w.at("nodes")&&logical_read==w.at("logical_read_bytes")&&logical_write==w.at("logical_write_bytes"),"complete selected history and all source kernel work");
 need(J(kinds)==plan.at("counts")&&api_read==plan.at("API_work").at("logical_read_bytes")&&api_write==plan.at("API_work").at("logical_write_bytes"),"all metadata and API effects preserved");
 const double execution=tiny_full::elapsed(start);const auto finish_start=tiny_full::HostClock::now();writer.finish();auto attribution=writer.report<J>();auto hbf=d.backend.close();auto st=d.cache.runtime_statistics();need(hbf.at("accepted")==hbf.at("completed")&&hbf.at("reserved_bursts_at_end")==0&&hbf.at("native_enqueue_service_violations")==0&&hbf.at("physical").at("read_bytes")==st.dram_fill_bytes&&hbf.at("physical").at("write_bytes")==st.dram_writeback_bytes,"one shared backend closes actual work");
 J result={{"status","PASS_SELECTED_CONTINUOUS_CURRENT_HISTORY"},{"checks",checks},{"rows",rows},{"counts",kinds},{"kernels",kernels},{"APIs",apis},{"CTAs",ctas},{"nodes",nodes},{"cycles",d.now},{"host_preparation_seconds",preparation},{"host_execution_seconds",execution},{"host_finalization_seconds",tiny_full::elapsed(finish_start)},{"HBFSIM",hbf},{"dirty",tiny_full::dirty_stats(d.cache)},{"loader",catalog.loading_receipt()},{"writer_attribution",attribution},{"logical_kernel_read_bytes",logical_read},{"logical_kernel_write_bytes",logical_write},{"logical_API_read_bytes",api_read},{"logical_API_write_bytes",api_write},{"complete_prefix_from_process_start",plan.at("complete_prefix_from_process_start")},{"full_history",plan.at("full_history")},{"full_inference",plan.at("full_history")},{"initialization_work",variant},{"hardware_accuracy_claimed",false},{"DMA_hardware_qualified",false},{"initialization_summary_is_original_SASS_DAG",false},{"full_trace_saved",false},{"no_end_dirty_flush",true},{"per_operation_quiescence_is_model_assumption",true}};
 result["ada_alignment"]={{"profile",plan.at("ada_alignment_profile")},{"core_MHz",d.cfg.core_frequency_mhz},{"L1_hit_cycles",d.cfg.per_sm_l1.hit_latency_cycles},{"L2_hit_cycles",d.cfg.l2_hit_latency_cycles},{"L2_bytes",d.cfg.l2_cache_size_bytes},{"L1_capacity_bytes_per_SM",d.cfg.per_sm_l1.capacity_bytes_per_sm},{"external_HBFSIM_route_aligned_to_AccelSim",false},{"L1_r4_adopted",false},{"whole_line_RFO_preserved",true},{"internal_604_parameter_executed",false},{"hardware_accuracy_claimed",false}};
 result["initial_cache_state"]=plan.value("initial_cache_state",std::string("PROCESS_START_EMPTY"));
 result["full_measured_complete"]=plan.value("full_measured",false);
 result["full_inference"]=plan.value("full_measured",false)||plan.at("full_history").get<bool>();
 std::ofstream(output)<<result.dump(2)<<'\n';std::cout<<J({{"status",result.at("status")},{"kernels",kernels},{"APIs",apis},{"cycles",d.now},{"host_execution_seconds",execution}}).dump()<<'\n';return 0;
}
}
int main(int argc,char**argv){try{history_case::need(argc==3,"plan/output");return history_case::run(argv[1],argv[2]);}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
