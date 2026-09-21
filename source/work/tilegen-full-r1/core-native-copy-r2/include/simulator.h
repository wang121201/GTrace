#include "cycle.h"
#ifndef SIMULATOR_H
#define SIMULATOR_H

#include "gpu.h"
#include "cta_graph_store.h"
#include "dag_node.h"
#include "sector_observer.h"
#include "rtx4000_ada_profile_readiness.h"
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace GTSim {

enum class WorkloadType {
    GEMM,
    FA3,
    FlashMLA,
    FlashDecoding,
    ClusterReduce,
    Llama3Elementwise
};

inline constexpr int kH100SxmNumSms = 132;
inline constexpr int kRtx4000AdaNumSms = 48;

// Hardware facts admitted by the sealed M7 exact-profile evidence.  This is
// deliberately not a runnable SimulatorConfig: the timing, cache-service,
// scheduler, Tensor Core, TMA, and SRAM fields listed below remain unresolved.
struct Rtx4000AdaHardwareDescriptor {
    std::string architecture_profile_name;
    std::string device_name;
    std::string device_uuid;
    int compute_capability_major;
    int compute_capability_minor;
    int num_sms;
    int l2_cache_size_bytes;
    double reported_core_frequency_mhz;
    double theoretical_dram_bandwidth_gbps;
    bool core_frequency_admitted_for_simulation;
    bool dram_bandwidth_admitted_for_simulation;
    bool runnable_simulator_profile;
    std::vector<std::string> unresolved_simulator_fields;
};

inline Rtx4000AdaHardwareDescriptor
make_rtx4000_ada_admitted_hardware_descriptor() {
    const auto readiness = make_rtx4000_ada_profile_readiness_decision();
    return {
        "rtx4000-ada-m7-exact-profile",
        "NVIDIA RTX 4000 Ada Generation",
        "GPU-18ace299-5348-e6e4-d48c-1ee5a602859b",
        8,
        9,
        kRtx4000AdaNumSms,
        40 * 1024 * 1024,
        2175.0,
        360.04,
        false,
        false,
        false,
        readiness.blocking_field_names(true),
    };
}

inline Rtx4000AdaHardwareDescriptor
make_rtx4000_ada_admitted_hardware_descriptor_a3b3() {
    const auto readiness = make_rtx4000_ada_profile_readiness_decision_a3b3();
    return {
        "rtx4000-ada-m7-exact-profile",
        "NVIDIA RTX 4000 Ada Generation",
        "GPU-18ace299-5348-e6e4-d48c-1ee5a602859b",
        8,
        9,
        kRtx4000AdaNumSms,
        40 * 1024 * 1024,
        2175.0,
        360.04,
        false,
        false,
        false,
        readiness.blocking_field_names(true),
    };
}

// Configuration for the simulator
struct SimulatorConfig {
    std::string architecture_profile_name;
    std::string architecture_parameter_source;
    // True means every field needed to execute is populated; it does not claim
    // hardware accuracy or independent calibration.
    bool architecture_profile_runnable;
    int warp_count;
    int tensor_core_throughput;
    int tensor_core_latency_cycles;
    int simd_throughput;
    int simd_latency_cycles;
    int sfu_throughput;
    int sfu_latency_cycles;
    int shfl_latency_cycles;
    int ls_throughput_bytes;
    int ls_latency_cycles;
    int sram_latency_cycles;
    int sram_bandwidth_bytes_per_cycle;
    int sram_queue_depth;
    int tensor_core_width;
    int simd_width;
    int sfu_width;
    int ls_width_bytes;
    int num_sms;
    int max_concurrent_blocks_per_sm;  // Maximum concurrent thread blocks per SM
    bool O2_dynamic_allocation;
    bool aggressive_store_coalesce;
    bool silence_mode;
    SchedulePolicy schedule_policy;
    WorkloadType workload_type;

    // L2 cache configuration
    int l2_cache_size_bytes;
    int l2_line_size_bytes;
    L2GeometryConfig l2_geometry;
    int l2_hit_latency_cycles;
    int l2_miss_penalty_cycles;
    int l2_bandwidth_bytes_per_cycle;
    int l2_write_bandwidth_bytes_per_cycle;
    int l2_queue_depth;
    int l2_max_transactions_per_cycle_per_sp;
    bool l2_bypass_cache;

    // Optional modeled per-SM L1.  BYPASS preserves all historical results.
    PerSmL1Config per_sm_l1;

    // DRAM configuration
    long long dram_size_bytes;
    double dram_bandwidth_gbps;
    double core_frequency_mhz;
    double dram_frequency_mhz;

    // Explicit model semantics. Defaults preserve every historical run. Exact
    // modes are opt-in and serialize their rational rates into run provenance.
    MemoryModelSemantics memory_model_semantics{};
    TensorIssueWorkSemantics tensor_issue_work_semantics =
        TensorIssueWorkSemantics::LEGACY_OUTPUT_ELEMENTS;

    // Cluster NoC configuration (DSM)
    int cluster_size;
    int cluster_noc_bandwidth_bytes_per_cycle;
    int cluster_noc_latency_cycles;

    // Bulk-copy configuration. The three tma_* fields preserve the sealed
    // Hopper API. Ada uses exact rational rates and remains fail-closed until
    // its setup/command timing and sector behavior are admitted.
    int tma_setup_latency_cycles;
    int tma_issue_interval_cycles;
    int tma_issue_rate_bytes_per_cycle;
    BulkCopyMechanism bulk_copy_mechanism;
    std::uint64_t bulk_copy_per_source_rate_numerator_bytes;
    std::uint64_t bulk_copy_per_source_rate_denominator_cycles;
    std::uint64_t bulk_copy_aggregate_sm_rate_numerator_bytes;
    std::uint64_t bulk_copy_aggregate_sm_rate_denominator_cycles;
    bool bulk_copy_timing_complete;

    // Tensor Memory (Blackwell/UMMA) configuration
    int tmem_cols_per_cta;
    int tmem_issue_interval_cycles;
    int tmem_read_latency_cycles;
    int tmem_read_bandwidth_bytes_per_cycle;
    int tmem_write_latency_cycles;
    int tmem_write_bandwidth_bytes_per_cycle;
    int tmem_queue_depth;

    // Startup delay (cycles) before any node can issue
    int startup_delay_cycles;
    // Delay (cycles) before scheduling a new TB after a TB completes (per SM)
    int block_schedule_latency_cycles;

    // ========================================================================
    // NoC configuration — H100 DSMEM reference parameters
    // ========================================================================
    //
    // Source: "Dissecting the NVIDIA Hopper Architecture through
    //          Microbenchmarking" (arXiv 2501.12084, 2025)
    //
    // H100 DSMEM measured data:
    //   Local SMEM access:             33 cycles
    //   SM-to-SM read (cluster_size=2): 181 cycles
    //   SM-to-SM read (cluster_size=16): 184-213 cycles
    //   Aggregate bandwidth:           3.28 TB/s (@cluster_size=2)
    //   Cluster size:                  16 SMs per GPC
    //   Near-flat latency across cluster sizes → suggests crossbar topology
    //
    // ---- Crossbar defaults ----
    //
    // IMPORTANT: 3.28 TB/s is AGGREGATE GPU bandwidth (all ~7 GPCs).
    // Per-GPC crossbar bandwidth = 3.28 / 7 ≈ 0.469 TB/s = 237 B/cyc.
    //
    // Latency decomposition (read path, cluster_size=2):
    //   total = sram_overhead + 2 × noc_one_way
    //   181 = 33 + 2 × 74  →  noc_latency = 74 cycles (one-way)
    //   sram_latency = 33 cycles (local DSMEM interface + SRAM access)
    //
    // Bandwidth (per-GPC):
    //   3.28 TB/s / 7 GPCs @ 1980MHz = 0.469e12 / 1.98e9 ≈ 237 bytes/cycle
    //
    // ---- Mesh equivalent (what-if analysis) ----
    //
    // Mesh per-hop latency: 3 cycles
    //   Based on 2-stage router pipeline (standard academic baseline).
    //   Ref: "Crossbar NoCs Are Scalable Beyond 100 Nodes"
    //        (Passas et al., IEEE TCAD 2012)
    //
    // Mesh per-link bandwidth: 30 bytes/cycle (for 4×4 mesh)
    //   Derived from bisection bandwidth equivalence:
    //     Crossbar bisection BW = 237 / 2 = 118.5 bytes/cycle
    //     Mesh bisection (4×4):  k = 4 links cross the cut
    //     4 × b_link = 118.5  →  b_link ≈ 30 bytes/cycle
    //
    // Mesh base latency (DSMEM protocol overhead): 60 cycles
    //   Dominates total latency — same as crossbar. Measured crossbar
    //   latency barely changes with cluster_size (181 vs 184-213),
    //   confirming protocol overhead >> network traversal.
    //
    // Mesh per-hop latency: 5 cycles
    //   Realistic 5-stage router pipeline (BW+RC+VA+SA+ST/LT).
    //
    // Mesh latency model: base(60) + hops × per_hop(5)
    //
    // Resulting end-to-end latencies (SM-to-SM read = sram + 2×noc_one_way):
    //                    Mesh one-way    Crossbar(74)  Round-trip comparison
    //   1 hop (neighbor):  60+1×5 = 65    74          Mesh:33+2×65=163  CB:181
    //   2 hops:            60+2×5 = 70    74          Mesh:33+2×70=173  CB:181
    //   3 hops:            60+3×5 = 75    74          Mesh:33+2×75=183  CB:181
    //   6 hops (diagonal): 60+6×5 = 90    74          Mesh:33+2×90=213  CB:181
    //   avg (3.33 hops):   60+17  = 77    74          Nearly identical
    //
    // Key insight: mesh vs crossbar difference is mainly in BANDWIDTH
    // contention, not latency. Near-neighbor traffic avoids far links.
    // ========================================================================
    bool noc_enabled;
    NoCTopology noc_topology;
    int noc_latency_cycles;           // Crossbar: one-way latency; Mesh: per-hop latency
    int noc_bandwidth_bytes_per_cycle; // Per-GPC aggregate bandwidth (bytes/cycle at core freq)
    int noc_queue_depth;
    int noc_mesh_rows;                // Mesh: grid rows (rows × cols >= sms_per_cluster)
    int noc_mesh_cols;                // Mesh: grid columns
    int noc_sms_per_cluster;          // SMs per cluster/GPC (0 = no cluster restriction)
    int noc_link_bandwidth_bytes_per_cycle;  // Mesh: per-link bandwidth (0 = use aggregate)
    int noc_mesh_base_latency_cycles; // Mesh: fixed injection+ejection overhead (DSMEM protocol)
    std::vector<int> noc_mesh_sm_mapping; // Mesh: logical SM → physical node mapping (empty = identity)

    SimulatorConfig()
        : architecture_profile_name("legacy-constructor-default"),
          architecture_parameter_source("unqualified legacy constructor defaults"),
          architecture_profile_runnable(false),
          warp_count(4),
          tensor_core_throughput(256),  // Tensor ops per instruction
          tensor_core_latency_cycles(48),
          simd_throughput(32),
          simd_latency_cycles(4),
          sfu_throughput(32),
          sfu_latency_cycles(30),
          shfl_latency_cycles(4),
          ls_throughput_bytes(128),
          ls_latency_cycles(10),
          sram_latency_cycles(33),           // H100 local DSMEM access (microbenchmark)
          sram_bandwidth_bytes_per_cycle(128),
          sram_queue_depth(64),
          tensor_core_width(32),
          simd_width(32),
          sfu_width(4),
          ls_width_bytes(128),
          num_sms(1),
          max_concurrent_blocks_per_sm(4),  // Default: 4 concurrent blocks
          O2_dynamic_allocation(false),
          aggressive_store_coalesce(false),
          silence_mode(false),
          schedule_policy(SchedulePolicy::GTO),
          workload_type(WorkloadType::GEMM),
          l2_cache_size_bytes(40 * 1024 * 1024),
          l2_line_size_bytes(128),
          l2_hit_latency_cycles(300),
          l2_miss_penalty_cycles(566 - 300),
          l2_bandwidth_bytes_per_cycle(1853),
          l2_write_bandwidth_bytes_per_cycle(0),
          l2_queue_depth(65536),
          l2_max_transactions_per_cycle_per_sp(1),
          l2_bypass_cache(false),
          dram_size_bytes(80LL * 1024 * 1024 * 1024), // H100 SXM typical 80GB
          dram_bandwidth_gbps(1407.0), // 1407
          core_frequency_mhz(1410.0),
          dram_frequency_mhz(1215.0),
          cluster_size(0),
          cluster_noc_bandwidth_bytes_per_cycle(0),
          cluster_noc_latency_cycles(0),
          tma_setup_latency_cycles(200),
          tma_issue_interval_cycles(60),
          tma_issue_rate_bytes_per_cycle(100),
          bulk_copy_mechanism(BulkCopyMechanism::HOPPER_TMA),
          bulk_copy_per_source_rate_numerator_bytes(0),
          bulk_copy_per_source_rate_denominator_cycles(0),
          bulk_copy_aggregate_sm_rate_numerator_bytes(0),
          bulk_copy_aggregate_sm_rate_denominator_cycles(0),
          bulk_copy_timing_complete(true),
          tmem_cols_per_cta(256),
          tmem_issue_interval_cycles(1),
          tmem_read_latency_cycles(33),
          tmem_read_bandwidth_bytes_per_cycle(128),
          tmem_write_latency_cycles(33),
          tmem_write_bandwidth_bytes_per_cycle(128),
          tmem_queue_depth(64),
          startup_delay_cycles(0),
          block_schedule_latency_cycles(0),
          // H100 DSMEM defaults (crossbar model, per-GPC)
          noc_enabled(false),
          noc_topology(NoCTopology::CROSSBAR),
          noc_latency_cycles(74),            // Crossbar one-way: (181 - 33) / 2
          noc_bandwidth_bytes_per_cycle(237), // 3.28 TB/s / 7 GPCs @ 1980 MHz
          noc_queue_depth(128),
          noc_mesh_rows(4),                  // 4×4 mesh for 16-SM cluster
          noc_mesh_cols(4),
          noc_sms_per_cluster(16),           // H100: 16 SMs per GPC
          noc_link_bandwidth_bytes_per_cycle(30), // Bisection equiv: 237/2/4 ≈ 30
          noc_mesh_base_latency_cycles(60)   // DSMEM protocol overhead (dominant term)
          {
            if (l2_write_bandwidth_bytes_per_cycle <= 0) {
                l2_write_bandwidth_bytes_per_cycle =
                    l2_bandwidth_bytes_per_cycle / 2;
            }
          }

    BulkCopyEngineConfig make_bulk_copy_engine_config() const {
        if (bulk_copy_mechanism == BulkCopyMechanism::HOPPER_TMA) {
            return BulkCopyEngineConfig::hopper_tma(
                tma_setup_latency_cycles,
                tma_issue_interval_cycles,
                tma_issue_rate_bytes_per_cycle);
        }
        if (!bulk_copy_timing_complete) {
            throw std::runtime_error(
                "Ada LDGSTS engine is contract-admitted but timing-incomplete");
        }
        return BulkCopyEngineConfig::ada_ldgsts(
            tma_setup_latency_cycles,
            tma_issue_interval_cycles,
            {bulk_copy_per_source_rate_numerator_bytes,
             bulk_copy_per_source_rate_denominator_cycles},
            {bulk_copy_aggregate_sm_rate_numerator_bytes,
             bulk_copy_aggregate_sm_rate_denominator_cycles});
    }
};

// Shared H100 SXM kernel-simulation profile.  Workload builders must not
// override timing or resource fields: they describe tile-graph semantics only.
inline SimulatorConfig make_h100_sxm_config() {
    SimulatorConfig cfg;
    // Preserve the sealed observer identity emitted by all historical H100
    // native-memory evidence.
    cfg.architecture_profile_name = "make_h100_sxm_config";
    cfg.architecture_parameter_source =
        "historical make_h100_sxm_config profile";
    cfg.architecture_profile_runnable = true;
    cfg.tensor_core_throughput = 128;
    cfg.tensor_core_latency_cycles = 16;
    cfg.tensor_core_width = 32;
    cfg.l2_hit_latency_cycles = 380;
    cfg.l2_miss_penalty_cycles = 270;
    cfg.l2_bandwidth_bytes_per_cycle = 4300;
    cfg.l2_write_bandwidth_bytes_per_cycle = 2150;
    cfg.l2_cache_size_bytes = 50 * 1024 * 1024;
    cfg.l2_bypass_cache = false;
    cfg.dram_frequency_mhz = 2619;
    cfg.core_frequency_mhz = 1980;
    cfg.dram_bandwidth_gbps = 3100;
    cfg.num_sms = kH100SxmNumSms;
    cfg.max_concurrent_blocks_per_sm = 1;
    cfg.O2_dynamic_allocation = false;
    cfg.tma_setup_latency_cycles = 100;
    // The TMA engine itself serializes bulk transfers. One scheduler cycle is
    // the shared command-front-end baseline; no workload bypasses it.
    cfg.tma_issue_interval_cycles = 1;
    cfg.tma_issue_rate_bytes_per_cycle = 100;
    cfg.bulk_copy_mechanism = BulkCopyMechanism::HOPPER_TMA;
    cfg.bulk_copy_timing_complete = true;
    // Kernel simulation starts after launch. CTA reuse is represented by the
    // graph's residency and dependency structure rather than a fitted delay.
    cfg.startup_delay_cycles = 0;
    cfg.block_schedule_latency_cycles = 0;
    return cfg;
}

// Runnable footprint-reference profile for the RTX 4000 Ada target.
//
// This is intentionally separate from the fail-closed exact selector below.
// R77/R84 fields are admitted measurements or exact model semantics.  Every
// remaining execution field is an explicit uncalibrated modeling assumption;
// none is inherited implicitly from the H100 profile.  The profile is suitable
// for structural scheduling and memory-footprint sensitivity, not hardware
// latency, throughput, or TTFT claims.
inline SimulatorConfig make_rtx4000_ada_footprint_reference_config() {
    SimulatorConfig cfg;
    cfg.architecture_profile_name = "gtsim-native-rtx4000-ada";
    cfg.architecture_parameter_source =
        "R77/R84 admitted Ada memory fields plus explicit uncalibrated "
        "footprint-reference scheduler/pipeline/queue assumptions; "
        "PAPER_ADA_L2_V1_WORKFLOW_1 cache geometry/lifetime";
    cfg.architecture_profile_runnable = true;

    // Observed schedule contract for the certified Ada BF16 GEMM is four
    // warps per CTA.  Pipeline throughput/latency values below remain modeled.
    cfg.warp_count = 4;
    cfg.tensor_core_throughput = 256;
    cfg.tensor_core_latency_cycles = 32;
    cfg.tensor_core_width = 32;
    cfg.tensor_issue_work_semantics =
        TensorIssueWorkSemantics::DECLARED_FMA_WORK;
    cfg.simd_throughput = 32;
    cfg.simd_latency_cycles = 4;
    cfg.sfu_throughput = 16;
    cfg.sfu_latency_cycles = 30;
    cfg.shfl_latency_cycles = 4;
    cfg.ls_throughput_bytes = 128;
    cfg.ls_latency_cycles = 10;
    cfg.simd_width = 32;
    cfg.sfu_width = 4;
    cfg.ls_width_bytes = 128;

    cfg.num_sms = kRtx4000AdaNumSms;
    cfg.max_concurrent_blocks_per_sm = 4;
    cfg.O2_dynamic_allocation = false;
    cfg.aggressive_store_coalesce = false;
    cfg.schedule_policy = SchedulePolicy::GTO;

    // R77/R84 admitted cache and DRAM model fields.
    cfg.l2_cache_size_bytes = 40 * 1024 * 1024;
    cfg.l2_line_size_bytes = 128;
    cfg.l2_geometry = L2GeometryConfig::paper_ada_l2_v1();
    cfg.l2_hit_latency_cycles = 272;
    cfg.l2_miss_penalty_cycles = 604;
    cfg.l2_bandwidth_bytes_per_cycle = 711;
    cfg.l2_write_bandwidth_bytes_per_cycle = 540;
    cfg.l2_queue_depth = 65536;                 // modeled
    cfg.l2_max_transactions_per_cycle_per_sp = 1;  // modeled
    cfg.l2_bypass_cache = false;
    cfg.memory_model_semantics =
        make_rtx4000_ada_a3b4_memory_model_semantics();
    // Match the observed visible-memory census and runtime placement guard.
    // The nominal 20 GB product label is not an addressable capacity.
    cfg.dram_size_bytes = 20475LL * 1024 * 1024;
    cfg.dram_bandwidth_gbps = 311.660957;
    cfg.core_frequency_mhz = 2243.3277391728507;
    // Exact rational DRAM service does not consume a DRAM-clock conversion.
    cfg.dram_frequency_mhz = 0.0;

    // R77 observed the steady-state LDGSTS rates.  Setup and command interval
    // are still unobserved and are explicitly modeled as 0/1 for runnable
    // footprint scheduling.  Ada stores in the new builder use direct global
    // stores and do not claim a cp.async/TMA store path.
    cfg.bulk_copy_mechanism = BulkCopyMechanism::ADA_LDGSTS;
    cfg.bulk_copy_per_source_rate_numerator_bytes = 4537170818ULL;
    cfg.bulk_copy_per_source_rate_denominator_cycles = 1000000000ULL;
    cfg.bulk_copy_aggregate_sm_rate_numerator_bytes = 11704224357ULL;
    cfg.bulk_copy_aggregate_sm_rate_denominator_cycles = 1000000000ULL;
    cfg.tma_setup_latency_cycles = 0;            // modeled
    cfg.tma_issue_interval_cycles = 1;           // modeled
    cfg.tma_issue_rate_bytes_per_cycle = 1;      // unused by exact Ada rates
    cfg.bulk_copy_timing_complete = true;        // runnable model, not calibrated

    cfg.sram_latency_cycles = 21;
    cfg.sram_bandwidth_bytes_per_cycle = 63;
    cfg.sram_queue_depth = 64;                   // modeled
    cfg.startup_delay_cycles = 0;
    cfg.block_schedule_latency_cycles = 0;
    cfg.noc_enabled = false;

    cfg.per_sm_l1.mode = PerSmL1Mode::MODELED_SET_ASSOCIATIVE;
    cfg.per_sm_l1.persistence =
        PerSmL1Persistence::KERNEL_FLUSH;
    cfg.per_sm_l1.num_sms = kRtx4000AdaNumSms;
    cfg.per_sm_l1.capacity_bytes_per_sm = 32ULL * 1024ULL;
    cfg.per_sm_l1.ways = 64;
    cfg.per_sm_l1.line_bytes = 128;
    cfg.per_sm_l1.store_bypass = true;
    cfg.per_sm_l1.hit_latency_cycles = 7;
    return cfg;
}

// A2 only establishes typed exact-profile identity.  It must never obtain a
// runnable exact Ada profile by copying unresolved values from this modeled
// reference or from H100.
inline SimulatorConfig make_native_architecture_config(
    const std::string& architecture_profile_name) {
    if (architecture_profile_name.empty() ||
        architecture_profile_name == "h100-sxm" ||
        architecture_profile_name == "make_h100_sxm_config") {
        return make_h100_sxm_config();
    }
    if (architecture_profile_name == "gtsim-native-rtx4000-ada" ||
        architecture_profile_name == "rtx4000-ada-footprint-reference") {
        // Keep the former name as an input alias only.
        return make_rtx4000_ada_footprint_reference_config();
    }
    if (architecture_profile_name == "rtx4000-ada" ||
        architecture_profile_name == "rtx4000-ada-m7-exact-profile") {
        const auto readiness = make_rtx4000_ada_profile_readiness_decision();
        throw std::runtime_error(
            "RTX 4000 Ada hardware identity is admitted, but its runnable "
            "GTSim profile is A3B4 fail-closed with " +
            std::to_string(readiness.execution_blocker_count) +
            " execution blockers; first=" +
            readiness.first_blocking_field(false));
    }
    throw std::invalid_argument(
        "unknown GTSim architecture profile: " + architecture_profile_name);
}

inline SimulatorConfig make_native_architecture_config_from_environment() {
    const char* selected = std::getenv("GTSIM_ARCHITECTURE_PROFILE");
    return make_native_architecture_config(selected == nullptr ? "h100-sxm"
                                                               : selected);
}

// Convert peak throughput in TB/s to simulator bytes/cycle at core clock.
inline int bytes_per_cycle_from_tbps(double tbps, double core_frequency_mhz) {
    if (tbps <= 0.0 || core_frequency_mhz <= 0.0) return 0;
    return static_cast<int>(std::lround((tbps * 1e12) / (core_frequency_mhz * 1e6)));
}

// Apply Blackwell B200 defaults from the paper evaluation configuration.
// - HBM bandwidth: up to 8 TB/s per GPU (NVIDIA HGX/DGX B200 docs).
// - TMEM peak: 12 TB/s and latency: 28 cycles.
inline void apply_blackwell_b200_defaults(SimulatorConfig& cfg,
                                          double tmem_peak_tbps = 12.0) {
    cfg.core_frequency_mhz = 1965.0;
    cfg.dram_frequency_mhz = 2000.0;
    cfg.dram_size_bytes = 192LL * 1024 * 1024 * 1024; // 192GB HBM3e
    cfg.dram_bandwidth_gbps = 8000.0; // 8 TB/s
    cfg.num_sms = 148;
    cfg.l2_cache_size_bytes = 50 * 1024 * 1024; // 50MB
    cfg.l2_hit_latency_cycles = 380;
    cfg.l2_miss_penalty_cycles = 270; // Used as DRAM miss penalty in this model.
    cfg.l2_bandwidth_bytes_per_cycle = 6000;
    cfg.l2_write_bandwidth_bytes_per_cycle = 3000;
    cfg.tma_issue_rate_bytes_per_cycle = 100;
    cfg.tma_setup_latency_cycles = 100;
    cfg.tma_issue_interval_cycles = 200;
    cfg.bulk_copy_mechanism = BulkCopyMechanism::HOPPER_TMA;
    cfg.bulk_copy_timing_complete = true;

    int tmem_bw_bpc = bytes_per_cycle_from_tbps(tmem_peak_tbps, cfg.core_frequency_mhz);
    if (tmem_bw_bpc > 0) {
        cfg.tmem_read_bandwidth_bytes_per_cycle = tmem_bw_bpc;
        cfg.tmem_write_bandwidth_bytes_per_cycle = tmem_bw_bpc;
    }
}

// Main simulator class
class Simulator {
public:
    SimulatorConfig config;
    GPU* simulated_gpu;
    DAG* dag;
    Cycle cycle;
    std::ofstream* log_file;
    std::ostream* output_stream;
    CtaSparseSlots<std::uint8_t> global_scoreboard;
    CtaSparseSlots<Subpartition*> node_id_to_sp;
    CtaSparseSlots<int> node_id_to_sm;
    std::size_t completed_node_count;

    Simulator(DAG* input_dag, const SimulatorConfig& cfg = SimulatorConfig(),
              L2Cache* shared_l2 = nullptr, Cycle initial_cycle = 0)
        : config(cfg), dag(input_dag), cycle(std::max<Cycle>(0, initial_cycle)),
          log_file(nullptr), output_stream(&std::cout),
          completed_node_count(0) {

        if (dag->cta_graph_store && (!dag->nodes.empty() || !dag->nodes_dict.empty() ||
            !dag->nodes_by_name.empty() || config.noc_enabled))
            throw std::logic_error("F1 empty carrier/NoC scope");
        int max_warp_id = dag->cta_graph_store ? dag->cta_graph_store->warp_count()-1 : -1;
        for (auto* node : dag->nodes) {
            max_warp_id = std::max(max_warp_id, node->warp_id);
        }
        if (max_warp_id >= 0) {
            config.warp_count = max_warp_id + 1;
        }

        if (config.cluster_size > 0) {
            int active_sms = (config.num_sms / config.cluster_size) * config.cluster_size;
            if (active_sms <= 0) {
                active_sms = 0;
            }
            config.num_sms = active_sms;
        }

        int dram_bandwidth_bytes_per_cycle = 0;
        if (config.memory_model_semantics.dram_bandwidth ==
            DramBandwidthSemantics::LEGACY_INTEGER_DRAM_CYCLE_WITH_FREQUENCY_RATIO) {
            if (config.dram_bandwidth_gbps <= 0.0 ||
                config.dram_frequency_mhz <= 0.0 ||
                config.core_frequency_mhz <= 0.0) {
                throw std::invalid_argument(
                    "legacy DRAM conversion requires positive bandwidth and frequencies");
            }
            dram_bandwidth_bytes_per_cycle = static_cast<int>(
                std::lround((config.dram_bandwidth_gbps * 1e9) /
                            (config.dram_frequency_mhz * 1e6)));
        }

        simulated_gpu = new GPU(config.num_sms, config.warp_count,
                               config.tensor_core_throughput,
                               config.simd_throughput,
                               config.sfu_throughput,
                               config.ls_throughput_bytes,
                               config.tensor_core_latency_cycles,
                               config.simd_latency_cycles,
                               config.sfu_latency_cycles,
                               config.shfl_latency_cycles,
                               config.ls_latency_cycles,
                               config.tensor_core_width,
                               config.simd_width,
                               config.sfu_width,
                               config.ls_width_bytes,
                               config.sram_latency_cycles,
                               config.sram_bandwidth_bytes_per_cycle,
                               config.sram_queue_depth,
                               config.O2_dynamic_allocation,
                               config.schedule_policy,
                               config.max_concurrent_blocks_per_sm,
                               config.l2_cache_size_bytes,
                               config.l2_line_size_bytes,
                               config.l2_hit_latency_cycles,
                               config.l2_bandwidth_bytes_per_cycle,
                               config.l2_write_bandwidth_bytes_per_cycle,
                               config.l2_queue_depth,
                               config.l2_max_transactions_per_cycle_per_sp,
                               config.l2_bypass_cache,
                               dram_bandwidth_bytes_per_cycle,
                               config.l2_miss_penalty_cycles,
                               config.core_frequency_mhz,
                               config.dram_frequency_mhz,
                               config.make_bulk_copy_engine_config(),
                               config.tmem_cols_per_cta,
                               config.tmem_issue_interval_cycles,
                               config.tmem_read_latency_cycles,
                               config.tmem_read_bandwidth_bytes_per_cycle,
                               config.tmem_write_latency_cycles,
                               config.tmem_write_bandwidth_bytes_per_cycle,
                               config.tmem_queue_depth,
                                config.startup_delay_cycles,
                                config.block_schedule_latency_cycles,
                                shared_l2,
                                config.memory_model_semantics,
                                config.per_sm_l1,
                                config.l2_geometry);

        try {
        for (auto* sm_inst : simulated_gpu->sms) {
            for (auto* sp : sm_inst->sps) {
                for (auto* pipe : sp->pipelines) {
                    if (pipe->pipeline_name == "Tensor") {
                        pipe->tensor_issue_work_semantics =
                            config.tensor_issue_work_semantics;
                    }
                }
            }
        }

        if (dag->cta_graph_store) {
            dag->cta_graph_store->initialize(simulated_gpu,&global_scoreboard,
                &node_id_to_sp,&node_id_to_sm,&completed_node_count);
        } else {
        // Initialize schedulers with nodes
        int total_nodes = static_cast<int>(dag->nodes.size());
        dag->build_dependency_graph();

        global_scoreboard.assign(total_nodes, 0);

        node_id_to_sp.assign(total_nodes, nullptr);
        node_id_to_sm.assign(total_nodes, -1);

        for (auto* sm_inst : simulated_gpu->sms) {
            for (auto* sp : sm_inst->sps) {
                // Assign warps to subpartitions: warp_id % 4 == sp.subpartition_id
                std::vector<DAGNode*> sp_nodes;
                for (auto* node : dag->nodes) {
                    if (node->warp_id % 4 == sp->subpartition_id) {
                        // Assign node to this SM based on sm_id or thread_block_id
                        if (node->sm_id == -1 || node->sm_id == sm_inst->sm_id) {
                            sp_nodes.push_back(node);
                            if (node->id >= 0 && node->id < total_nodes) {
                                node_id_to_sp[node->id] = sp;
                                node_id_to_sm[node->id] = sm_inst->sm_id;
                            }
                        }
                    }
                }
                sp->sp_scheduler->init_with_nodes(sp_nodes, total_nodes, sp->subpartition_id,
                                                  &global_scoreboard);
                sp->set_node_to_sp_map(&node_id_to_sp);
                sp->set_completed_node_counter(&completed_node_count);
            }
        }
        simulated_gpu->set_node_to_sp_map(&node_id_to_sp);
        simulated_gpu->set_node_to_sm_map(&node_id_to_sm);

        }
        // Initialize NoC if enabled
        if (config.noc_enabled) {
            int cluster_size = config.noc_sms_per_cluster;  // 0 = no restriction
            if (config.noc_topology == NoCTopology::MESH) {
                int rows = config.noc_mesh_rows;
                int cols = config.noc_mesh_cols;
                // Auto-compute mesh dimensions if not specified
                // When clustering is active, mesh covers one cluster; otherwise all SMs
                if (rows <= 0 || cols <= 0) {
                    int mesh_sms = (cluster_size > 0) ? cluster_size : config.num_sms;
                    cols = static_cast<int>(std::ceil(std::sqrt(mesh_sms)));
                    rows = (mesh_sms + cols - 1) / cols;
                }

                // Mesh per-hop latency: noc_latency_cycles is used directly as
                // per_hop when topology is MESH (typically 2-3 cycles for a
                // simple router pipeline, vs 88 cycles for crossbar one-way).
                // User should set noc_latency_cycles appropriately per topology.

                // Derive per-link bandwidth from aggregate bandwidth using
                // bisection equivalence: b_link = (N/2)/k × (BW_total/N)
                // For simpler default: aggregate / num_links
                int num_links = rows * (cols - 1) + (rows - 1) * cols;
                int link_bw = config.noc_link_bandwidth_bytes_per_cycle;
                if (link_bw <= 0 && num_links > 0) {
                    link_bw = config.noc_bandwidth_bytes_per_cycle / num_links;
                }

                simulated_gpu->noc = new MeshNoC(
                    config.noc_mesh_base_latency_cycles,
                    config.noc_latency_cycles,
                    config.noc_bandwidth_bytes_per_cycle,
                    config.noc_queue_depth,
                    rows, cols, cluster_size,
                    link_bw,
                    config.noc_mesh_sm_mapping);
            } else {
                simulated_gpu->noc = new CrossbarNoC(
                    config.noc_latency_cycles,
                    config.noc_bandwidth_bytes_per_cycle,
                    config.noc_queue_depth,
                    cluster_size);
            }
            for (auto* sm : simulated_gpu->sms) {
                sm->set_noc(simulated_gpu->noc);
            }
        }

        if (!dag->cta_graph_store) {
        // Initialize thread block residency scheduler for each SM
        for (auto* sm_inst : simulated_gpu->sms) {
            std::vector<DAGNode*> sm_nodes;
            for (auto* node : dag->nodes) {
                if (node->sm_id == -1 || node->sm_id == sm_inst->sm_id) {
                    sm_nodes.push_back(node);
                }
            }
            sm_inst->initialize_tb_scheduler(sm_nodes);
        }
        }
        } catch (...) {
            if (dag->cta_graph_store) dag->cta_graph_store->detach();
            delete simulated_gpu; simulated_gpu=nullptr;
            throw;
        }
    }

    ~Simulator() {
        if (dag->cta_graph_store) dag->cta_graph_store->detach();
        delete simulated_gpu;
        if (log_file) {
            log_file->close();
            delete log_file;
        }
    }

    // Set log file for output
    bool set_log_file(const std::string& filename) {
        log_file = new std::ofstream(filename);
        if (!log_file->is_open()) {
            delete log_file;
            log_file = nullptr;
            return false;
        }
        output_stream = log_file;
        return true;
    }

    std::uint64_t host_step_calls = 0, host_skipped_cycles = 0;
    // Step simulation by one cycle, or to the next causally relevant event.
    void step(Cycle cycle_limit=std::numeric_limits<Cycle>::max()) {
        ++host_step_calls;
        auto* port=simulated_gpu->l2->whole_tile_port;
        const Cycle interval_start=cycle;
        if (port && port->skip_idle_cycles() && !simulated_gpu->noc) {
            simulated_gpu->initialize_events(cycle);
            const Cycle local = std::min(cycle_limit, simulated_gpu->next_event_cycle());
            Cycle next = cycle_add(cycle,1);
            if (local > next) next = std::max(next, std::min(local, port->next_idle_event_cycle(local)));
            const auto delta = static_cast<std::uint64_t>(next-cycle);
            port->account_memory_blocked_warps(simulated_gpu->event_blocked_warps*delta);
            host_skipped_cycles += delta-1;
            cycle = next;
            simulated_gpu->step(cycle);
            if(dag->cta_graph_store)dag->cta_graph_store->collect(cycle);
            return;
        }
        std::uint64_t blocked=0;
        if(port)for(auto* sm:simulated_gpu->sms)for(auto* sp:sm->sps)
            for(int w:sp->sp_scheduler->resident_warps)
                if(sp->sp_scheduler->ready_warp[w].empty() && port->warp_waiting(sm->sm_id,w))++blocked;
        bool idle=port && port->skip_idle_cycles() && !simulated_gpu->noc;
        if (idle) for (auto* sm:simulated_gpu->sms) {
            if(sm->tma->has_work() || !sm->waiting_tb_queue.empty() ||
               !sm->sram->memory_queue.empty() || !sm->sram->memory_write_queue.empty() ||
               !sm->tmem_mem->memory_queue.empty() || !sm->tmem_mem->memory_write_queue.empty()) { idle=false;break; }
            for(auto* sp:sm->sps) {
                if(!sp->barrier_queue.empty() || !sp->retry_queue_sram.empty() ||
                   !sp->retry_queue_l2.empty() || !sp->deferred_tmem_pair_completions.empty())idle=false;
                for(auto* p:sp->pipelines)if(!p->pipeline_executing_nodes.empty())idle=false;
                for(int w:sp->sp_scheduler->resident_warps)
                    if(!sp->sp_scheduler->ready_warp[w].empty())idle=false;
            }
            if(!idle)break;
        }
        if(idle) {
            Cycle next=port->next_idle_event_cycle(cycle_limit);
            if(next>cycle+1 && next<std::numeric_limits<Cycle>::max())cycle=std::min(next,cycle_limit)-1;
        }
        host_skipped_cycles += cycle-interval_start;
        cycle++;
        if(port)port->account_memory_blocked_warps(blocked*static_cast<std::uint64_t>(cycle-interval_start));
        simulated_gpu->step(cycle);
        if(dag->cta_graph_store)dag->cta_graph_store->collect(cycle);
    }

    // Run simulation until completion or timeout
    bool run(Cycle max_cycles = 500000, bool verbose = true);

    // Print execution results
    void print_results();

private:
    void print_configuration(bool verbose);
    void print_statistics(bool verbose);
};

} // namespace GTSim

#endif // SIMULATOR_H
