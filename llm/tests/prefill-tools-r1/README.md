# Prefill sweep preparation and safe status

These two independent tools handle **new Qwen2.5-1.5B B1 BF16 P64/P256/P512 D2 r4 runs only**. P32/P128 reuse belongs to the main report and cannot be launched here. Every new case has its own complete six-phase Warmup + Measured history. No prepare operation compiles, launches a cache replay, or starts a GPU job.

The exact remote root is `/home/xmu/nvidiagds/codex-runs/gtsim-ada-r4-prefill-20260922-r1`. All preparation, source, and binary paths are checked locally on XMU; this tool does not export them. Run the preparer under the original shared CPU lease controller:

```sh
python3 -B repo/llm/tools/prefill_prepare_xmu.py --root "$TASK_ROOT" --admission /absolute/admission.json --cpu 0
python3 -B repo/llm/tools/prefill_safe_status_xmu.py --root "$TASK_ROOT"
```

`--cpu` is one explicit 0..15 CPU. Intended scheduling is P64=0, P256=1, P512=9, but availability and acquisition belong to the outer shared lease controller. Generated specs are r4 only, GPU=null, 43,200 seconds, 16 GiB, one thread; `--fast-gemv --fast-prefill` and no stateful Down option. They explicitly set observed-only L1, r4, h=288, dirty age=64,000,000. The qualified runner retains 128 B fill/RFO, 32 B writeback, and no final flush; this preparer never changes cache code or establishes timing-model equivalence.

## Admission and runner provenance

`ADMITTED_PREFILL_CASE_V1` contains `case_id`, `model_key=qwen25_1p5b`, `prefill_length` in 64/256/512, `decode_steps=2`; absolute `graph`, `registry`, `runtime`, `native_tree`, `support_tree`, `launch_resources`, **`runner`**; input `source_pins`; and `expected_counts={kernels,memory_APIs,allocation_observations,phases:6}`. `runner_evidence` is a pin of the JSON below. Every pin is exactly `{path:absolute,bytes:integer,sha256:lowercase SHA256}`.

```json
{
  "schema": "PREFILL_NATIVE_RUNNER_EVIDENCE_V1",
  "status": "PASS_BUILD_AND_SOURCE_CASE_VALIDATION",
  "binary": {"path": "...", "bytes": 0, "sha256": "..."},
  "sources": [{"path": "...", "bytes": 0, "sha256": "..."}],
  "complete_compilation_dependencies": true,
  "build_receipt": {"path": "...", "bytes": 0, "sha256": "..."},
  "case_validation": {"path": "...", "bytes": 0, "sha256": "..."}
}
```

Illustrative ellipses/zero sizes are schema placeholders, never admissible production evidence. `sources` must contain the complete compiled source dependency set, not just the eight required core files. The guard requires the entry point, runner, cache, L1 adapter, geometry, EF policy, shared L1, and r4 profile headers. It rehashes every source and requires every listed source to equal the build job's actual pinned source row. `build_receipt` is the original `run_job.py` job-finish: `PASS_PROCESS_ONLY`, `process.cleanup.owned_descendants_empty=true`, and `sources`. The runner itself must match its pin and start with the ELF magic. No pre-existing binary is implicitly approved for a new P.

`case_validation` is a small wrapper created only after a real build and the native input qualification have passed:

```json
{
  "status": "PASS_NATIVE_RUNNER_CASE_SOURCE_VALIDATION",
  "runner_sha256": "actual Linux runner SHA256",
  "cases": [{
    "model_key": "qwen25_1p5b",
    "prefill_length": 64,
    "decode_steps": 2,
    "graph_sha256": "actual admitted graph SHA256",
    "source_qualified": true,
    "typed_source_equivalent": true,
    "source_validation_receipt": {"path": "...", "bytes": 0, "sha256": "..."}
  }]
}
```

The source validation receipt is Kuhn's actual `PREFILL_NATIVE_CASE_VALIDATION_V1`, status `PASS_CURRENT_RAW_ABI_SASS_AND_CPP_SOURCE_EQUIVALENCE`. The wrapper builder must first validate its graph/registry/provider/contract pins, controls, unsupported count and typed event checks against the actual binary source. This preparer checks the wrapper's binary and graph identity, exact case tuple and true qualification flags, then rehashes the referenced native receipt and checks its schema/status. It does not repeat the SASS/address derivation or turn a whitelist into a source proof. Additional wrapper fields such as detailed counts or limitations are allowed.

Full input/source/resource checks remain independent: fixed B1 BF16, one warmup, retained sampling without feedback or CUDA graph; exact phase order and kernel/API/allocation counts; unique native launch IDs; actual shared-memory observations covering every initialization/warmup/measured kernel and bound to the graph SHA. Shared 8/16 KiB observations remain distinguishable from calibrated 32/64/100 KiB bins in safe output.

## Outputs and reference separation

Prepare writes a fresh `cases/<case>/preflight/`, `r4-spec.json`, and `prepare-result.json`. The receipt schema is `PREFILL_R4_PREPARATION_V1`, status `PASS_PREFILL_R4_PREFLIGHT_NOT_EXECUTED`, with actual runner pin, runner-evidence pin, graph/resource pins, full input contract, counts and one spec. Existing preparations are never overwritten. Source/binary pins are checked again after preflight.

NCU results go **only** to `references/<case>/ncu-result.json`, so NCU can complete first without occupying the fresh preparation directory. Safe status pairs that reference with the prepared graph's complete input contract; `metadata_actual_controls_compared=true`, closed NCU status and nonadditive independent ROI declaration are required. Initial references must contain exactly `full/Prefill/D1/D2`, each n=1. Missing/mismatched references produce an explicit status with null rows, not zero traffic. ASCII decimal duration strings are preserved as exact integers; missing duration stays null.

Safe output schema: `PREFILL_R4_LLM_SAFE_AGGREGATES_V1`, three `cases` rows with P, fixed D2 and r4. The case structure follows the P128 safe status: preparation expected counts/source pin count/graph/resource/**binary/runner-evidence hashes**, source-stream hash, finite states, whitelisted L1 configurations and counters, completed phase deltas, job CPU/wall/cleanup, NCU rows. It exports no raw addresses, symbols, source paths, prompt IDs, or free-form errors. The safe snapshot is not an accuracy acceptance result.

## Local validation

`python3 -B llm/tests/prefill-tools-r1/test_tools.py`

Nine synthetic tests cover the three finite lengths; forbidden P/D cases; source mutation; missing/extra/duplicate/mismatched resources; runner ELF/source/build/shape/proof failures; fixed single-arm budget; mocked preflight without cache execution; NCU-first directory independence; complete contract/ROI pairing; safe whitelist and phase counter deltas. Synthetic numbers are not measured workload results. This scope does not claim a current P64/P256/P512 binary or native source admission exists.
