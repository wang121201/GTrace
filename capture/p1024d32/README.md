# Isolated native SGLang P1024 / D32 capture

This package prepares **new hardware observations**, not a P1024 / D32 TileGen model. No GPU execution is performed by local preparation. The source baseline is the existing native SGLang P32 / D2 driver and app-range NCU protocol; exact originals are pinned in `origin.json`.

The default workload is Meta-Llama-3-8B-Instruct, real original BF16 checkpoint, batch 1, TP/PP 1, 32 layers, native FlashInfer eager operations. Prompt IDs are 1000 through 2023. Decode inputs alternate 944, 291 for 32 steps; native sampling still runs, but its predictions never feed the next input. Every phase is independently checked against the frozen input contract. Capacity is 1280 tokens, with a minimum P+D+1 gate. CUDA Graph, torch.compile, radix-cache and overlap scheduler remain disabled; static memory fraction remains 0.90. A complete unmeasured warmup precedes allocator reset and the measured workflow. Native SGLang kernels are unmodified.

`--prefill-length`, `--decode-steps`, and `--max-total-tokens` are shared by both hosts and the controller. The bounded supported input range is P<=1024, D<=32. The default target is P1024/D32. Lower shapes are only explicit diagnostic workloads and get a different contract SHA.

## Local preparation

```sh
python3 -B prepare_package.py
python3 -B run_capture.py --mode discovery --run-name discovery-r1
```

The second command prints a plan only. CPU tests cover input/control failures, all six ROI boundaries, one-action NCU metric units, and ownership/PID reuse. They do not establish compatibility with the remote installed GPU stack. `upload-manifest.json` pins all package files, including the CPU receipt. Freeze it after review; do not run preparation in the staged package or silently regenerate its hashes.

## Remote deployment and execution by the parent task

Stage the exact manifest files and manifest in the previously nonexistent directory:

`/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi/capture`

The controller is deliberately pinned to this private task root. It stores new results in `../runs/` and caches in `../cache/`. It does not edit the original C runtime or its sources. Reuse the **existing** CPU46 lock (or CPU45 build lock with `--cpu 45`) under the original C task and the shared lock for GPU `GPU-18ace299-5348-e6e4-d48c-1ee5a602859b`. A lock conflict or live GPU owner rejects the run. Do not make a replacement lock file or stop another task.

From the staged package, discovery runs as user `xmu`:

```sh
/usr/bin/python3 -B run_capture.py --mode discovery --run-name discovery-r1 --execute
```

It first runs an unprofiled full native validation with CUDA-event times, then a separate torch.profiler metadata execution. Both execute all 33 stages. The latter records native phase/module markers, kernel launches, tensor roots, shape/stride and KV state; it does not collect dynamic SASS or memory addresses. Instrumented metadata wall times are diagnostics, not hardware timing references.

NCU requires caller-provided authorized profiling privilege. The script never invokes sudo or reads credentials. The first capture collects one pilot group across Full, Prefill, Decode1, Decode8, Decode16 and Decode32:

```sh
/usr/bin/python3 -B run_capture.py --mode ncu --run-name ncu-pilot-r1 --groups 1 --discovery-receipt ../runs/discovery-r1/controller.json --execute
```

Only after the pilot closes and is reviewed, run three new independent groups:

```sh
/usr/bin/python3 -B run_capture.py --mode ncu --run-name ncu-three-groups-r1 --groups 3 --discovery-receipt ../runs/discovery-r1/controller.json --pilot-receipt ../runs/ncu-pilot-r1/controller.json --execute
```

Every scope runs the complete frozen workload with one selected profiler range, warmup excluded. A Decode8 capture includes unprofiled Prefill and Decode1–7 before its selected range, and still finishes Decode9–32. It never begins from an empty KV context or resets the cache at that phase. The NCU settings preserve the original all-scope protocol: `--replay-mode app-range --cache-control none --clock-control none`, exactly `dram__bytes_read.sum,dram__bytes_write.sum,gpu__time_duration.sum`. The import must contain one range action with byte/byte/ns units and a PID matching a completed native host. All replay hosts must close; replay passes are not independent samples. The pilot is separate from the final three groups, not reused as their first sample. There is no kernel-name filter.

R/W and duration belong to the same app-range observation. CUDA-event validation times are separate and must not be substituted as the NCU bandwidth denominator. Full and the individual selected ranges are independently replayed observations, so their statistics are not an additive timing decomposition. These six scopes do not yield per-wave or all-32-decode-step hardware counters.

## Resource and identity gates

Defaults: one selected CPU, 1800 seconds per owned step, 48 GiB combined owned RSS, 64 MiB combined logs, 1 GiB per step, 8 GiB durable run output, at least 32 GiB free disk before admission. Quotas are polled and may overshoot between samples; failure rejects rather than truncates data. SIGTERM/SIGKILL target only witnessed child births. Cleanup retains the leases until owned children exit; final GPU quiescence is checked. No unrelated process is signalled. All runs use nonexistent output directories and clean, explicitly constructed child environments, with no inherited LD_PRELOAD.

The package manifest, six model/index/config content hashes, six Python package versions and 16 native SGLang Python source hashes are checked. Model files are fully hashed before each controller run and stat-checked afterward; Python source identity is checked before and after every host execution. This is not a claim that every native shared library or JIT binary is already pinned. New static SASS/kernel ABI identities require the next collection step.

## Remaining native model work

Do not edit the sealed P32/D2 1,138-launch plans to pretend they represent P1024/D32. New launch chronology, actual decoded SASS hashes, argument ABI/values, object roots, native resources, selected-CTA memory/program samples and holdout validation remain required. Prefill GEMM kernels and FlashInfer attention variants may change; decode attention has longer sequence-dependent loops and masks. Even unchanged code hashes do not prove unchanged address rules.

Original metadata-only observer source: C `work/sglang-integration/nvbit_observer/{observer.cu,build.py}`. The existing full argument sampler `canonical-allargs-r1` hardcodes the 1,138-launch/three-epoch plan in its argument compiler and consumer. Later `canonical-gemm-p28-allargs-r3` and `canonical-attention-prefill-allargs-r1` preserve selected-CTA dynamic PC/static SASS/shared-operand records, but explicitly whitelist old source codes. They must be adapted from fresh measured metadata; this package neither stages nor runs them.

No full memory trace, instruction dependency model, TileGen P1024/D32 admission, stage-compute calibration, GPU-stall timing or hardware accuracy qualification is claimed by a successful capture. NCU has no per-CTA wave timing here. Existing runtime frames, source ordinals, service maps and shape-specific bindings remain frozen until a separate reviewed import is complete.
