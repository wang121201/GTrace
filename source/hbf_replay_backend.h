#pragma once
#include "replay_backend.h"
#include "host/hbf_controller.hpp"
#include <map>
#include <vector>

namespace hbf_replay_backend {
using U=std::uint64_t;using J=nlohmann::json;
namespace g=GTSim;namespace p=hbfsim::physical;namespace host=hbfsim::host;
struct PageRange { U first_lpn,page_count; };
class Backend final:public replay_backend::Backend {
    struct Live {g::L2DramRequest request;U finish_ps;};
    sg_hbf::Clock clock_;
    p::hbm::HbmDevice buffer_hbm_;
    host::HbfController device_;
    std::vector<PageRange> seeds_;
    U max_live_,seed_pages_=0,last_step_=0,last_admission_=0,last_issue_=0;
    U drain_begin_=0,drain_end_=0,drain_physical_bytes_=0;
    bool stepped_=false,failed_=false,drained_=false,compact_seed_=false;
    replay_backend::Admission admission_;
    g::L2DramRuntimeStatistics traffic_;
    std::map<U,Live> live_;
    std::map<std::pair<U,U>,U> due_;
    static void require(bool ok,const char* why){native_trace::need(ok,why);}
    static bool write(const g::L2DramRequest& r) {
        require(r.cause==g::L2DramRequestCause::FILL_READ||r.cause==g::L2DramRequestCause::DIRTY_WRITEBACK,
                "HBF replay request cause is invalid");
        return r.cause==g::L2DramRequestCause::DIRTY_WRITEBACK;
    }
    bool seeded(U lpn)const {
        auto it=std::upper_bound(seeds_.begin(),seeds_.end(),lpn,
            [](U page,const PageRange& range){return page<range.first_lpn;});
        if(it==seeds_.begin())return false;--it;
        return lpn-it->first_lpn<it->page_count;
    }
    static J work_json(const p::Breakdown& stats) {
        J work=J::object();
#define WORK(x) work[#x]=stats.x
        WORK(ingress_queue_wait_ns);WORK(scheduler_queue_wait_ns);WORK(address_mapping_ns);WORK(translation_ns);
        WORK(mapping_dram_ns);WORK(write_buffer_dram_ns);WORK(refresh_stall_ns);WORK(precharge_ns);WORK(activation_ns);
        WORK(command_ns);WORK(array_read_ns);WORK(array_program_ns);WORK(array_erase_ns);WORK(media_lane_transfer_ns);
        WORK(page_buffer_ns);WORK(sram_staging_ns);WORK(channel_transfer_ns);WORK(tsv_transfer_ns);WORK(hb_io_transfer_ns);
        WORK(transport_latency_ns);WORK(ecc_queue_wait_ns);WORK(ecc_latency_ns);WORK(maintenance_ns);
#undef WORK
        return work;
    }
public:
    Backend(sg_hbf::Clock clock,const hbfsim::app::SystemConfig& config,
            const std::vector<PageRange>& seed_ranges,U max_live=512)
        :clock_(clock),buffer_hbm_(config.hbm),device_(config.hbf),max_live_(max_live) {
        require(max_live>0&&max_live<=512,"HBF replay parent capacity must be 1..512");
        require(config.hbf.device.page_size_bytes==4096,"HBF replay requires native 4096B pages");
        require(config.hbf.host.mapping_mode!=host::MappingMode::RawPhysical,"HBF replay requires logical host mapping");
        require(seed_ranges.size()<=65536,"HBF replay seed range cap exceeded");
        device_.attach_hbm_buffer(buffer_hbm_);
        U end=0;
        for(const auto& range:seed_ranges) {
            require(range.page_count>0&&range.first_lpn<=UINT64_MAX-range.page_count,
                    "HBF replay seed range is empty or overflows");
            require(seeds_.empty()||range.first_lpn>=end,"HBF replay seed ranges must be sorted and disjoint");
            end=range.first_lpn+range.page_count;
            require(end<=device_.logical_capacity_pages(),"HBF replay seed range exceeds logical capacity");
            seed_pages_=native_trace::add(seed_pages_,range.page_count);
            if(!seeds_.empty()&&seeds_.back().first_lpn+seeds_.back().page_count==range.first_lpn)
                seeds_.back().page_count=native_trace::add(seeds_.back().page_count,range.page_count);
            else seeds_.push_back(range);
        }
        if(seeds_.size()==1) {
            // Mutable refers to future mapping updates, not initialization IO:
            // seeding performs no measured read/program traffic.
            compact_seed_=true;
            device_.prepopulate_mutable_logical_page_range(seeds_[0].first_lpn,seeds_[0].page_count);
        } else if(!seeds_.empty()) {
            require(seed_pages_<=(1ULL<<20),"HBF sparse seed exceeds bounded 1M-page materialization; supply an explicit compact image range");
            std::vector<U> pages;pages.reserve(seed_pages_);
            for(const auto& range:seeds_)for(U i=0;i<range.page_count;++i)pages.push_back(range.first_lpn+i);
            device_.prepopulate_logical_pages(pages);
        }
        const auto&s=device_.execution_stats();
        require(s.logical_read_bytes==0&&s.logical_write_bytes==0&&s.physical_read_bytes==0&&s.physical_write_bytes==0,
                "HBF initialization must not enter replay traffic ledger");
    }
    sg_hbf::Clock clock()const override{return clock_;}
    U max_live()const override{return max_live_;}
    U queue_depth()const override{return live_.size();}
    bool all_live_schedules_known()const override{return true;}
    U next_completion_cycle()const override {
        return due_.empty()?native_trace::unknown:clock_.completion_cycle(due_.begin()->first.first);
    }
    bool try_enqueue(const g::L2DramRequest& r,U cycle)override {
        require(!failed_&&!drained_,"HBF replay cannot admit after failure/drain");
        try {
            require(admission_.accepted<UINT64_MAX&&r.request_id==admission_.accepted&&r.source_sequence==r.request_id,
                    "HBF replay requires contiguous ordered source IDs");
            require(cycle<=U(INT64_MAX)&&r.issue_cycle<=cycle&&r.issue_time_ps==clock_.issue_ps(r.issue_cycle)&&
                    (!stepped_||cycle>=last_step_)&&(!admission_.accepted||cycle>=last_admission_),
                    "HBF replay issue/admission clock regressed");
            require(!admission_.accepted||r.issue_cycle>=last_issue_,"HBF replay source issue order regressed");
            const bool is_write=write(r);
            require(r.bytes==(is_write?32U:128U)&&r.address%r.bytes==0&&r.address<=UINT64_MAX-(r.bytes-1),
                    "HBF logical replay requires aligned read128/write32 source requests");
            require(seeded(r.address/4096)&&seeded((r.address+r.bytes-1)/4096),
                    "HBF replay request references an unseeded initial page");
            if(live_.size()>=max_live_){++admission_.blocked;return false;}
            const U admission_ps=clock_.issue_ps(cycle);const double arrival=sg_hbf::ps_to_ns(admission_ps);
            // Same joint frontier as upstream SimulationSession. Never use
            // a speculative HBF finish to advance shared buffer calendars.
            buffer_hbm_.advance_buffer_frontier(arrival);
            const auto completion=device_.issue(p::PhysicalRequest{
                .id="stage-hbf/"+std::to_string(r.request_id),.tier=p::Tier::HBF,
                .op=is_write?p::Op::Write:p::Op::Read,.address_space=p::AddressSpace::Logical,
                .trace={p::TraceMode::Off,false},.arrival_ns=arrival,.addr=r.address,.bytes=r.bytes,.stream_id=0,
                .heatmap_source=p::HeatmapTrafficSource::Direct});
            require(completion.id=="stage-hbf/"+std::to_string(r.request_id)&&completion.tier==p::Tier::HBF&&
                completion.op==(is_write?p::Op::Write:p::Op::Read)&&completion.logical_bytes==r.bytes&&
                completion.arrival_ns==arrival&&std::isfinite(completion.finish_ns)&&
                completion.finish_ns>=completion.start_ns&&completion.start_ns>=arrival&&completion.spans.empty(),
                "HBF native completion identity, byte count, or timing differs");
            const U finish_ps=sg_hbf::ns_to_ps(completion.finish_ns);
            require(finish_ps>=admission_ps&&clock_.completion_cycle(finish_ps)>cycle,
                    "HBF native request must finish after its admission cycle");
            require(live_.emplace(r.request_id,Live{r,finish_ps}).second,"duplicate HBF source parent");
            require(due_.emplace(std::make_pair(finish_ps,r.source_sequence),r.request_id).second,
                    "duplicate HBF completion key");
            ++admission_.accepted;++admission_.native_completions_posted;
            admission_.peak_live=std::max(admission_.peak_live,U(live_.size()));
            for(U value:{r.request_id,r.address,U(r.bytes),U(is_write)})for(unsigned byte=0;byte<8;++byte) {
                admission_.actual_request_shape_fnv1a64^=(value>>(8*byte))&255;
                admission_.actual_request_shape_fnv1a64*=1099511628211ULL;
            }
            if(is_write){++traffic_.writeback_requests;traffic_.writeback_bytes=native_trace::add(traffic_.writeback_bytes,r.bytes);}
            else{++traffic_.fill_requests;traffic_.fill_bytes=native_trace::add(traffic_.fill_bytes,r.bytes);}
            traffic_.peak_queue_depth=std::max<U>(traffic_.peak_queue_depth,live_.size());
            last_admission_=cycle;last_issue_=r.issue_cycle;return true;
        }catch(...){failed_=true;throw;}
    }
    std::vector<g::L2DramCompletion> step(U cycle)override {
        require(!failed_,"failed HBF replay backend");
        try {
            require(cycle<=U(INT64_MAX)&&(!stepped_||cycle>=last_step_)&&
                    (!admission_.accepted||cycle>=last_admission_),"HBF replay step regressed");
            stepped_=true;last_step_=cycle;const U through=clock_.poll_ps(cycle);
            std::vector<g::L2DramCompletion> result;
            while(!due_.empty()&&due_.begin()->first.first<=through) {
                const U id=due_.begin()->second;const auto& live=live_.at(id);const auto&r=live.request;
                const bool is_write=write(r);const U complete_cycle=clock_.completion_cycle(live.finish_ps);
                require(complete_cycle<=cycle&&complete_cycle>=r.issue_cycle,"early HBF completion delivery");
                result.push_back({r.request_id,r.source_sequence,r.issue_cycle,r.issue_time_ps,complete_cycle,r.key,is_write});
                ++admission_.completed;admission_.last_completion_ps=live.finish_ps;
                if(is_write){++traffic_.writeback_completions;traffic_.writeback_completed_bytes=native_trace::add(traffic_.writeback_completed_bytes,r.bytes);}
                else{++traffic_.fill_completions;traffic_.fill_completed_bytes=native_trace::add(traffic_.fill_completed_bytes,r.bytes);}
                live_.erase(id);due_.erase(due_.begin());
            }
            return result;
        }catch(...){failed_=true;throw;}
    }
    U drain_cycle(U cycle)override {
        require(!failed_&&!drained_&&live_.empty()&&due_.empty(),"HBF final drain requires completed source parents and may run once");
        try {
            require(cycle<=U(INT64_MAX)&&(!stepped_||cycle>=last_step_)&&
                    (!admission_.accepted||cycle>=last_admission_),"HBF final drain frontier regressed");
            drain_begin_=cycle;
            const double arrival=sg_hbf::ps_to_ns(clock_.issue_ps(cycle));
            const auto completion=device_.drain_pending("stage-final-maintenance",arrival,{p::TraceMode::Off,false});
            require(std::isfinite(completion.finish_ns)&&completion.finish_ns>=arrival&&completion.logical_bytes==0&&
                    completion.spans.empty(),"HBF maintenance drain timing/ledger differs");
            drain_physical_bytes_=completion.physical_bytes;
            drain_end_=completion.finish_ns==arrival?cycle:std::max(cycle,clock_.completion_cycle(sg_hbf::ns_to_ps(completion.finish_ns)));
            drained_=true;return drain_end_;
        }catch(...){failed_=true;throw;}
    }
    void finalize()const override {
        require(!failed_&&drained_&&live_.empty()&&due_.empty()&&admission_.accepted==admission_.completed&&
            admission_.accepted==admission_.native_completions_posted,"unclosed HBF source parent ledger");
        require(traffic_.fill_bytes==traffic_.fill_completed_bytes&&traffic_.writeback_bytes==traffic_.writeback_completed_bytes,
                "unclosed HBF input byte ledger");
        const auto&s=device_.stats();
        require(s.logical_read_bytes==traffic_.fill_bytes&&s.logical_write_bytes==traffic_.writeback_bytes,
                "HBF native logical/source byte ledger differs");
        require(device_.quiescence_stats().quiescent(),"HBF final drain left pending controller work");
    }
    replay_backend::Snapshot snapshot()const override {
        const auto&s=device_.stats();J physical;
#define FIELD(x) physical[#x]=s.x
        FIELD(logical_read_bytes);FIELD(logical_write_bytes);FIELD(physical_read_bytes);FIELD(physical_write_bytes);
        FIELD(data_program_payload_bytes);FIELD(mapping_program_payload_bytes);FIELD(gc_relocation_payload_bytes);
        FIELD(read_requests);FIELD(program_requests);FIELD(erase_requests);FIELD(auto_erase_requests);
        FIELD(page_reads);FIELD(data_programs);FIELD(page_programs);FIELD(block_erases);
        FIELD(initial_logical_data_pages);FIELD(initial_mapping_pages);
        FIELD(logical_capacity_pages);FIELD(logical_capacity_bytes);
        FIELD(read_buffer_hits);FIELD(read_buffer_misses);FIELD(read_buffer_read_bytes);
        FIELD(write_buffer_hits);FIELD(write_buffer_misses);FIELD(write_buffer_flushes);FIELD(write_buffer_merged_bytes);
        FIELD(write_buffer_read_hits);FIELD(write_buffer_read_bytes);FIELD(host_hbm_read_bytes);FIELD(host_hbm_write_bytes);
        FIELD(host_hbm_reserved_bytes);FIELD(host_hbm_busy_ns);FIELD(host_hbm_queue_wait_ns);
        FIELD(finish_ns);FIELD(stacks);FIELD(channels);FIELD(planes);FIELD(active_channels);FIELD(active_planes);
        FIELD(logic_ingress_busy_ns);FIELD(tsv_busy_ns);FIELD(sram_busy_ns);FIELD(channel_command_busy_ns);FIELD(channel_data_busy_ns);
#undef FIELD
        physical["first_arrival_ns"]=std::isfinite(s.first_arrival_ns)?J(s.first_arrival_ns):J(nullptr);
        physical["overlapping_stage_work_ns"]=work_json(s.stage_work);
        physical["kind"]="HBF_LOGICAL_CONTROLLER_NAND";
        physical["payload_page_bytes"]=4096;physical["oob_bytes_per_page"]=device_.config().device.oob_bytes_per_page;
        physical["bytes_scope"]="logical trace bytes differ from NAND page payload and controller HBM traffic; physical payload excludes OOB";
        physical["controller_hbm"]=trace_replay::physical_json(buffer_hbm_.stats());
        physical["final_quiescent"]=device_.quiescence_stats().quiescent();
        physical["read_payload_amplification"]=s.logical_read_bytes?J(double(s.physical_read_bytes)/s.logical_read_bytes):J(nullptr);
        physical["write_payload_amplification"]=s.logical_write_bytes?J(double(s.physical_write_bytes)/s.logical_write_bytes):J(nullptr);
        const auto& d=device_.config().device;const auto& h=device_.config().host;const auto& b=buffer_hbm_.config();
        physical["resolved_config"]={
            {"hbf_device",{{"standard",std::string(d.standard)},{"speed_grade",d.speed_grade},
                {"stacks",d.stacks},{"channels_per_stack",d.channels_per_stack},{"dies_per_channel",d.dies_per_channel},
                {"planes_per_die",d.planes_per_die},{"blocks_per_plane",d.blocks_per_plane},{"pages_per_block",d.pages_per_block},
                {"page_size_bytes",d.page_size_bytes},{"oob_bytes_per_page",d.oob_bytes_per_page},
                {"read_page_ns",d.t_read_page_ns},{"program_page_ns",d.t_program_page_ns},{"erase_block_ns",d.t_erase_block_ns},
                {"ecc_decode_latency_ns",d.ecc_decode_latency_ns},{"ecc_encode_latency_ns",d.ecc_encode_latency_ns},
                {"ecc_decode_raw_bandwidth_GBps_per_die",d.ecc_decode_raw_bandwidth_GBps_per_die},
                {"ecc_encode_raw_bandwidth_GBps_per_die",d.ecc_encode_raw_bandwidth_GBps_per_die},
                {"HBIO_peak_payload_GBps_per_stack",d.hb_io_bandwidth_GBps()},
                {"HBIO_peak_payload_GBps_total",d.stacks*d.hb_io_bandwidth_GBps()},
                {"channel_raw_bandwidth_GBps",d.channel_bandwidth_GBps},{"tsv_raw_bandwidth_GBps",d.tsv_bandwidth_GBps},
                {"page_read_queue_depth_per_stack",d.page_read_queue_depth_per_stack},
                {"page_buffer_banks_per_plane",d.page_buffer_banks_per_plane},{"thermal_enabled",d.thermal_enabled},
                {"peak_scope","configured external interface envelope, not measured or sustained NAND bandwidth"}}},
            {"hbf_host",{{"mapping_mode",host::to_string(h.mapping_mode)},{"mapping_cache_layout",host::to_string(h.mapping_cache_layout)},
                {"logical_capacity_pages",device_.logical_capacity_pages()},{"logical_capacity_bytes",s.logical_capacity_bytes},
                {"ctrl_dram_bytes",h.ctrl_dram_bytes},{"write_coalescing_enabled",h.write_coalescing_enabled},
                {"write_buffer_pages_per_stack",h.write_buffer_pages},{"write_buffer_flush_threshold_pages",h.write_buffer_flush_threshold_pages},
                {"write_buffer_completion_requires_flush",h.write_buffer_completion_requires_flush},
                {"auto_gc_enabled",h.auto_gc_enabled},{"gc_reserved_free_blocks_per_plane",h.gc_reserved_free_blocks_per_plane},
                {"gc_low_watermark_pages",h.gc_low_watermark_pages},{"gc_hard_watermark_pages",h.gc_hard_watermark_pages}}},
            {"controller_hbm",{{"role","controller-buffer-only; no source trace requests target this HBM"},
                {"capacity_bytes",b.device.capacity_bytes},{"reserved_buffer_bytes",buffer_hbm_.controller_buffer_bytes()},
                {"stacks",b.device.stacks},{"channels_per_stack",b.device.channels_per_stack},
                {"pseudo_channels_per_channel",b.device.pseudo_channels_per_channel},
                {"channel_width_bits",b.device.channel_width_bits},{"pin_rate_Gbps",b.device.pin_rate_Gbps},
                {"burst_bytes",b.burst_bytes()},
                {"model_scope","native shared channel calendars for controller buffers; buffer row commands are not expanded"}}}};
        replay_backend::Snapshot result;result.admission=admission_;result.traffic=traffic_;
        result.physical=std::move(physical);result.first_arrival_ns=s.first_arrival_ns;result.finish_ns=s.finish_ns;
        result.physical_read_bytes=s.physical_read_bytes;result.physical_write_bytes=s.physical_write_bytes;
        result.source_bytes_equal_physical=false;result.kind="HBF_LOGICAL_CONTROLLER_NAND";
        result.diagnostics={{"native_enqueue_execution_checks",nullptr},{"native_enqueue_service_violations",nullptr},
            {"credits_per_channel_bursts",nullptr},{"peak_reserved_bursts",nullptr},{"peak_channel_burst_credits",nullptr},
            {"final_reserved_bursts",nullptr},{"parent_credit_unit","host source request; not NAND pages or 32B bursts"},
            {"service_quantum",nullptr},{"drain_mode","native-hbf-scheduled-completions"},
            {"independent_fast_drains",nullptr},{"fallback_global_drains",nullptr},
            {"HBF_host_write_completion_requires_flush",device_.config().host.write_buffer_completion_requires_flush},
            {"HBF_write_coalescing",device_.config().host.write_coalescing_enabled},
            {"seed_policy","all declared service pages initially resident; mutable logical mappings; no timed preload IO"},
            {"seed_pages",seed_pages_},{"seed_ranges",seeds_.size()},{"compact_seed",compact_seed_},
            {"HBF_logical_address_scope","unchanged service byte addresses reinterpreted as logical HBF addresses"},
            {"HBF_final_drain_begin_cycle",drain_begin_},{"HBF_final_drain_end_cycle",drain_end_},
            {"HBF_final_drain_physical_bytes",drain_physical_bytes_}};
        return result;
    }
};
} // namespace hbf_replay_backend
