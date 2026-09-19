# Native P1024/D32 host arguments

This isolated package extends the successful `native-observer-r2` capture with one host-only argument journal. The real SGLang workload, checkpoint, B1/BF16/32-layer P1024/D32 input contract, 1 GiB observer cap, GPU/CPU leases and process cleanup stay the same. Original capture packages are immutable dependencies.

The sealed plan comes from the successful 33-epoch static census: 13,112 measured launches, 58,905 argument slots and 3,975,666 payload bytes. It pins code, ABI sizes, launch geometry/resources, module ancestry and order. A fresh run binds its own process, native launch, function and module-call identities to those structural checks. It captures each real `kernelParams[i]` host buffer before the original launch, records exact bytes and SHA, and uses the same reference on return. Packed `extra` transport and unsupported dispatches fail rather than guessing parameter offsets.

The observer still executes original, uninstrumented kernels. Pointer bit patterns are captured as argument bytes; device buffers are not dereferenced and per-lane effective addresses are not observed. A successful `PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY` result establishes transport and same-run correspondence, not typed tensor relocation, dynamic memory equivalence, timing accuracy or complete TileGen support. The simulator's existing 32 B dirty-sector writeback rule is unaffected.

## Local preparation

The plan generator independently validates the successful static run before generating JSON and a compact C++ header:

```sh
python3 -B capture/native-arguments-r1/make_argument_plan.py \
  --source /Users/wgs/Documents/Codex/2026-09-17/zhi/work/p1024d32-capture/observer-r2
python3 -B capture/native-arguments-r1/test_arguments.py
python3 -B capture/native-arguments-r1/test_producer.py \
  --output build/native-arguments-producer-r1 --sanitize
```

Only run preparation on an unfrozen local package. Seal it using the completed test receipts; the sealer verifies their input SHA rather than rerunning successful tests:

```sh
python3 -B capture/native-arguments-r1/prepare_package.py \
  --producer-receipt /absolute/path/to/producer/receipt.json \
  --consumer-receipt /absolute/path/to/consumer/receipt.json
```

The manifest pins every flat source file, both generated plan artifacts, producer/consumer sources, CPU-test evidence and NVBit headers. Do not regenerate it after deployment. Build inputs include all local production headers and the exact plan JSON used to generate the embedded plan SHA.

## Isolated remote execution

Deploy only the manifest files into the fresh directory:

`/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi/native-arguments-r1`

As `xmu`, the following first commands print plans; adding `--execute` performs the bounded step:

```sh
/usr/bin/python3 -B build_controlled.py
/usr/bin/python3 -B build_controlled.py --execute
/usr/bin/python3 -B run_arguments.py --discovery-receipt ../runs/discovery-r1/controller.json
/usr/bin/python3 -B run_arguments.py --discovery-receipt ../runs/discovery-r1/controller.json --execute
```

Compilation uses the existing CPU45 lease and empty GPU visibility. The later capture uses CPU46 and the existing selected-GPU lease; it does not run NCU or require root. Both output directories must be fresh. All seven journals, argument payload/reference SHA, phase/launch/module joins, native return codes, source/build/contract identities and resource cleanup must close before success. A partial or failed run is retained but never qualified.

Output is `../runs/arguments-r1/`, including `controller.json`, `argument-census.json`, the seven observer journals, and the same-process native tensor/module metadata. `controller.json` separately records elapsed time, controller user+system CPU time, and waited-child user+system CPU time; these are one-time capture costs, not simulated GPU latency or direct-trace generation speed.
