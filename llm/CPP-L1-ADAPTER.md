# Full-source LLM L1 adapter

This runner preserves the frozen complete-source instruction stream and the historical L2 body: global VA / matrix 0 tags, 40 MiB PAPER_ADA_L2_V1_20x1024x16, full 128 B fill/RFO, 32 B dirty-sector writeback, h288 and last-store 64,000,000 forwarded-L2-access age. There is no final flush, allocation flush, or API-induced L1 flush. This is functional caller-order replay, not GPU timing or HBFSIM cosimulation.

## Entry

Build `llm/executor-r1/main.cpp` with C++20, `-O3 -DTINY_SHA_PORTABLE`, and include directories in this order:

1. `llm/executor-r1`
2. `source`
3. `source/work/tilegen-full-r1/core-native-copy-r2/include`
4. `source/work/gddr6-support/delivery-stage/sources/third_party/gtsim-prepared/third_party`

No large captured trace is a build dependency. Adjacent C++ source-loop heads remain in `llm/fast-prefill-r2`, `llm/fast-down-r1`, and `llm/fast-llama-p32-prefill-r1`. Python producer and deployment are maintained separately by the parent task.

The CLI is unchanged: `source-cache-runner --input - --summary FRESH.json --snapshots FRESH.jsonl`.

Process-fixed environment:

- `TILEGEN_ADA_L1_PROFILE=legacy32|r2|r4` (default `legacy32`).
- Preferred observed mode: `TILEGEN_ADA_REQUIRE_OBSERVED=1`, no `TILEGEN_ADA_SHARED_BYTES`. Initial L1 construction uses an explicitly labeled bootstrap 32 KiB shared value, without claiming observed resources; initialization APIs bypass L1. Every `begin_kernel` must then contain `observed_shared_bytes`.
- Sensitivity mode: omit REQUIRE_OBSERVED and provide `TILEGEN_ADA_SHARED_BYTES` explicitly. Its value is assumed, not observed. A supplied per-kernel `observed_shared_bytes` takes precedence.
- Fixed paired-test L2 values: `TILEGEN_EF_HIT_RATE=288`, `TILEGEN_L2_DIRTY_AGE_ACCESSES=64000000`.

Shared means the selected shared-memory carveout, not dynamic shared bytes per CTA. Supported bins: 8192, 16384, 32768, 65536, 102400 bytes. Original 32/64/100 KiB bins use the repository's frozen r4 serial helper. The 8/16 KiB bins are explicitly marked **uncalibrated_extrapolation_8_16KiB** and apply the same 1062/1000 scale to nominal `(128 KiB - shared)` with whole-set flooring. No new calibration is claimed.

| Shared KiB | r2 L1 KiB / ways (4 sets) | r4 L1 KiB / ways (16 sets) |
|---|---|---|
| 8 | 120 / 240 | 126 / 63 |
| 16 | 112 / 224 | 118 / 59 |
| 32 | 96 / 192 | 100 / 50 |
| 64 | 64 / 128 | 66 / 33 |
| 100 | 28 / 56 | 28 / 14 |

Both new modes use 48 modeled SMs, CTA-linear-id modulo 48, serial source order, kernel L1 invalidation, original store bypass, no write allocation, and no dirty protection. r2 is LRU/global-linear hash; r4 is CLOCK/allocation-relative hash2. These are LLM extrapolations of serial read-filter policies, not newly verified physical Ada behavior. The existing native source's cache-op bypass remains respected. Since L1 filtering changes, the same 64M age parameter may trigger at different points; its units remain forwarded L2 accesses.

## Allocation protocol and boundaries

`{"type":"allocation_metadata","node":FULL_NATIVE_GRAPH_NODE}` carries the original `allocation_API_observation`. Only current qualified synchronous `cuMemAlloc_v2` / `cuMemFree_v2` nodes are supported, with complete return/reference metadata. Driver base, extent and generation are checked; active overlap, unmatched free, wrong generation and overflow are rejected. Actual `cuMemFree_v2(0)` observations are accepted as no-ops. Nothing clears L2.

For r2/r4, every request that enters modeled L1 must fit a known active CUDA driver allocation; a read without such a binding fails. L1-only allocation ID and relative byte offset are derived there. L2 retains its original matrix 0/global VA identity. API/source-op/store bypasses do not need or invent hash offsets. R4's frozen uint32 relative-address domain is enforced by the shared header. The observed driver allocation is not a complete tensor/suballocator lifetime; no such claim is made.

Legacy32 uses a namespaced copy of the old L1 header, preserving its sector-validity/fill-ticket behavior and counters exactly. The two new profiles use the repository's shared `per_sm_l1.h`. They do not pretend that old/new readiness or decision hashes have the same definition. The profile-specific L1 counters exported by this adapter are global only; per-SM counters are not exported.

Every kernel emits `kernel_L1_configuration` with exact capacity/sets/ways/profile/hash/replacement/shared origin/qualification. Snapshots include cumulative allocation resolution/bypass/gap counters. Source effect hashes and original DRAM record fields are retained. Coverage min/max fields are real addresses and should be excluded from public reports.

## Verification

`tests/component-results.json`: ASan/UBSan bounded component tests. The legacy32 test sends 5,000 deterministic mixed instructions, independently compares every full cache snapshot to the frozen old core and compares every resulting DRAM record. New-profile tests cover requested-sector readiness, 32 B dirty creation/writeback, age expiry, L1 reconfiguration without L2 flush, allocation/free, unknown addresses and the new carveout bins.

`tests/cli-results.json`: observed-only bootstrap and actual stream interface; 3 profiles plus missing/overflowed witness, unknown allocation, mixed assumed/observed configuration, and missing assumptions are tested. Across valid profiles the source effect hash is identical. A dirty tail remains at run end and DRAM write stays zero in this small fixture, proving no mandatory end flush.

`tests/actual-allocation-results.json`: only the 105 Qwen and 150 Llama allocation metadata observations from current full graphs are accepted in original order. No model memory instructions are expanded; this is not model traffic acceptance.

No remote jobs, GPU work, whole-model replay, or new accuracy claim is part of this component delivery. The complete runs still require the caller's graph/source/resource admission and post-run closure.
