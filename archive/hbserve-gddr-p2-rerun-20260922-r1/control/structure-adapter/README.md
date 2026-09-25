# Isolated Ada structure adapter — actual bounded CPU closure

`run-r1/receipt.json` is **PASS_OWNED_ADA_STRUCTURE_ASYNC_COMPONENT**: 18.8151 s including six core compiles, two fixture compiles and two actual executions. 274 input pins remained unchanged. The candidate performed 373,904 checks. All ten steps exited zero, runtime stderr was empty, and the owned process group was absent and reaped at closure. Compiler warnings are preserved; this is not a warning-free build claim.

This package adds the pinned AccelSim **software** L2 indexing mode to the frozen asynchronous GTSim core. It preserves the existing PAPER/fully-associative modes and the entire `L2GroupedLru` implementation, including h288's existing LRU-end insertion/touch. No cache policy, dirty-age rule, request source, backend routing, or frozen file was changed.

| Candidate | L1 hit | Folded L2 hit | Core clock | Source qualification |
|---|---:|---:|---:|---|
| `tuner-v1` (default for root integration) | 34 cycles | 34 + 238 = 272 cycles | 2,175 MHz | Original tuner-v1 configuration, software reference |
| `J-candidate` (optional) | 39 cycles | 39 + 237 = 276 cycles | 2,175 MHz | J refinement candidate; **not promoted**, clock-validity failures |
| `legacy` | Original 7 cycles | Original 272 cycles | Original profile | Exact old factory; bounded q1/q8 regression |

The 271-cycle mixture is not used. `provenance.json` contains the exact original config paths/hashes and every raw field. The new grouping is 40 MiB, 20 subpartitions × 1,024 sets × 16 ways, with 128B lines. The fixture checked all 327,680 aligned lines in a 40 MiB range against separate integer arithmetic, with exactly 16 addresses in every group. 311,296 tested line addresses differ from the old PAPER grouping. Raw cache-key VA is indexed before the existing service-address mapper.

This is not a sector-valid L2 implementation: the existing 128B read/write-allocate fill and 32B dirty/writeback semantics remain. L1 stays the current 32 KiB/64-way policy, with current sector validity selected by the runtime. These fields are not claimed to reproduce AccelSim's entire L1 organization.

## What actually ran

* A separate original binary, with original objects and no overlay, and the candidate binary ran the same cold q1/q8 sequence. Every saved legacy runtime/dirty/readiness/backend counter, decision/fill/callback, end cycle and policy field matched exactly.
* For each candidate, q1/q8 and fixed external delays 17/31 were checked. q1 misses issued at cycle 1 completed at 18/32. q8 misses issued at 8 and became visible at 32/40, consistent with epoch polling. Every candidate output was identical with the unused internal miss fields set to 604 or 0. Thus there was no observed internal floor or post-return addition on this external path. No fake backend completion was handed to the actual HBF case.
* Actual q1 L1 hit completion used 34/39 cycles from acceptance; L2 hit completion used 272/276 from the L2 decision. L1 misses did not first incur 34/39. These are modeled hit settings; the unmodeled frontend/network delay on a miss remains an explicit gap.
* Actual unmodified HBFSIM served 17 congruent cache-line fills and the eviction of one 32B dirty sector: **2,176 B read, 32 B write**, 18 admitted/completed requests, zero reserved credits at end, dirty **I0+C1=E1+F0**. No final dirty flush was performed. Native finish was 644.8889 ns; the GTSim quiescent cycle was 1,403. This synthetic serial small component is not a full-workload timing result.
* The rational core clock is exactly 40,000/87 ps per cycle. The existing HBF configuration and command timing were unchanged and explicitly pinned, including `selected.cfg`.

## Runtime integration

Include this directory's `config.h` and call:

```cpp
auto cfg = GTSim::make_ada_cosim_alignment_config("tuner-v1");
// Existing runtime still selects sector32 and enables the same h288 policy.
// Its actual external HBF backend must use sg_hbf::Clock(40000,87).
```

Factory signature: `GTSim::SimulatorConfig GTSim::make_ada_cosim_alignment_config(const std::string&)`. Only `legacy`, `tuner-v1`, and `J-candidate` are accepted. The nonlegacy configurations are qualified **only with an external completion backend**. The factory does not instantiate the backend or silently change the caller's clock. The `legacy` route requires its original rational clock, not 40,000/87.

Read `run-r1/receipt.json` for `compile_flags`, `overlay_flags`, and all 21 `objects[i].object.path`. Compile the new driver with both recorded `-ivfsoverlay` pairs and the unchanged flags. Link the six new 01..06 objects and the fifteen proven unrelated 07..21 original HBF objects. The second overlay is the already qualified passive writer-capacity extension needed by full history; its default behavior remains unchanged. Every candidate TU and fixture dependency file includes the virtual geometry and writer headers, and the actual new mapping header. Their physical replacement files are separately pinned. Do not mix an old 01..06 object with an overlaid driver.

`profile.json` is the existing external HBF profile with only its clock/source identity updated. Its internal 604 field is retained **unused**, not an executed latency. Keep `native_hbfsim_config_file` and the rest of the backend fields unchanged for this candidate.

## Limits before a full run

The caller's `sm_id` and `l2_subpartition_id` remain intact (the fixture explicitly checks 3 and 2). HBF routes `mapper.map(cache_key)` through its unchanged `HbmDevice.decode`; it does not route by the L2 metadata field. Therefore 20 software L2 subpartitions are **not** evidence of 20 hardware queues, nor a proved mapping onto HBF's ten service channels. No `%10` shortcut was introduced. NVIDIA's physical hash, true L2 bank contention and miss frontend/network latency remain unqualified.

No GPU, source workload, full history, or full inference was executed here. This package supplies compatible core objects and bounded structural evidence for the root's next mixed-window integration, not full-run admission or an accuracy/speedup claim. Current h288 is merely retained; no new tuning, hardware calibration, or dirty-age heuristic is adopted.
