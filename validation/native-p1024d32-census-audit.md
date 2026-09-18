# P1024/D32 native metadata: independent census audit

**PASS_METADATA_CENSUS_AUDIT_ONLY**. No GPU execution or simulation was performed by this audit.

Observed 13112 measured launches across 33 epochs; 33 measured decoded-SASS hashes and 32 kernel symbols. All six journal SHA/lengths, paired successful launch returns, scope/module ancestry, complete epoch enclosure, static instruction indices/PCs/counts/hashes and ABI size layouts passed independent checks.

The separately inspected static function population is 330 functions / 282 hashes. It is not the measured kernel-family population. Metadata used 299,144,830 bytes against 1,073,741,824 bytes.

| Symbol class (name grouping only) | Prefill | Decode total | Full |
|---|---:|---:|---:|
| ElementwiseHelper | 3 | 32 | 35 |
| TokenPoolWrite | 1 | 0 | 1 |
| Position | 1 | 0 | 1 |
| CopyOrCast | 38 | 96 | 134 |
| ReductionHelper | 1 | 0 | 1 |
| Scan | 6 | 64 | 70 |
| KVIndices | 1 | 32 | 33 |
| IndexSelect | 1 | 32 | 33 |
| RMSNorm | 1 | 32 | 33 |
| CUTLASS_GEMM | 64 | 0 | 64 |
| Rotary | 32 | 1024 | 1056 |
| PrefillAttention | 32 | 0 | 32 |
| IndexPut | 64 | 2080 | 2144 |
| Ampere_GEMM | 64 | 0 | 64 |
| FusedAddRMSNorm | 64 | 2048 | 2112 |
| SiLU | 32 | 1024 | 1056 |
| IndexRead | 1 | 0 | 1 |
| GEMV | 1 | 4128 | 4129 |
| ArgMax | 1 | 32 | 33 |
| ClampPosition | 0 | 32 | 32 |
| DecodeAttention | 0 | 1024 | 1024 |
| MergeAttentionStates | 0 | 1024 | 1024 |

Compared with the old canonical native P32/D2 workflow and its pinned launch journal (1138 launches / 32 measured code hashes), this run has 4 new measured code hashes and 4 new symbol strings. Exact code/ABI/geometry matches in the JSON are reuse candidates only: argument values, tensor identity, dynamic control and actual memory accesses are not proved.

| Comparison with old canonical workflow | Launches |
|---|---:|
| exact_code_ABI_launch_configuration_candidate | 10679 |
| same_code_ABI_changed_launch_configuration | 1281 |
| new_code | 1152 |

## New symbol strings

- `ampere_bf16_s16816gemm_bf16_256x128_ldg8_f2f_stages_32x3_tn`
- `ampere_bf16_s1688gemm_bf16_128x128_ldg8_f2f_stages_32x1_tn`
- `void cutlass::Kernel2<cutlass_80_tensorop_bf16_s16816gemm_relu_bf16_256x128_32x3_tn_align8>(cutlass_80_tensorop_bf16_s16816gemm_relu_bf16_256x128_32x3_tn_align8::Params)`
- `void flashinfer::PersistentVariableLengthMergeStatesKernel<8u, 16u, 8u, 4u, __nv_bfloat16, __nv_bfloat16, int>(__nv_bfloat16*, float*, int*, __nv_bfloat16*, float*, unsigned int, unsigned int*, unsigned int)`

Raw parameter bytes, typed pointer binding, dynamic PC/memory witnesses, actual CTA/SM placement, cubin identity and exhaustive callback coverage remain unqualified. Native model admission remains **false**.

Machine audit: `/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/validation/native-p1024d32-census-audit.json`.
Source run: `/Users/wgs/Documents/Codex/2026-09-17/zhi/work/p1024d32-capture/observer-r2`.
