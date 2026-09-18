# P1024/D32 native metadata observer reuse plan

Read-only audit of the original C sources and historical receipts. No remote inspection or GPU execution was performed for this plan. The frozen `capture/p1024d32/` package is unchanged.

## Reuse the successful r3 observer

Use the existing metadata-only library, after the parent verifies its current bytes:

```
/home/xmu/nvidiagds/codex-runs/hbserve-memgen-gtsim-alignment-20260914-01a09f50-r1/sglang-integration-r6/nvbit-build-r3/observer.so
SHA256 1248ba7c81c4bd35acb28a972fa1edd16d1993d3b2eb2c6d5d70fcfdbb3666c7
```

Evidence is C `work/sglang-integration/runs/r6/controller.json`: PASS, unprivileged CPU46, actual native FlashInfer/BF16/P32/D2, mem_fraction_static=0.9, no torch profiler. It ran 2026-09-15 07:07:35.879948 through 07:09:20.345939 UTC. The observer receipt at `runs/r6/observer/process-1488395-845566790/finish.json` reports `PASS_METADATA_OBSERVER_CLOSED_NOT_TRACE`, 2,426 total launch-before records including setup/warmup, 222 inspected functions, three marked epochs, and 90,334,029 metadata bytes against a 268,435,456-byte cap. The measured phase count was separately 1,138; it is not an observer constant.

The matching source is C `work/sglang-integration/nvbit_observer_r3/`, not the oldest un-suffixed `nvbit_observer/`:

- `observer.cu`: SHA256 `7ebb59827281aea8651921b476af5f54bea17772e5aeac72da13a1d27160f2f7`, 35,678 bytes.
- `build.py`: SHA256 `f4c290b9ab34741a30c69131cb42810861217583d6f7c4e3dcdfb98076048b54`, 4,590 bytes.
- `manifest.json` pins the nine real NVBit headers. Its historical pending-build status is not the executed binary receipt; use the successful r6 controller and actual library SHA above to distinguish them.

r2 restricted expensive static inspection to marked inference epochs; r3 raised the bounded metadata ceiling from 64 to 256 MiB. The un-suffixed r1 is therefore the wrong baseline. The successful r6 lifecycle records NVBit 1.7.6 and NVBit bundled CUDA header version 13000. The build script uses CUDA 12.8 nvcc, `sm_89`, real `libnvbit.a` and those pinned headers. Do not confuse nvcc's version with the bundled header version. Do not deploy CPU-test mock headers.

If the original library/build receipt cannot be verified, rebuild only these three r3 files into a fresh private directory using `build.py --output <fresh-build>`, under a free CPU lease. The build is GPU-free and emits input/binary SHA receipts. A different binary identity must be recorded as a new build, not silently substituted for the historical one.

## Compatibility and a minimal controlled run

`observer_r3/observer.cu:300–318` accepts any positive paired epoch and forward IDs >=-1; layers are -1 through 1023 and phase names are bounded strings. There is no three-phase, P32, or 1,138-launch rule. Global caps remain 100,000 launches, 4,096 inspected functions, 100,000 instructions/function, and 256 MiB metadata including the finish reserve. Setup and warmup launches count toward the launch cap; static inspection occurs only inside marked epochs. A larger new kernel/static census may exceed the cap and must fail, never truncate.

The new `capture/p1024d32/sglang_driver.py` is compatible with the same five-function scope ABI. It emits epochs 1–33 and forward IDs 0–32 from its actual phase list, with module/layer ownership inherited from the original driver. Its parent now initializes the observer with that phase list, so it does not retain the original three-name lookup.

Run **a separate unprofiled observer execution**, after discovery has drained and the shared CPU/GPU locks can be acquired. Do not combine NVBit with `--profile`/torch.profiler or NCU. C's prior audit explicitly did not support CUPTI+NVBit in the same process. Discovery's separate profiler run remains an independent symbol/geometry/order cross-check, not the same-process SASS census.

The child environment should retain the new task's explicit CUDA/offline/thread/cache settings and add:

```
LD_PRELOAD=<the one SHA-verified metadata observer.so>
SG_NVBIT_SCOPE_ABI=1
SG_NVBIT_OUTPUT_ROOT=<new empty task-owned observer directory>
SG_NVBIT_MAX_BYTES=268435456
ACK_CTX_INIT_LIMITATION=1
```

The child argv is:

```
/home/xmu/sgl/bin/python -B <new-task>/capture/sglang_driver.py
  --output <new-observer-run>/artifacts
  --prefill-length 1024 --decode-steps 32 --max-total-tokens 1280
```

No `--profile`. The default attention/memory settings are already frozen to FlashInfer/.90. Run as xmu; this metadata observer uses no NCU hardware counters and the original route was unprivileged. New execution remains subject to successful load and native closure.

The frozen `run_capture.py` deliberately constructs a clean child environment and removes inherited LD_PRELOAD. Consequently, prefixing its invocation with these environment variables will **not** enable the observer. The smallest future implementation is a separate wrapper outside the frozen package that imports its bounded `run_step`, acquires the exact existing shared locks, verifies package/model/observer identities, supplies this child environment and argv, then verifies the observer finish. Do not edit the original C controller, use replacement lock files, or weaken ownership cleanup. All new metadata belongs below `/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi/`.

## Required closure and what it establishes

The wrapper must require both the driver manifest and exactly one successful observer process with matching PID/startticks. Check all six journal files against `finish.json` sizes/SHA, matched launch before/return pairs with successful CUresult, no unsupported dispatch/graph/unknown attributes/errors, no open contexts/epochs, and 33 distinct ordered begin/end epochs enclosing all measured launches. Measured scope/phase/layer attribution must match the new driver's module-call ancestry. Derive a new per-epoch launch ordinal from these records; never paste in 408/365/365 or the old launch keys.

The generic C `audit_observer.py` is **not** directly reusable unchanged: it requires exactly three epochs at line 92 and enumerates Prefill/Decode1/Decode2 at line 139. `join_launch_metadata.py` also hardcodes its phase-summary loop at line 63. Their parsing/closure mechanics can be extracted, but the expected phase list must come from the new input contract. The low-level observer itself has no such shape restriction.

The resulting records provide:

- Host launch chronology with before/return, native CUDA API, process/context/stream/module-epoch/function identity, epoch and Python module/layer scope.
- Actual grid/block, static/dynamic shared memory, register/local-byte attributes, launch attributes and API status. These are native resource descriptors, not observed occupancy or actual SM placement.
- Every NVBit-decoded static instruction for inspected functions, including offset/PC, opcode, SASS text, operand kinds and predicate metadata. The ordered canonical instruction SHA is `sha256_nvbit_decoded_instruction_rows_v1`; it is **not a cubin/module SHA**.
- Ordered kernel argument **sizes** from `nvbit_get_kernel_argument_sizes` plus a hash of that size vector. This is the ABI size layout only.

It does **not** capture raw argument values, pointer roles/types, struct-field offsets, dynamic PC execution, masks, lane addresses, shared/global access sequences, actual SM scheduling, register values, or a post-cache DRAM trace. Allocation callbacks preserve a bounded host lifecycle journal but do not prove complete allocator or async-device lifetime coverage. These missing facts require fresh argument-sideband/program sampling and holdout validation before P1024/D32 native TileGen binding. The fixed 1,138-launch argument sampler and old code-whitelisted program descriptors must remain separate from this generic metadata step.
