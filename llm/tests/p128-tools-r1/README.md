# P128 pair preparation and safe status

Owned scripts: `llm/tools/p128_prepare_xmu.py`, `p128_safe_status_xmu.py`. These add no cache/producer implementation changes. Local synthetic validation: 11 tests PASS; receipt records exact sources. No remote operation performed by this subtask.

## Admission JSON

Required fields:

```json
{
  "schema": "ADMITTED_P128_CASE_V1",
  "case_id": "qwen-p128d16",
  "model_key": "qwen25_1p5b",
  "prefill_length": 128,
  "decode_steps": 16,
  "graph": "/absolute/graph.json",
  "registry": "/absolute/registry.json",
  "runtime": "/absolute/P128/support/runtime.py",
  "native_tree": "/absolute/native/tree",
  "support_tree": "/absolute/P128/support",
  "launch_resources": "/absolute/per-launch-resource.json",
  "source_pins": [{"path":"/absolute/source", "bytes":1, "sha256":"64 lowercase hex characters"}],
  "expected_counts": {"kernels":0,"memory_APIs":0,"allocation_observations":0,"phases":34}
}
```

The example is structural, not runnable evidence: actual pins/counts are mandatory; kernels must be positive. Decode whitelist is 2/4/8/16. Case ID accepts only safe alphanumeric/underscore/hyphen names. Graph/registry/runtime and graph.artifacts.nodes must occur in admitted source pins. Contract must be Qwen B1 BF16, one warmup, retained sampling, no feedback/CUDA graph. Nodes/counts/phase order and complete initialization+warmup+measured launch resources are verified. Sidecar schema is OBSERVED_LAUNCH_CARVEOUT_V1, graph SHA exact, all and only native launch IDs, observed bins8/16/32/64/100KiB.

## Prepare command and outputs

Run under the existing shared CPU controller (one CPU lease); the script does not acquire a lease itself:

```text
/usr/bin/python3 -B ROOT/repo/llm/tools/p128_prepare_xmu.py --root ROOT --admission CASE_ADMISSION.json --cpu-r2 8 --cpu-r4 9
```

ROOT is exactly `/home/xmu/nvidiagds/codex-runs/gtsim-ada-r4-p128-20260922-r1`. Preparation reads the frozen old `build-qwen/source-cache-runner` and verifies its binary plus12 key source pins against old `qwen-r2-spec.json`; no compilation. New producer is pinned separately. The only child is producer `--preflight-only`, with `--fast-gemv --fast-prefill`; no cache execution. Success directory:

```text
ROOT/cases/CASE_ID/
  preflight/status.json
  preflight.log
  r2-spec.json
  r4-spec.json
  prepare-result.json
```

Run outputs are `r2/` and `r4/`; use adjacent `r2-job/` and `r4-job/` for original run_job lifecycle. Each spec GPU=null,43200s,16GiB,one CPU in0..15. CPUs must differ within a pair. Root must coordinate all leases/total concurrency with existing8B tasks; this preparer starts none. Fixed h288/65536,dirty-age64M,observed-only L1 resource mode,thread counts1. Same frozen binary retains128B fill/RFO and32B WB. Failed/fresh directories are never overwritten. Only one successful prepared case per decode length is accepted.

`prepare-result.json` includes expected_counts, actual graph/resource/producer/binary pins,source_pin_count and complete graph input_contract for later pairing. Input IDs/model paths remain on XMU and are not emitted by safe status.

## Safe status / optional NCU

```text
/usr/bin/python3 -B ROOT/repo/llm/tools/p128_safe_status_xmu.py --root ROOT
```

Outputs P128_LLM_R2_R4_SAFE_AGGREGATES_V1 with exactly8 D/profile arms. Missing cases/NCU remain NOT_STARTED/NOT_READY and null, never zero. Per-arm preparation exports expected_counts,source_pin_count,graph_sha256,launch_resources_sha256 and preflight status; runtime exports whitelisted counters,CPU/wall,completed phases,actual L1 configuration histograms and fixed L2 configuration. No source records,raw addresses,kernel names,error messages,contract token IDs or input paths are exported.

Optional reference file is `ROOT/cases/CASE_ID/ncu-result.json`. Accepts existing `ROI_rows` with top-level dram_* fields or nested `ncu` same fields; conflicting values reject. Reference input_contract must equal the graph contract stored in prepare receipt (including B1/BF16/warmup/prompt/decode controls), metadata_actual_controls_compared must be true, and status must start PASS_CLOSED_RAW_NCU_. Otherwise ROI_rows=null with an explicit pairing status. Exact decimal duration strings convert to integer; absent duration stays null. References and simulation progress are independent; independent NCU ROI windows are never added to reconstruct Full. This exporter is not the final numerical acceptance checker.

## Test scope

11 local tests cover all four phase counts; graph/source mutation; missing/extra/duplicate/mismatched resources; CPU bounds and fixed flags; existing binary/source pin mutation; preparation-only subprocess/no overwrite; eight pending arms; sensitive sentinel removal; nested/top-level NCU parity and actual-controls/input mismatch rejection; preparation count/hash-only export. Synthetic fixtures do not imply native full-run accuracy.
