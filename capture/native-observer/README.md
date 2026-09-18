# Native observer wrapper

Separate from the frozen capture package. No original source or binary is changed. Default invocation prints a plan; only `--execute` launches the unprivileged native driver. No NCU or torch profiler is loaded.

Stage the exact manifest files at:
`/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi/observer`

The sibling `../capture/` must have manifest SHA `fe559d9bca3a03446e801a37c0a544a09694a22cf9b028909c0af3df478d4884`. The wrapper imports its workload, model identity checks, existing GPU/CPU leases, process ownership, deadline/RSS/log handling and cleanup. It requires a successfully closed discovery receipt from that same package. Both directories are verified before and after execution.

After discovery has drained, as xmu:

```sh
/usr/bin/python3 -B run_observer.py --discovery-receipt ../runs/discovery-r1/controller.json
/usr/bin/python3 -B run_observer.py --discovery-receipt ../runs/discovery-r1/controller.json --execute
```

Default output is new `../runs/observer-r1/`; never overwrite or reuse it. The existing r3 `.so` is loaded only after SHA verification (`1248ba7c81c4bd35acb28a972fa1edd16d1993d3b2eb2c6d5d70fcfdbb3666c7`). It is the only LD_PRELOAD. Actual argv is the new native driver with P1024/D32/capacity1280 and no `--profile`. The scope ABI marks all 33 phases. The precreated outer observer root has the library's hard 256 MiB cap; native host artifacts/logs retain the reused controller's 1 GiB cap, 48 GiB RSS, 64 MiB logs and default 1800-second step deadline. These are separate limits, not a combined 1 GiB output guarantee.

`native-census.json` is written only after successful host/process closure, exact six-journal hashes, exact static-instruction count/SHA, successful before/return launch pairs, all 33 ordered epoch pairs, native phase/module/decoder ancestry and argument-size layout hashes. Launch ordinals are derived from the new journals. No old 1,138-launch census is embedded. It reports grid/block, native resource attributes, static SASS identity and ABI **sizes**; raw argument values, typed pointers, dynamic PCs/addresses, cubin SHA and native model admission remain false.

The library source and historical executed receipt are detailed in the sibling planning document `capture/native-observer-plan.md` in the local repository. This package does not use the old three-phase Python audit unchanged. Its generalized closure is tested locally using a synthetic 33-phase fixture and hash/static/pair/ABI/process/epoch negatives. Local tests do not establish remote NVBit loading or new workload compatibility.

Prepare and freeze locally with `python3 -B prepare_package.py`. Do not regenerate hashes after upload. No GPU operation occurs during preparation.
