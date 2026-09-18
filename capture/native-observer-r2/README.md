# Observer r2: bounded 1 GiB metadata capacity

The P1024/D32 r1 run failed with `observer total metadata quota` (child exit 74). The original r3 binary has a compiled 256 MiB maximum, so increasing only its environment variable cannot work. This package creates a new isolated build; r1 and the frozen workload are unchanged.

`observer.cu` changes exactly two literals: `DEFAULT_CAP` from 256 MiB to 1 GiB and the diagnostic range text. The 128 KiB finish reserve, 1 MiB per row, 100,000 launch cap, 4,096 function cap, static/scope/argument metadata, closure and CUDA callback logic are unchanged. `observer-capacity.diff` is the exact patch, and `origin.json` records the old and new source SHA. `build.py` is byte-identical to the original audited GPU-free builder; `manifest.json` pins its new source and the unchanged real NVBit headers. No dynamic instruction instrumentation is added.

Stage the exact upload manifest files at:

`/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi/observer-r2`

Dependencies remain the sibling `../capture/` (manifest SHA `fe559d9bca3a03446e801a37c0a544a09694a22cf9b028909c0af3df478d4884`) and `../observer/` (SHA `48d8d0366b965b5df13325861135ad931b7d1fd53a35c55a36d1b87a90d12e00`). The r1 validation code and capture workload are imported only after all their files and exact manifests pass validation. No frozen file is modified.

## Build only, as xmu

From the staged directory:

```sh
/usr/bin/python3 -B build_controlled.py
/usr/bin/python3 -B build_controlled.py --execute
```

The first command only prints its plan. Actual compilation holds the existing original C `cpu45-build.lock`, pins affinity to CPU45, and sets `CUDA_VISIBLE_DEVICES=''`. It never queries a GPU or takes a GPU lock. Therefore the parent may schedule this CPU-only build separately from NCU on CPU46. The real defaults are CUDA `/usr/local/cuda-12.8`, NVBit `/home/xmu/nvidiagds/simulators/hyfiss/tracing-tool/nvbit`, target `sm_89`; all real headers/library/compiler are included in the build receipt. Compiler children retain the original bounded build/cleanup behavior. Do not run the bare build script without the CPU lease.

The new binary and receipt are `observer-r2/build/{observer.so,build.json}`. Build controller logs are in `../runs/observer-r2-build/`. Both output directories must be nonexistent. `PASS_CPU45_BUILD_ONLY` is not GPU qualification.

## Later observer run, after NCU releases the GPU

```sh
/usr/bin/python3 -B run_observer.py --discovery-receipt ../runs/discovery-r1/controller.json
/usr/bin/python3 -B run_observer.py --discovery-receipt ../runs/discovery-r1/controller.json --execute
```

No observer GPU execution may overlap NCU. Actual execution holds the existing CPU46/GPU leases, verifies successful discovery, full model contents, both frozen dependencies, the new wrapper, every compiler input and the newly built binary/receipt. It uses the same native P1024/D32 driver without torch profiler, the new library as sole LD_PRELOAD, and `SG_NVBIT_MAX_BYTES=1073741824`. The default new run is `../runs/observer-r2/`.

Native host artifacts retain the reused controller's 1 GiB cap. The observer's separate directory has a hard 1 GiB metadata writer cap. They are separate budgets, not one shared 1 GiB allowance. The same 48 GiB owned RSS, 64 MiB combined logs, 1800-second default step deadline, cleanup and final GPU-quiescence requirements apply. A new capacity failure remains a failed/incomplete run; this change does not guarantee P1024/D32 will fit.

The exact r1 validator is reused for all 33 epoch pairs, before/return launch identities, native module ancestry, static instruction counts/SHA and ABI size layouts. The new finish must additionally confirm the actual 1 GiB cap. Raw argument values, typed pointer binding, dynamic program/memory trace and native model admission remain false.

Run `python3 -B prepare_package.py` only locally before freezing. Tests prove reversing the two source literals reproduces the original SHA, the builder is unchanged, and the frozen dependency/33-phase closure tests pass. They do not run NVCC, SSH or a GPU. Do not regenerate the manifest after upload.
