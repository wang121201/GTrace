# Native P1024/D32 implementation status — 2026-09-19

The independent branch remains `codex/tilegen-trace-cosim-20260918-r1`, at `/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo`. B8 remains excluded. Direct generation and precise cosimulation keep their shared native memory providers and 32 B dirty-sector writeback. The existing stage replay supports GDDR6, HBM and HBF, with explicitly approximate compute overlap.

## Completed input work

The real native SGLang B1, Llama3-8B BF16, P1024/D32 workload has a closed discovery run, three groups of NCU samples and observer-r2 static metadata. These are separate runs with matching frozen workload contracts; NCU profiling duration is not natural inference duration. Full native TileGen traffic, phase timing and backend predictions for this new workload remain unavailable.

`capture/native-arguments-r1/` now implements the next input step: copying actual host parameter buffers immediately before native CUDA launches, plus independent journal/launch/return validation. This does not dereference GPU pointers or instrument dynamic instructions. The original r1/r2 captures and workload are unchanged.

The generated plan is derived from the closed observer-r2 evidence:

| Item | Value |
|---|---:|
| Measured launches | 13,112 = 408 + 32 × 397 |
| Host argument slots | 58,905 |
| Expected raw payload | 3,975,666 B |
| Argument journal cap | 32 MiB |
| Complete observer cap | 1 GiB |

These are expected capture counts, **not newly measured parameter bytes**. Plan SHA-256: `c109636a9064917d630feb1213108209831a1b24a5ae8f8452be298f5757d9e5`. Sealed package upload-manifest SHA-256: `5be5d50ca2c326feccd7de44aa148bf4e9cbf8d07eb1e0b4748769040b119051`.

The 30.1 MB generated plan and 568 KB compiled header remain local generated artifacts, excluded from Git. Their generator, exact hashes, test receipts, package manifest and reproduction commands are versioned. See `capture/native-arguments-r1/README.md` to regenerate them from the frozen observer-r2 evidence.

## Validation achieved

- Producer: 104 ledger checks, actual CUDA callback layouts for four APIs, six rejection cases and the entire generated launch plan pass CPU tests under ASan/UBSan. Independent mock buffers verify every captured byte. Full-plan serialization is 30,436,724 B, maximum row 4,029 B; this is synthetic test output, not GPU capture.
- Consumer: 35 CPU tests pass; the final generator reproduces the exact plan/header from the real source journals and static evidence.
- Typed decoder: 15 CPU tests pass. All 646 applicable calls from the old real argument corpus agree with its independently typed models (GEMV 259, PlainNorm 3, FusedNorm 192, SiLU 96, Rotary 96).
- Independent code review checked the shared norm ABI, GEMV epilogue alias and nonpointer bytes, RoPE positions, and the successful-controller/census requirement. No additional defect was found after the controller publication-order fix.
- GEMV typed adapter: 11 sealed templates, each at first/middle/last CTA, match the old JSON address oracle and packet binding across 58,476 records / 1,867,944 lane addresses; 486 negative cases pass. See `validation/gemv-typed-binding.json`.
- SiLU typed adapter: all 96 old calls / 1,088 CTAs match the old address oracle across 25,903,104 lane addresses. Its new single-CTA path matches all 64 old Decode calls / 1,523,712 lane addresses; 255 rejection checks pass under ASan/UBSan. See `validation/silu-typed-binding.json`.
- The complete CPU native engine compiles with the final source identities unchanged: `build/native-typed-adapters-r2/build-receipt.json`. This is a build check, not a new simulation or benchmark. The r1 build was rejected by its source-identity guard during a concurrent source correction; only r2 is qualified for use.

CPU tests and old-corpus regressions do not establish a new P1024/D32 dynamic memory witness. The decoder preserves real phase labels, reports unsupported calls explicitly and keeps all model-admission flags false. RoPE position contents remain unknown until separately observed.

## Runtime and remaining work

The new package has not yet been built with remote NVCC or run on a GPU. Its remote destination is a fresh `native-arguments-r1` directory under the existing XMU task. Upload awaits explicit destination authorization requested after automatic approval review rejected the transfer. No source payload was transferred by the rejected call.

Local GEMV and SiLU typed address-provider adapters are complete as CPU-validated candidates. They reuse the existing materializer and a small shared identity header; they do not put new calls into the old sealed `Model` or into the live whole-workload factory. Actual new argument capture, same-run object/root bindings, source-transfer witnesses, remaining kernel families, phase coverage and streaming capacity still precede a full native P1024/D32 result. The static five-family priority covers 8,257 calls, not all 13,112; GEMV and SiLU are only two of those five families.

Capture runtime will be reported in minutes using measured controller user+system CPU, waited-child user+system CPU, and elapsed wall time separately. No simulated bandwidth or GPU inference latency will be inferred from those capture costs.
