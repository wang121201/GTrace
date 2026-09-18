#pragma once
// Hybrid adapter only. Runtime/port/import/source providers remain the qualified
// tiny implementations. The build must map packet-runtime-r1/runtime.h to the
// frozen tiny-full-r1/runtime-r4.h; no event-queue/frontier/batch overlays.
#include "../tilegen-tiny-full-r1/source.h"
#include "../tilegen-tiny-full-r1/import.h"
#include "../tilegen-memory-epoch-native-r1/port.h"
#include "../tilegen-tiny-full-r1/reporting.h"
#include <cmath>
#include <limits>
#include "../tilegen-prefill-compute-native-r1/prefill_setup.h"

namespace hybrid_full {

struct TinyPolicy {
    unsigned q=16;
    unsigned memory_epoch=1; // 1 unchanged; 4/8 explicit finite resource epochs.
    unsigned prefill_compute_width=1; // 1 retains the original Prefill engine.
    unsigned phase_width=1; // 1 = original q16; 4/16 = bounded GEMV memory phase.
    bool silu_q1=false; // Explicit local override; never inferred from the family.
    std::uint64_t max_kernel_cycles=2000000000ULL;
    std::uint64_t max_drain_cycles=10000000ULL;
};

// All four borrowed objects outlive this context and every run_kernel call.
// The caller owns the immutable source model and binding, which must outlive the
// local FrontendPort. Each kernel gets original fresh per-SM SRAM/ADA/frontends;
// L1/L2/HBFSIM, their request sequence, mapper, and absolute clock are retained.
// Exceptions are workflow fail-stop; this object cannot resume a partial kernel.
class TinyContext {
    using J=nlohmann::json;
    using U=std::uint64_t;
    const GTSim::SimulatorConfig& cfg_;
    GTSim::L2Cache& l2_;
    coupling::Runtime& memory_;
    GTSim::Cycle& cycle_;

    static U source_total(const J& evidence,const std::string& field,U count) {
        using namespace tiny_full;
        if(evidence.contains("selected_totals"))
            return p::natural(evidence.at("selected_totals").at(field));
        const std::string family=evidence.at("family");
        p::need(family=="GEMV"||family=="P28QKV"||family=="GEMMO"||
            family=="GEMMGate"||family=="GEMMDown",
            "nonuniform source requires exact selected totals");
        return p::multiply(p::natural(evidence.at(field+"_per_CTA")),count);
    }

public:
    TinyContext(const GTSim::SimulatorConfig& cfg,GTSim::L2Cache& l2,
                coupling::Runtime& memory,GTSim::Cycle& shared_cycle)
        :cfg_(cfg),l2_(l2),memory_(memory),cycle_(shared_cycle) {
        tiny_full::p::need(cycle_>=0&&cfg_.num_sms==48,"hybrid tiny resource/clock profile");
        tiny_full::p::need(bool(memory_.backend)&&memory_.gddr6_active,
            "hybrid tiny requires supplied native GDDR6 backend");
    }
    TinyContext(const TinyContext&)=delete;
    TinyContext& operator=(const TinyContext&)=delete;

    // entry supplies the already-qualified original family/call identity only.
    // binding_seconds is measured by the caller around binding creation. The
    // returned row retains every original per-call field, plus explicit policy
    // and cumulative before/after counters for fine/tiny handoff auditing.
    J run_kernel(const tiny_full::KernelBinding& binding,const J& entry,
                 const TinyPolicy& policy=TinyPolicy{},double binding_seconds=0) {
        using namespace tiny_full;
        const auto start=HostClock::now();
        const std::string family=entry.at("family").get<std::string>();
        p::need(policy.q==1||policy.q==2||policy.q==4||policy.q==8||policy.q==16,
            "hybrid tiny packet quantum");
        const unsigned q=policy.silu_q1&&family=="SiLU"?1:policy.q;
        p::need(policy.phase_width==1||policy.phase_width==4||policy.phase_width==16,"memory phase width");
        if(policy.phase_width>1)p::need(policy.q==16&&!policy.silu_q1,"phase first experiment fixes q16 for every family");
        const unsigned phase_width=family=="GEMV"?policy.phase_width:1;
        p::need(policy.memory_epoch==1||policy.memory_epoch==4||policy.memory_epoch==8,"memory epoch quantum");
        if(policy.memory_epoch>1){
            memory_.backend->require_epoch_policy();
            l2_.require_memory_epoch_profile(*memory_.backend);
        }
        p::need(cycle_>=0&&policy.max_kernel_cycles>0&&policy.max_drain_cycles>0,
            "hybrid tiny cycle budget");
        p::need(std::isfinite(binding_seconds)&&binding_seconds>=0,"binding host timer");
        // No silent signed-to-unsigned wrap or uint64-to-core narrowing. Reserving
        // both budgets proves every serviced absolute cycle is representable.
        (void)g::cycle_add(g::cycle_add(cycle_,g::checked_cycle(policy.max_kernel_cycles)),
            g::checked_cycle(policy.max_drain_cycles));
        const U ctas=binding.ctas();
        p::need(ctas>0&&ctas<=U(std::numeric_limits<int>::max()),"full selected CTA domain");
        const auto program_start=HostClock::now();
        std::map<U,std::shared_ptr<const rt::Program>> programs;
        std::map<U,std::pair<U,U>> program_work;
        U census_group_visits=0;
        PrefillSetup prefill(binding,family,q,cfg_,policy.prefill_compute_width);
        std::map<U,std::vector<memory_phase::SourceFact>> phase_facts;
        std::vector<std::shared_ptr<const rt::Program>> cta_programs;
        cta_programs.reserve(ctas); // O(grid) handles, not a full-grid node/trace copy.
        U expected_nodes=0,expected_edges=0,expected_groups=0;
        U expected_compute=0,expected_fma=0,internal_edges=0;
        for(U c=0;c<ctas;++c) {
            const U key=binding.template_class(c);
            auto it=programs.find(key);
            if(it==programs.end())it=programs.emplace(key,import_program(binding,c,q,cfg_)).first;
            if(phase_width>1&&!phase_facts.count(key)) {
                std::vector<memory_phase::SourceFact> facts;
                const auto original=binding.nodes(c);facts.reserve(original.size());
                for(const auto& node:original) {
                    p::need(node.id==facts.size(),"phase source fact identity");
                    auto kind=memory_phase::SourceKind::Other;
                    if(node.kind==Kind::Global&&!node.write)kind=memory_phase::SourceKind::GlobalRead;
                    else if(node.kind==Kind::Compute&&node.pipeline=="SIMD")kind=memory_phase::SourceKind::SimdCompute;
                    facts.push_back({node.id,node.warp,kind});
                }
                phase_facts.emplace(key,std::move(facts));
            }
            prefill.observe(c,it->second);
            const auto& program=*it->second;cta_programs.push_back(it->second);
            expected_nodes=p::add(expected_nodes,program.logical_members);
            expected_edges=p::add(expected_edges,program.logical_edges);
            expected_groups=p::add(expected_groups,program.groups.size());
            internal_edges=p::add(internal_edges,program.internal_logical_edges);
            auto wi=program_work.find(key);
            if(wi==program_work.end()) {
                U elements=0,fma=0;
                for(const auto& group:program.groups) {
                    elements=p::add(elements,group.modeled_compute_elements);
                    fma=p::add(fma,group.modeled_tensor_fma);
                    census_group_visits=p::add(census_group_visits,1);
                }
                wi=program_work.emplace(key,std::make_pair(elements,fma)).first;
            }
            // Same nonnegative integer census, once for this CTA. No runtime
            // compute issue, dependency release, or memory operation is skipped.
            expected_compute=p::add(expected_compute,wi->second.first);
            expected_fma=p::add(expected_fma,wi->second.second);
        }
        prefill.finish(cta_programs);
        const double program_seconds=elapsed(program_start);
        const auto execution_start=HostClock::now();
        const U begin=U(cycle_);
        const auto before=l2_.runtime_statistics();
        const auto physical_before=memory_.backend->physical_statistics();
        const J admission_before=native_admission(memory_);
        p::need(l2_.is_quiescent(),"tiny kernel starts quiescent");
        sg_hbf::require_external_only(l2_);
        l2_.begin_kernel(); // Exactly once; existing configured L1 boundary policy.
        std::vector<std::unique_ptr<g::Memory>> srams;
        std::vector<g::Memory*> ptrs;
        for(int sm=0;sm<cfg_.num_sms;++sm) {
            srams.push_back(std::make_unique<g::Memory>(cfg_.sram_latency_cycles,
                cfg_.sram_bandwidth_bytes_per_cycle,cfg_.sram_queue_depth));
            ptrs.push_back(srams.back().get());
        }
        auto epoch_l2=[&](rt::Cycle at,unsigned span,g::L2Cache::EpochServiceStatistics& observed) {
            const auto core=g::checked_cycle(at);
            l2_.require_memory_epoch_span(core,span,*memory_.backend);
            // Exactly one old batch, before any L2 fill/eviction/new arrival.
            const auto old_returns=memory_.backend->step_epoch(at);
            return l2_.service_epoch(core,span,*memory_.backend,old_returns,&observed);
        };
        std::function<std::vector<int>(rt::Cycle,unsigned,g::L2Cache::EpochServiceStatistics&)> epoch_service;
        if(policy.memory_epoch>1)epoch_service=epoch_l2;
        FrontendPort port(binding,l2_,cfg_,ptrs,begin,policy.memory_epoch,std::move(epoch_service));
        rt::Runtime runtime(unsigned(cfg_.num_sms)*4,6,&port,begin,phase_width,prefill.selection());
        U phase_groups=expected_groups,phase_edges=expected_edges,phase_gates=0;
        U phase_internal=0,phase_entries=0,phase_reads=0,phase_computes=0;
        double phase_setup_seconds=0;
        if(phase_width>1) {
            const auto phase_start=HostClock::now();
            for(const auto& item:programs)runtime.register_phase_program(item.second,phase_facts.at(item.first));
            phase_groups=phase_edges=0;
            for(const auto& program:cta_programs) {
                const auto* plan=runtime.phase_plan(*program);p::need(plan!=nullptr,"registered phase program");
                phase_groups=p::add(phase_groups,plan->units.size());phase_edges=p::add(phase_edges,plan->external_edge_multiplicity);
                phase_gates=p::add(phase_gates,plan->gates.size());phase_internal=p::add(phase_internal,plan->internal_edges_newly_compiled);
                phase_entries=p::add(phase_entries,plan->phase_count);
                // Shared-plan totals cached below, never materialize memory.
            }
            // Count each shared plan once, then multiply by its actual CTA use.
            std::map<const rt::Program*,std::pair<U,U>> prefix_work;
            for(const auto& program:cta_programs) {
                auto it=prefix_work.find(program.get());
                if(it==prefix_work.end()) {
                    U reads=0,computes=0;
                    for(const auto& unit:runtime.phase_plan(*program)->units)if(unit.phase) {
                        reads=p::add(reads,unit.checkpoints.size());computes=p::add(computes,unit.compute.size());
                    }
                    it=prefix_work.emplace(program.get(),std::make_pair(reads,computes)).first;
                }
                phase_reads=p::add(phase_reads,it->second.first);phase_computes=p::add(phase_computes,it->second.second);
            }
            phase_setup_seconds=elapsed(phase_start);
        }

        port.attach_runtime(runtime);
        std::set<U> active;
        std::vector<U> next(cfg_.num_sms);
        auto admit=[&](U c,U ready) {
            std::vector<unsigned> placement;
            for(unsigned w=0;w<binding.warps(c);++w)
                placement.push_back(unsigned(c%U(cfg_.num_sms))*4+
                    unsigned(binding.warp_token(c,w,unsigned(cfg_.num_sms))%4));
            runtime.add_cta(c,cta_programs[c],placement,ready);
            p::need(active.insert(c).second,"unique CTA");
        };
        const U resident=p::multiply(U(cfg_.num_sms),binding.resident_limit());
        p::need(resident>0,"resident limit");
        for(U c=0;c<std::min(ctas,resident);++c)admit(c,U(g::cycle_add(cycle_,1)));
        for(unsigned sm=0;sm<next.size();++sm)next[sm]=p::add(sm,resident);
        U retired=0,seen=0;
        while(retired<ctas) {
            p::need(U(cycle_)-begin<policy.max_kernel_cycles,"tiny kernel cycle budget");
            cycle_=g::cycle_add(cycle_,1);
            // q1 retains the old service. q>1 still ticks and dispatches
            // every real cycle; only the Port heavy stage uses epochs.
            port.tick(U(cycle_));runtime.advance_to(U(cycle_));l2_.end_cycle(cycle_);
            if(runtime.stats().ctas_completed!=seen) {
                seen=runtime.stats().ctas_completed;
                std::vector<U> done;
                for(U c:active)if(runtime.cta_complete(c))done.push_back(c);
                for(U c:done) {
                    runtime.retire_cta(c);active.erase(c);++retired;
                    auto& n=next[c%U(cfg_.num_sms)];
                    if(n<ctas) {
                        admit(n,U(g::cycle_add(cycle_,1)));
                        n=p::add(n,U(cfg_.num_sms));
                    }
                }
            }
        }
        const U kernel_end=U(cycle_);
        p::need(port.quiescent()&&runtime.pending_tokens()==0&&runtime.resident_ctas()==0,
            "tiny frontend complete");
        if(policy.memory_epoch>1)port.flush_epoch();
        const auto front=port.stats();
        const J epoch_kernel=policy.memory_epoch>1?port.epoch_statistics():J(nullptr);
        for(const auto* key:{"instruction_ledger_closed","global_line_ledger_closed",
            "shared_service_ledger_closed","async_copy_ledger_closed_at_quiescence"})
            p::need(front.at(key)==true,"tiny frontend ledger");
        while(!l2_.is_quiescent()) {
            p::need(U(cycle_)-kernel_end<policy.max_drain_cycles,"tiny drain budget");
            cycle_=g::cycle_add(cycle_,1);
            if(policy.memory_epoch==1)p::need(l2_.step(cycle_).empty(),"late instruction completion");
            else port.tick(U(cycle_)); // no live token: only old L2/WB drain
            l2_.end_cycle(cycle_);
        }
        // Invalid future Schedule/RetryWake entities need not be drained; no
        // live token/CTA/port request survives the original termination boundary.
        const double execution_seconds=elapsed(execution_start);
        const auto census_start=HostClock::now();
        require_closed(l2_,memory_);
        const auto after=l2_.runtime_statistics();
        const auto before_json=stats(before),after_json=stats(after);
        const auto d=delta(after_json,before_json);
        const U rd=d.at("dram_fill_bytes").get<U>();
        const U wr=d.at("dram_writeback_bytes").get<U>();
        const auto physical_after=memory_.backend->physical_statistics();
        p::need(physical_after.read_bytes>=physical_before.read_bytes&&
            physical_after.write_bytes>=physical_before.write_bytes&&
            physical_after.read_bytes-physical_before.read_bytes==rd&&
            physical_after.write_bytes-physical_before.write_bytes==wr,"native byte deltas");
        const auto& s=runtime.stats();
        p::need(s.ctas_added==ctas&&s.ctas_completed==ctas&&
            s.logical_members_completed==expected_nodes&&
            s.logical_external_edges_released==phase_edges&&
            s.groups_completed==phase_groups&&s.compute_elements_completed==expected_compute&&
            s.tensor_fma_completed==expected_fma,"tiny original work census");
        const auto& ph=runtime.phase_stats();
        if(phase_width>1)p::need(ph.entries==phase_entries&&ph.returns==phase_reads&&
            ph.virtual_compute_issues==phase_computes&&ph.issue_tails==phase_entries&&
            ph.compute_tails==phase_entries&&ph.return_tails==phase_entries&&
            ph.original_groups_completed==expected_groups&&ph.internal_edges_covered==phase_internal&&
            ph.gate_updates==phase_gates&&s.typed_group_events_released==phase_gates&&
            p::add(s.logical_external_edges_released,ph.internal_edges_covered)==expected_edges,
            "phase original work, typed coverage, three tails and actual gate closure");
        const auto evidence=binding.evidence();
        p::need(evidence.at("family")==family,"bound tiny family identity");
        for(const auto& pair:std::vector<std::pair<std::string,std::string>>{
            {"logical_global_read_bytes","global_read_bytes"},
            {"logical_global_write_bytes","global_write_bytes"},
            {"logical_shared_read_bytes","shared_read_bytes"},
            {"logical_shared_write_bytes","shared_write_bytes"},
            {"async_copy_dispatches","async_copies"},
            {"zero_source_copies","zero_source_async_copies"}})
            p::need(front.at(pair.first)==source_total(evidence,pair.second,ctas),
                "tiny independent source byte/copy census");
        p::need(expected_nodes==source_total(evidence,"nodes",ctas)&&
            expected_compute==source_total(evidence,"scalar_compute_elements",ctas)&&
            expected_fma==source_total(evidence,"declared_tensor_FMA",ctas)&&
            p::add(expected_edges,internal_edges)==p::add(
                source_total(evidence,"completion_edges",ctas),source_total(evidence,"issue_edges",ctas)),
            "original source logical work and typed-edge census");
        const J prefill_diagnostic=prefill.report(runtime,expected_nodes,expected_groups,expected_edges,internal_edges,expected_compute,expected_fma);
        const double census_seconds=elapsed(census_start);
        const double seconds=binding_seconds+elapsed(start);
        J result={{"family",entry.at("family")},{"call",entry.at("call")},{"source",evidence},
            {"start_cycle",begin},{"kernel_cycles",kernel_end-begin},
            {"drain_cycles",U(cycle_)-kernel_end},{"window_cycles",U(cycle_)-begin},
            {"CTAs",ctas},{"logical_nodes",expected_nodes},
            {"host_template_census",{{"actual_group_visits",census_group_visits},
                {"program_classes",program_work.size()},{"CTA_census_additions",ctas},
                {"scope",phase_width==1?"Only immutable Program work sums reused within a call; every resident runtime operation retained.":"Original immutable work census; phase compute/dependency operations compiled into shared prefixes and three milestone gates."}}},
            {"memory_phase",{{"requested_width",policy.phase_width},{"effective_width",phase_width},
                {"schedule_invocations_scope",phase_width==1?"all_schedule_entries":"unleased_schedule_entries_after_phase_lease_guard"},
                {"plans",ph.plans},{"shared_phases",ph.phases},{"entries",ph.entries},
                {"checkpoint_attempts",ph.checkpoints},{"checkpoint_retries",ph.retries},{"read_notifications",ph.returns},
                {"virtual_compute_issues",ph.virtual_compute_issues},{"issue_tails",ph.issue_tails},
                {"compute_tails",ph.compute_tails},{"return_tails",ph.return_tails},
                {"original_groups_completed",ph.original_groups_completed},{"internal_edges_compiled",ph.internal_edges_covered},
                {"actual_gate_updates",ph.gate_updates},{"cycle_fallbacks",ph.cycle_fallbacks},
                {"original_q16_groups",expected_groups},{"expected_runtime_units",phase_groups},
                {"original_q16_external_edges",expected_edges},{"expected_external_gate_multiplicity",phase_edges},
                {"expected_gate_updates",phase_gates},{"expected_phase_entries",phase_entries},
                {"expected_phase_reads",phase_reads},{"expected_virtual_compute_issues",phase_computes},
                {"host_partition_registration_seconds",phase_setup_seconds}}},
            {"prefill_compute",prefill_diagnostic},
            {"runtime",runtime_stats(s)},{"frontend",front},{"counter_delta",d},
            {"DRAM_read_bytes",rd},{"DRAM_write_bytes",wr},{"dirty_sectors_cumulative",dirty_stats(l2_)},
            {"host_stage_seconds",{{"binding_and_source_seal",binding_seconds},
                {"template_compilation",program_seconds},
                {"resident_runtime_and_memory_drain",execution_seconds},{"census",census_seconds}}},
            {"host_engine_seconds",seconds},
            {"route","tiny"},{"effective_q",q},{"silu_q1_policy_enabled",policy.silu_q1},
            {"counter_before",before_json},{"counter_after",after_json},
            {"native_admission_before",admission_before},{"native_admission_after",native_admission(memory_)},
            {"native_physical_before",tiny_hbf_reporting::native_statistics(physical_before)},
            {"native_physical_after",tiny_hbf_reporting::native_statistics(physical_after)},
            {"logical_internal_edges_covered_by_packet_plan",internal_edges},
            {"kernel_end_cycle",kernel_end},{"quiescent_end_cycle",U(cycle_)}};
        if(policy.memory_epoch>1)result["memory_epoch"]={
            {"schema","TINY_MEMORY_EPOCH_SOURCE_V1"},{"quantum",policy.memory_epoch},
            {"hbf_common_policy","independent/1"},{"kernel",epoch_kernel},
            {"after_drain",port.epoch_statistics()},
            {"scope","Tiny Port heavy service only; Runtime and instruction dispatch remain per cycle; finite virtual resource slots, actual callbacks at boundary"},
            {"timing_qualification","EXPLICIT_UNCALIBRATED_MEMORY_EPOCH_APPROXIMATION"}};
        return result;
    }
};

} // namespace hybrid_full
