# Native P1024/D32 host argument capture: next implementation contract

Implementation update: the host producer, generated plan, independent consumer and CPU tests are now implemented in `capture/native-arguments-r1/`. Remote build/capture is pending. See [implementation status](native-p1024d32-implementation-status.md); the design snapshot below is retained as the implementation contract, not the latest progress report.

Status: design only, 2026-09-19; observer-r2 has now passed independent metadata census audit. See `validation/native-p1024d32-census-audit.json` and its Markdown companion. This document does not admit P1024/D32 to TileGen and does not introduce a new executable or change a frozen capture package.

The completed run contains **13,112 measured launches = Prefill 408 + 32 × Decode 397**, 33 measured kernel decoded-code hashes and 32 symbols. The larger static population (330 functions / 282 hashes) includes related functions. Exact old-workflow comparison finds 29 shared measured hashes and four new ones: CUTLASS GEMM (64 Prefill calls), two Ampere GEMM variants (32 calls each), and MergeStates (32 calls per Decode, 1024 total). There are 10,679 old code/ABI/launch-configuration matches, 1281 old code/ABI matches with changed launch configuration, and 1152 new-code calls. These are metadata reuse candidates, not dynamic-address equivalence.

Observed plan inputs are 58,905 argument slots, **3,975,666 bytes of expected host argument payload**, maximum 18 arguments / 1240 bytes per argument / 1248 bytes per launch, and API counts `cuLaunchKernel=10967`, `cuLaunchKernelEx=2145`. Those payload bytes are computed from the real ABI sizes and have **not yet been captured**. Metadata used 299,144,830 bytes before finish under the 1 GiB cap. The old 8 MiB argument-journal limit is unsuitable: hex alone requires 7,951,332 bytes before 58,905 argument descriptors/hashes and launch metadata. Final serialized bounds still need to be generated and sealed in the new plan.

The next artifact should capture the actual **host parameter byte vectors immediately before each measured CUDA launch**, while preserving the current uninstrumented native SGLang workload. A successful result proves argument transport and launch correspondence. It does not prove dynamic memory addresses, executed instructions, typed object layouts, allocation lifetimes, or simulator correctness.

## 1. Exact sources to reuse

Current repository root is `/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo`. Original C sampler source root, abbreviated `OLD` below, is:

```text
/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/capture/canonical-allargs-r1/sampler
```

| Source / interface | Reusable part | Required change or exclusion |
|---|---|---|
| `capture/native-observer-r2/observer.cu`, `launch_body`, `nvbit_at_cuda_event`, `State::finish` | Current metadata producer, original launch execution, native scope ABI, static code hashing, bounded journals, before/return pairing | Copy to a **new independent package**. Add one argument journal, ledger, and final argument closure. Leave `nvbit_enable_instrumented(ctx,f,false)` in effect. |
| `OLD/argument_capture.h`, `sgargs::{Entry,Actual,Record,Ledger}` | Pure host byte copy, plan comparison, per-epoch and per-module ordinals, selected native launch/return closure, per-argument SHA | Replace all old census constants and old `ALL1138` schema names with generated plan limits; retain rejection semantics. |
| `OLD/argument_runtime.inc`, `capture_native_arguments(...)` | Build `Actual` from the current callback/scope/function; serialize immediately; return `native_argument_record` reference for the launch journal | Replace the fixed 8 MiB journal / 8192-byte row budget with sealed new-plan budgets. Do not import old dynamic sampler machinery. |
| `OLD/sampler.cu:221–259,347–376` | Reference extraction of `kernelParams` / `extra`, passing native launch ID/API into `launch_body`, and `argument_ledger.complete` on return | Extract these small host call sites only. Exclude `sgsample::prepare`, instrumented execution, dynamic packets, device buffers, synchronization, flush ledgers and selected-program capture. |
| `OLD/compile_argument_plan.py`, `compile_header(raw)` | Emit a self-contained `argument_plan.h` whose embedded SHA seals the exact JSON plan | Replace fixed plan SHA, 1138/3-epoch loops and totals with validated generated metadata. No ABI offset inference. |
| `OLD/argument_sideband.py`, `validate_sideband`, `dynamic_modules` | Independent consumer: journal/argument joins, raw SHA/size checks, current-process module ancestry, before/return references | Generalize shapes/counts/caps and exact seven-journal finish status. Remove the fixed 93 shared-Rotary alias condition; derive ancestry and alias counts from this run. |
| `capture/native-observer/run_observer.py`, `validate_native` | Six-journal static/scope/launch closure and current workload/source contract | Reuse its checks in a **new consumer** supporting a seventh journal and new finish schema. The frozen function intentionally rejects a seven-journal producer; do not relax or rewrite the frozen r1 validator. |
| `capture/native-observer-r2/{build_controlled.py,run_observer.py,support.py}` | GPU-free build lease, independent package/build/input SHA, actual model-content/source checks, owned-process bounds and GPU lease | New package and output names, fresh manifests/binary SHA, updated final consumer only. Never overwrite r2 output or modify the frozen workload. |

Original source SHA-256 pins:

```text
argument_capture.h       35dc38640f6a3dbedae1094d8606673ef1909d8625647f47b64822d26a035398
argument_runtime.inc     339fc28d0600d757e74bee583c7e10c3194609e5cae946ed06c950a36918cd1f
compile_argument_plan.py 19009266200b8cf330b1863a28c594329ecb59bc14e2df493cb2b43c789e20ce
argument_sideband.py     1ebf107ce4f260049fe0a725a4b0ba55a4d30e354ac1388920a80cb6672ee2c4
sampler.cu               3416d4306470fb0df1be8879e67d9db2fbfe66f3854907424e01c9de1b73ac49
observer-r2/observer.cu  e136fb1e82380bd9d09b0e69f4452a5e9007319ea3352047f7a22de8b22beba5
```

The frozen workload manifest is `fe559d9bca3a03446e801a37c0a544a09694a22cf9b028909c0af3df478d4884`; observer-r2 package manifest is `36098fdf38aa6437af9c555af8db430a0accfbc4a8b752d41c2956002eb452ff`. The next package must pin both its imported source and its actual new build, not claim the old `.so` identity.

## 2. Produce the plan from successful observer-r2 evidence

Inputs are the completed `runs/observer-r2/controller.json`, `native-census.json`, `observer/process-*/{finish.json,launch-journal.jsonl,functions.jsonl,static-instructions.jsonl,scope-journal.jsonl,lifecycle.jsonl,allocation-journal.jsonl}`, and `native/artifacts/{manifest.json,module_calls.json,...}`. Require the real successful controller, its census SHA, all six journal hashes, exact workload/source pins and 33 epoch closures before generating anything.

The proposed `make_argument_plan.py` takes these files, never a prompt-length scaling factor. It emits a new `SG_NATIVE_ARGUMENT_CAPTURE_PLAN_V1` JSON plus generated header. Preserve native event order; keys are `(epoch_id, epoch_launch_ordinal)` / `epoch-E-launch-O`. Each entry seals:

- Phase, forward ID, role, raw module scope, actual decoder layer and module kernel ordinal. Derive module ordinal by counting launches under `(epoch_id, current run call_id)`; derive decoder ancestry from `module_calls.json`, including shared Rotary instances.
- Exact decoded-SASS SHA and kind, argument size vector and layout SHA, CUDA API name, grid/block, static/dynamic shared sizes, and launch attributes.
- Reference symbol and source native launch ID for audit; do not use function name alone for identity.

Plan-level fields seal workload contract SHA; source controller/census/finish/journal/artifact hashes; phase order; per-epoch launch counts; API counts; total arguments/raw bytes; maximum arguments, argument width and bytes per launch; measured kernel-code/ABI population. Verify contiguous ordinals independently from the journals. Function IDs, native launch IDs, PIDs, handles, pointer values and module call IDs are **current-process bindings**, not expected cross-process constants. A fresh run may have a different initialization launch count. Preserve stream/context topology by rebinding within the fresh process rather than comparing opaque handles across processes.

If a launch attribute contains an opaque event/handle, do not silently normalize it: initially reject that plan as unsupported or add a separately specified identity binding. Current supported simple attributes should remain exact.

All values above come from successful observed metadata. The successful run is now available to generate the plan, but no argument plan/header has been generated by this design audit. Its resulting counts must not be copied from old `1138=408+365+365`, guessed as `408+32×365`, or inferred from the number of transformer layers.

## 3. Minimal host producer API

Keep the small existing interface, with the constructor additionally receiving generated limits:

```cpp
sgargs::Ledger ledger(plan_entries, plan_limits);
sgargs::Record record = ledger.before(actual, kernel_params, extra);
std::string payload = sgargs::serialize(record, sha256);
ledger.complete(current_native_launch_id, cuda_status == CUDA_SUCCESS);
bool closed = ledger.closed();
```

In the entry callback, allocate the native launch ID first, decode the real direct/Ex callback structure, inspect its current function/ABI, copy the host vectors once, write one argument row, then embed its sequence and payload SHA into the saved `Pending::body`. The return callback must reuse that same body/reference and close the corresponding ledger entry. Never recopy parameter buffers after return: the caller may have reused them.

Supported transport initially remains **separate `kernelParams[i]` buffers**: require `kernelParams != nullptr`, `extra == nullptr`, and a non-null buffer for every positive captured size. Direct APIs are `cuLaunchKernel` and `cuLaunchKernel_ptsz`; extended APIs are `cuLaunchKernelEx` and `cuLaunchKernelEx_ptsz`. Decode each against the pinned NVBit/CUDA callback headers. Admit only API variants actually present in the sealed plan; test each implemented decoder with its own callback fixture. The old sampler uses the direct struct for both direct variants, which must be explicitly checked against the pinned headers rather than assumed for a new build.

Copy exactly `argument_sizes[i]` bytes from each host parameter buffer before the original launch. Do not dereference the device pointer value stored inside such a buffer. A captured row records the fresh process/start ticks, native ID, plan key, phase/scope, code/layout identities, transport, capture-before flag, per-argument index/size/raw hex/SHA and `parameter_buffer_offset: null`. The vector is not a packed buffer: byte offsets cannot be reconstructed by prefix sums of argument sizes.

Mark `dynamic_instrumentation=false`, `memory_addresses_captured=false`, `actual_sm_placement_captured=false`, `device_memory_dereferenced=false`, `kernel_argument_values_captured=true`, `typed_objects_or_relocation_qualified=false`. Static SASS remains decoded rows, not a cubin hash. No added CUDA operations, dynamic instrumentation, CUPTI profiler, device synchronization or GPU buffers are required.

## 4. Values that must be measured, never supplied by the plan

| Data | Required source / remaining boundary |
|---|---|
| Actual kernel scalar and struct bytes, pointer bit patterns, strides, dimensions, leading dimensions, KV/page-table pointer fields, flags | Copy real host parameter buffers in the fresh entry callback. Names, shapes and ABI sizes cannot determine their values. |
| Current launch/scope/function/context/stream identity and return status | Current callback and current-process scope/module records; join to the plan by observed ordinal/code/ABI, not old process IDs. |
| Typed field boundaries, signedness, padding, pointer relocation and tensor semantic role | Later versioned ABI decoder backed by the exact source/type layout and same-process tensor/allocation evidence. Raw bytes alone do not establish these. |
| Device data referenced by pointers, page-table contents, per-lane active/predicate masks, dynamic PCs, effective addresses, actual CTA/SM order | Not provided by this host-only capture. Require separately collected and qualified native witnesses or typed source lowering with its own validation. |
| Per-kernel NCU timing or traffic | Separate profiling evidence and a justified phase/launch join; parameter capture runtime is not a kernel timing measurement. |

## 5. Bounds, validation, and failure behavior

Generate exact expected measured totals from the plan: `N = launches`, `A = sum(argument_count)`, `B = sum(sum(argument_sizes))`, per-epoch counts, and API populations. Use overflow-checked arithmetic. Enforce plan bounds before reading/copying host buffers; enforce entry/order limits before incrementing state. Finish requires exactly N entries and successful paired returns, A arguments, B raw bytes, every planned epoch, no duplicate/missing key and no pending launch.

Keep independent hard safety ceilings as well as these exact totals. Determine journal/row limits from the new observed maximum ABI and a conservative bound for the actual serialized schema (hex adds `2×B`, plus hashes and bounded metadata), and seal the chosen limits. Unit-test boundary acceptance and one-byte-over rejection. Do not retain the old 8 MiB argument/8192-byte row/32 MiB launch/8 MiB module/10,000 module-call caps without checking the new census. Old maxima 18 arguments, 1240 bytes per argument and 1248 bytes per launch were P32/D2 observations, not universal ABI limits.

The r2 total metadata cap is 1 GiB and its reserve is 128 KiB; a seventh journal consumes that same total if written with `State::emit`. Admit a run only when r2's observed bytes plus the sealed argument budget and finish reserve fit the new cap. Otherwise create a separately reviewed larger-cap source revision or a separately bounded ledger file; never bypass accounting. Preserve native artifact, log, disk/RSS, overall wall-time, launch/function/instruction and process-cleanup limits. Plan compilation itself must reject oversize or unsupported metadata before a GPU run.

Consumer checks are independent of producer counters: verify all seven file SHA/lengths, newline/row bounds, duplicate-key rejection, exact source/build/contract, 33 scopes, all before/return pairs and argument references, actual raw-byte lengths/hex/SHA, current-process ancestry and argument totals. Independently count all launches enclosed by each epoch, including detecting an incorrectly unmarked launch inside an epoch. Require the new final status, for example `PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY`; a metadata-only finish is insufficient.

On unknown ABI/packed transport, plan drift, inaccessible/null parameter, missing/duplicate launch, bad return, quota, signal, deadline or incomplete closure, the run fails and no qualified result is published. Retain partial files with a failed receipt for diagnosis. Only terminate this run's owned children through the existing controller; release leases after owned-process drain. A host read fault must yield child failure, never synthetic bytes or a successful footer.

## 6. Minimum tests and independent rerun

Before any GPU execution, port the existing host mock with a small synthetic multi-epoch plan. Test real independent scalar/struct buffers; mutation of buffers after `before` must not change captured bytes. Cover direct/Ex callback layouts, before/return reference identity, packed/null transport, code/layout/grid/scope/attribute drift, negative/overflow widths, missing/extra/reordered/duplicate entries, failed returns, cross-process binding, shared-module ancestry, newline/hash/hex corruption, row and aggregate quotas. Compare the generated header's normalized plan to its JSON, not just a duplicated code loop.

Then use one fresh host-only capture against the already sealed r2 plan. Treat it as an independent launch/ABI holdout: all measured ordering/code/layout/geometry/scope invariants must match exactly or the plan fails without auto-learning. Across processes, pointer bytes and struct padding need not match; raw argument SHA is an integrity hash for that run. Do not claim byte-for-byte cross-run determinism. A further independent capture can test stable typed scalar fields only after their decoder has been justified. No number of matching metadata/argument runs substitutes for a dynamic memory witness.

## 7. Smallest future change boundary

Create a new `capture/native-arguments-r1/` package with a copied r2 observer plus the argument hook/ledger; `make_argument_plan.py`; generated `argument-plan.json`/header; generalized host-only argument consumer; CPU tests; original GPU-free builder with explicit source closure; thin controller and manifests. Keep original workload, observer-r1/r2, old C sampler and production simulator untouched. Reuse controller/resource/model checks. Preserve diff/source hashes and new binary build receipt. Build under CPU45's existing lock with GPU visibility empty; a subsequent GPU capture needs its own existing GPU lease and explicit dispatch after other sampling finishes.

This completes a transport input step only. P1024/D32 typed bindings, memory/program lowering, selected-CTA dynamic witnesses, stage compute estimates and NCU comparison remain separate qualification steps.
