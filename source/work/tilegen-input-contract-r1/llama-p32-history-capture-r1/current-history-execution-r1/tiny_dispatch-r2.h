#pragma once
// Common full-grid Tiny dispatch. Device owns one persistent cache/backend/clock.
// Source binding and mapper must outlive all completions. No graph approximation
// is added here: use original q16 packets and optional original GEMV phase16.
namespace current_dispatch {
using namespace native_sequence;namespace rt=packet_runtime;
inline void need(bool b,const char*m){if(!b)throw std::runtime_error(m);}
template<class Device>
J tiny_kernel(Device&d,const tiny_full::KernelBinding&b,U id,const J&expected,bool gemv){
 need(d.cache.is_quiescent()&&d.backend.queue_depth()==0,"shared device must drain previous operation");
 auto host_start=tiny_full::HostClock::now();U begin=d.now,returned=d.callbacks;
 auto before=tiny_full::stats(d.cache.runtime_statistics());auto dirty_before=d.cache.dirty_sector_snapshot();auto physical_before=d.backend.physical();
 d.cache.begin_kernel();std::vector<std::unique_ptr<g::Memory>>sram;std::vector<g::Memory*>ptrs;
 for(int sm=0;sm<d.cfg.num_sms;++sm){sram.push_back(std::make_unique<g::Memory>(d.cfg.sram_latency_cycles,d.cfg.sram_bandwidth_bytes_per_cycle,d.cfg.sram_queue_depth));ptrs.push_back(sram.back().get());}
 using Program=decltype(tiny_full::import_program(b,0,16,d.cfg));
 struct Class{Program program;U count=0,nodes=0,global=0,shared=0,copies=0,barriers=0,elements=0,fma=0;};std::map<U,Class>classes;
 for(U c=0;c<b.ctas();++c){U k=b.template_class(c);auto it=classes.find(k);
  if(it==classes.end()){Class cl;cl.program=tiny_full::import_program(b,c,16,d.cfg);cl.nodes=b.nodes(c).size();for(const auto&n:b.nodes(c)){cl.global+=n.kind==tiny_full::Kind::Global;cl.shared+=n.kind==tiny_full::Kind::Shared;cl.copies+=n.kind==tiny_full::Kind::AsyncCopy;cl.elements+=n.compute_elements;cl.fma+=n.tensor_fma;cl.barriers+=n.kind==tiny_full::Kind::Barrier;}it=classes.emplace(k,std::move(cl)).first;}
  need(it->second.nodes==b.nodes(c).size(),"class-specific original source nodes");++it->second.count;
 }
 tiny_full::FrontendPort port(b,d.cache,d.cfg,ptrs,begin,8,[&](rt::Cycle at,unsigned span,g::L2Cache::EpochServiceStatistics&s){return d.service(at,span,s);});
 rt::Runtime runtime(unsigned(d.cfg.num_sms)*4,6,&port,begin,gemv?16:1);port.attach_runtime(runtime);
 if(gemv){need(classes.size()==1,"original GEMV single class");std::vector<memory_phase::SourceFact>facts;
  for(const auto&n:b.nodes(0)){auto k=memory_phase::SourceKind::Other;if(n.kind==tiny_full::Kind::Global&&!n.write)k=memory_phase::SourceKind::GlobalRead;else if(n.kind==tiny_full::Kind::Compute&&n.pipeline=="SIMD")k=memory_phase::SourceKind::SimdCompute;facts.push_back({n.id,n.warp,k});}runtime.register_phase_program(classes.begin()->second.program,facts);
 }
 U count=b.ctas(),resident=U(d.cfg.num_sms)*b.resident_limit();need(count&&resident,"nonempty source with finite occupancy");
 std::set<U>active;std::vector<U>next(d.cfg.num_sms),per_sm(d.cfg.num_sms);std::vector<bool>admitted(count),retired_once(count);
 auto admit=[&](U c,U at){need(c<count&&!admitted[c]&&++per_sm[c%d.cfg.num_sms]<=b.resident_limit(),"exact original-ID admission and SM capacity");admitted[c]=true;
  std::vector<unsigned>sp;for(unsigned w=0;w<b.warps(c);++w)sp.push_back(unsigned(c%d.cfg.num_sms)*4+unsigned(b.warp_token(c,w,d.cfg.num_sms)%4));runtime.add_cta(c,classes.at(b.template_class(c)).program,sp,at);need(active.insert(c).second,"unique resident CTA");};
 for(U c=0;c<std::min(count,resident);++c)admit(c,begin+1);for(unsigned sm=0;sm<next.size();++sm)next[sm]=sm+resident;
 U retired=0,seen=0;
 while(retired<count){++d.now;need(d.now>=begin&&d.now-begin<2000000000ULL,"kernel relative cycle bound");port.tick(d.now);runtime.advance_to(d.now);d.cache.end_cycle(g::checked_cycle(d.now));
  if(runtime.stats().ctas_completed!=seen){seen=runtime.stats().ctas_completed;std::vector<U>done;for(U c:active)if(runtime.cta_complete(c))done.push_back(c);for(U c:done){need(!retired_once[c]&&per_sm[c%d.cfg.num_sms]>0,"exact source retirement");runtime.retire_cta(c);retired_once[c]=true;active.erase(c);--per_sm[c%d.cfg.num_sms];++retired;auto&n=next[c%d.cfg.num_sms];if(n<count){admit(n,d.now+1);n+=d.cfg.num_sms;}}}
 }
 U kernel_end=d.now;need(port.quiescent()&&runtime.pending_tokens()==0&&runtime.resident_ctas()==0,"all actual source completions");port.flush_epoch();
 while(!d.cache.is_quiescent()){++d.now;need(d.now-kernel_end<10000000,"bounded final operation drain");port.tick(d.now);d.cache.end_cycle(g::checked_cycle(d.now));}
 U nodes=0,instructions=0,groups=0,original_groups=0,barriers=0,copies=0,elements=0,fma=0,external_edges=0;for(const auto&[k,cl]:classes){nodes+=cl.count*cl.nodes;instructions+=cl.count*(cl.global+cl.shared+cl.copies);barriers+=cl.count*cl.barriers;copies+=cl.count*cl.copies;elements+=cl.count*cl.elements;fma+=cl.count*cl.fma;external_edges+=cl.count*cl.program->logical_edges;original_groups+=cl.count*cl.program->groups.size();if(gemv){auto*phase=runtime.phase_plan(*cl.program);need(phase!=nullptr,"original GEMV phase plan");groups+=cl.count*phase->units.size();}else groups+=cl.count*cl.program->groups.size();}
 auto front=port.stats();need(front.at("async_copy_dispatches")==J(copies)&&runtime.stats().compute_elements_completed==elements&&runtime.stats().tensor_fma_completed==fma,"all original async/scalar/Tensor work");if(!gemv)need(runtime.stats().logical_external_edges_released==external_edges,"all original external typed edges");need(nodes==expected.at("nodes")&&count==expected.at("CTAs")&&runtime.stats().ctas_added==count&&runtime.stats().ctas_completed==count&&runtime.stats().logical_members_completed==nodes&&runtime.stats().groups_completed==groups,"complete full-grid original work");
 if(gemv)need(runtime.phase_stats().original_groups_completed==original_groups,"all original GEMV q16 work");
 need(front.at("logical_global_read_bytes")==expected.at("requested_read_bytes")&&front.at("logical_global_write_bytes")==expected.at("requested_write_bytes")&&front.at("instructions_accepted")==J(instructions),"complete source memory bytes/instructions");
 for(auto key:{"instruction_ledger_closed","global_line_ledger_closed","shared_service_ledger_closed","async_copy_ledger_closed_at_quiescence"})need(front.at(key)==true,"actual Port closure");
 need(front.at("global_lines_completed")==J(d.callbacks-returned)&&d.cache.per_sm_l1_readiness().live_tickets==0&&d.backend.queue_depth()==0,"callbacks/L1/backend closed");
 auto dirty=d.cache.dirty_sector_snapshot();need(dirty.dirty_sector_ledger_closed&&dirty.writeback_byte_ledger_closed&&dirty_before.resident_dirty_sectors+dirty.dirty_sector_creations-dirty_before.dirty_sector_creations==dirty.evicted_dirty_sectors-dirty_before.evicted_dirty_sectors+dirty.resident_dirty_sectors,"operation I+C=E+F");
 auto delta=tiny_full::delta(tiny_full::stats(d.cache.runtime_statistics()),before);auto physical=d.backend.physical();need(physical.at("read_bytes").template get<U>()-physical_before.at("read_bytes").template get<U>()==delta.at("dram_fill_bytes")&&physical.at("write_bytes").template get<U>()-physical_before.at("write_bytes").template get<U>()==delta.at("dram_writeback_bytes"),"shared actual physical accounting");
 return {{"native_launch_id",id},{"CTAs",count},{"nodes",nodes},{"template_classes",classes.size()},{"source_resident_limit",b.resident_limit()},{"current_occupancy_observed",false},{"start_cycle",begin},{"kernel_end_cycle",kernel_end},{"quiescent_end_cycle",d.now},{"counter_delta",delta},{"frontend",front},{"runtime",tiny_full::runtime_stats(runtime.stats())},{"epoch",port.epoch_statistics()},{"barrier_nodes_completed",barriers},{"async_copy_nodes_completed",copies},{"compute_elements_completed",elements},{"tensor_FMA_completed",fma},{"global_dirty",{{"I",dirty_before.resident_dirty_sectors},{"C",dirty.dirty_sector_creations-dirty_before.dirty_sector_creations},{"E",dirty.evicted_dirty_sectors-dirty_before.evicted_dirty_sectors},{"F",dirty.resident_dirty_sectors}}},{"physical_before",physical_before},{"physical_after",physical},{"host_seconds",tiny_full::elapsed(host_start)},{"exact_original_CTA_coverage",true}};
}
}
