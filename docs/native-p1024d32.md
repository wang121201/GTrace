# Native P1024/D32：准入状态、绑定方案与参数采集合同

本页合并原先四份 P1024/D32 说明，按「结论 → 状态 → 方案 → 采集合同」的顺序排列：

| 来源 | 主题 |
|---|---|
| `native-p1024d32.md` | 准入结论：已采元数据，模型尚未准入 |
| `native-p1024d32.md` | 已完成的输入工作与剩余工作 |
| `native-p1024d32.md` | 原生地址绑定的最小实施方案（逐 family） |
| `native-p1024d32.md` | host argument capture 的实现合同 |

正文按原样保留在下面各节。

## 共用口径

- 输入采集、NCU 测量与新形状 TileGen 地址模型的资格检查是**三个不同步骤**。
- **完整 P1024/D32 TileGen 尚未准入**；本页任何完成项都不能读作准入通过。
- 不同 NCU 范围来自**独立运行**，不能用 Full − Prefill 推导 Decode 流量。
- 未实测的值必须由采集得到，**不得由 plan 提供**。

<!-- 以下正文合并自：native-p1024d32.md native-p1024d32.md native-p1024d32.md native-p1024d32.md -->

## Native P1024D32：已采元数据，尚未准入模型


后续进展（2026-09-19）：正式 NCU 三组与原生静态 observer 均已采集完成；33 阶段、13,112 次 measured launch 的静态 SASS / 参数尺寸 ABI 已闭合，详见 [新 census 独立审计](../validation/native-p1024d32-census-audit.md) 和 [当前进度](../validation/native-p1024d32-progress.json)。**以下是采集完成前的审计快照**，其中“待采静态代码”按当时证据陈述；当前仍缺真实参数值、typed binding、动态访存证据与完整阶段模型适配。后续 host 参数采集的最小接口见 [实施说明](native-p1024d32.md)。

静读审计，未采 GPU、未跑模拟、未改生产代码。源码 HEAD `09d0953958a3`；完整路径、行号与 SHA 见 [机器记录](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/validation/native-p1024d32-admission.json)。**剩余工作包含原生输入、模板与容量适配，不能归结为 NCU 权限。**

当前 `manifest.json` 为 COMPLETE：Llama3-8B、BF16、B1、32 层、P1024/D32、KV pool1280、FlashInfer/eager，固定 token 不反馈；D1 context1025，D32 context1056。元数据明确 `native_scope_abi_enabled=false`、`instruction_memory_trace_collected=false`、`tilegraph_complete=false`，backend-private tensors 不完整。metadata driver 的 `weight_content_hashes_complete=false` 仅描述该 driver：外层 [controller](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/p1024d32-capture/discovery-r1/controller.json) 已记录 config/index 与四个 safetensors 共6文件的完整内容 SHA，且与此 manifest/input contract 闭合；**权重哈希缺失不是当前阻塞项**（本次未重读远端权重）。13112 是 torch.profiler 的 kernel 事件，旧1138是固定 NVBit/ABI registry，**不作同口径数量比较**。这批 profiler 事件没有 code SHA，不能据符号同名认定 SASS 相同。

| 已证实变化 | 当前旧绑定为何不能直接消费 |
|---|---|
| Prefill Norm/SiLU grid32→1024；RoPE `[4,40,1]`→`[128,40,1]` | Norm 固定 rows32；SiLU 只准 Prefill/D1/D2、rows32/1；RoPE 固定 nnz32/1、源 grid/stride。逐行公式可能复用，仍须新根对象/ABI/shape witness 与 heldout 验证。 |
| Prefill attention `[1,1,8]`→`[32,1,8]`；Decode `[1,8,1]`→`[9,8,1]` | 旧只有三相的封存 nodes/控制模板；context1025..1056 的循环、mask、地址、scratch 不可从旧33/34简单复制；网格9不等于已证明循环次数。 |
| 新 profiler 出现 CUTLASS256线程 GEMM、两种新 Ampere GEMM 符号，以及 `PersistentVariableLengthMergeStatesKernel` | 这四种**符号文本**不在旧选中 native journal 中；须采新 SASS/ABI/完整warp PC-mask-memory/shared/barrier/constants 并建模板，不能只换 epoch 或 P。 |
| Decode3..32；新 PID、地址、对象与 launch 序列 | Model 锁定旧 workflow/hash/1138 registry、phase、code/参数/资源/current-process 绑定及 union-map；GEMV 即使尺寸相同也有旧 PID 与120个 nonpointer byte 的硬约束。 |

最小准入顺序：①完成新 native launch/SASS/ABI/分配身份，补私有张量；②按 kernel+shape+context/control 分组采代表 CTA 和独立 heldout，验证动态 PC 顺序、mask、地址、共享内存/屏障/常数；③重建当前工作流、地址规则和控制模板，并保留尚未证明的结构估计标签；④对同一新来源比较 fast materializer 与 fine/source oracle 的 ranges/ordinals/bytes/hash；⑤完整生成、stage replay 后再对齐同范围 NCU。Norm/SiLU/GEMV 等只有**候选复用**资格，旧模板本身的 `estimated_*` / `native_target_qualified=false` 也不能随迁移升级。

容量边界同样需要处理：`--phase-ctas` 目前只支持 GEMV/SiLU/FusedNorm/PlainNorm/Rotary/P28QKV/GEMMO/GEMMGate/GEMMDown 九类，遇 attention/helper/IndexPut/PrefillCopy 会先拒绝；纯 direct 的旧 fine fallback 不等于完整 stage 支持。direct full 仍锁旧1138，非 full 选择限64调用/1500万源节点。trace 默认8GiB、硬上限64GiB，144B/record+224B envelope，最多477218586条；**新负载实际记录数尚未测得**，不能承诺单文件可容纳。HBF 多个离散 seed range 合计限1048576页（4GiB payload），单个合并连续 range 才走 compact；外层8388608页 bitmap/预算不覆盖这一 sparse 限制，parent credits最多512。需先做容量预检，必要时实现保持缓存/顺序的分段或流式传输及可扩展 seed image，不能仅调大命令参数。

主要依据：[新 manifest](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/p1024d32-capture/discovery-r1/metadata/artifacts/manifest.json)、[profiler launch metadata](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/p1024d32-capture/discovery-r1/metadata/artifacts/kernel_launches.json)、[旧固定 Model](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-full-runtime-r4/model_dispatch.h:18)、[direct 准入](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/direct_native.h:83)、[HBF seed 限制](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/hbf_replay_backend.h:68)。本页不替代正在独立开展的 observer 或 NCU 采集，也不将旧 P32D2/P512D32/B8P1024D4 结果改名为新目标结果。

## Native P1024/D32 implementation status — 2026-09-19


The independent branch remains `codex/tilegen-trace-cosim-20260918-r1`, at `/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo`. B8 remains excluded. Direct generation and precise cosimulation keep their shared native memory providers and 32 B dirty-sector writeback. The existing stage replay supports GDDR6, HBM and HBF, with explicitly approximate compute overlap.

### Completed input work

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

### Validation achieved

- Producer: 104 ledger checks, actual CUDA callback layouts for four APIs, six rejection cases and the entire generated launch plan pass CPU tests under ASan/UBSan. Independent mock buffers verify every captured byte. Full-plan serialization is 30,436,724 B, maximum row 4,029 B; this is synthetic test output, not GPU capture.
- Consumer: 35 CPU tests pass; the final generator reproduces the exact plan/header from the real source journals and static evidence.
- Typed decoder: 15 CPU tests pass. All 646 applicable calls from the old real argument corpus agree with its independently typed models (GEMV 259, PlainNorm 3, FusedNorm 192, SiLU 96, Rotary 96).
- Independent code review checked the shared norm ABI, GEMV epilogue alias and nonpointer bytes, RoPE positions, and the successful-controller/census requirement. No additional defect was found after the controller publication-order fix.
- GEMV typed adapter: 11 sealed templates, each at first/middle/last CTA, match the old JSON address oracle and packet binding across 58,476 records / 1,867,944 lane addresses; 486 negative cases pass. See `validation/gemv-typed-binding.json`.
- SiLU typed adapter: all 96 old calls / 1,088 CTAs match the old address oracle across 25,903,104 lane addresses. Its new single-CTA path matches all 64 old Decode calls / 1,523,712 lane addresses; 255 rejection checks pass under ASan/UBSan. See `validation/silu-typed-binding.json`.
- The complete CPU native engine compiles with the final source identities unchanged: `build/native-typed-adapters-r2/build-receipt.json`. This is a build check, not a new simulation or benchmark. The r1 build was rejected by its source-identity guard during a concurrent source correction; only r2 is qualified for use.

CPU tests and old-corpus regressions do not establish a new P1024/D32 dynamic memory witness. The decoder preserves real phase labels, reports unsupported calls explicitly and keeps all model-admission flags false. RoPE position contents remain unknown until separately observed.

### Runtime and remaining work

The new package has not yet been built with remote NVCC or run on a GPU. Its remote destination is a fresh `native-arguments-r1` directory under the existing XMU task. Upload awaits explicit destination authorization requested after automatic approval review rejected the transfer. No source payload was transferred by the rejected call.

Local GEMV and SiLU typed address-provider adapters are complete as CPU-validated candidates. They reuse the existing materializer and a small shared identity header; they do not put new calls into the old sealed `Model` or into the live whole-workload factory. Actual new argument capture, same-run object/root bindings, source-transfer witnesses, remaining kernel families, phase coverage and streaming capacity still precede a full native P1024/D32 result. The static five-family priority covers 8,257 calls, not all 13,112; GEMV and SiLU are only two of those five families.

Capture runtime will be reported in minutes using measured controller user+system CPU, waited-child user+system CPU, and elapsed wall time separately. No simulated bandwidth or GPU inference latency will be inferred from those capture costs.

## P1024/D32 原生地址绑定最小实施方案


实现更新：GEMV 与 SiLU 的 typed 地址入口已完成旧语料 CPU 对照；公共 materializer 被复用，原 Model 验收未放宽。它们仍是候选入口，尚未接入新工作负载的完整 factory；见 [当前实现状态](native-p1024d32.md)。以下保留原实施方案快照。

状态：**只读设计，尚未实现、尚未为新模型准入**。审查基线 `6ccfcc5a42851351371ef728c1013c999d271dee`；证据快照来自 observer-r2 与独立静态审计。机器可读字段、逐族代码 SHA、参数大小、47 项证据 SHA 见 [native-p1024d32-binding-plan.json](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/validation/native-p1024d32-binding-plan.json)。

### 1. 结论与适配边界

优先复用 **8,257 次 call 的五个现成 provider**：把不可变源程序、每 call 的 typed 参数与地址视图拆开，再接现有 `tiny_full::KernelBinding`。不复制整个 `canonical_full::Model`，不放松旧 sealed input 验收，不用后 cache DRAM trace 代替原生 pre-cache 访存生成。

| 现有 provider | 新工作负载全部同族 calls | code + ABI + 静态配置完全匹配候选 | 首轮范围 |
|---|---:|---:|---|
| GEMV | 4,129 | 4,129 | Prefill 的 1 次 + 每个 Decode 的 129 次 |
| PlainNorm | 33 | 32 | 每个 Decode 1 次 |
| FusedNorm | 2,112 | 2,048 | 每个 Decode 64 次 |
| SiLU | 1,056 | 1,024 | 每个 Decode 32 次 |
| Rotary | 1,056 | 1,024 | 每个 Decode 32 次 |
| 合计 | 8,386 | **8,257** | 不包含 129 次形状变化的 Prefill call |

全工作负载 13,112 次 calls 中，静态完全匹配候选是 10,679 次；上表之外还有 2,422 次静态候选，以及 2,433 次静态变化/新代码 calls。**这五族完成也不是整模型完成**。计数来自 [native-p1024d32-census-audit.json](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/validation/native-p1024d32-census-audit.json) 的 `code_groups` 与 `comparison`。静态配置比较包含 grid、block、shared、registers、local bytes、launch attributes 和 CUDA API；不包含参数值、运行时 PC/mask、地址或数据相关分支等价证明。

旧模型对跨层/跨 call 模板迁移本来保留 `estimated_address/estimated_compute` 和隐式依赖未完整的边界。新方案不能因静态一致而改写为原生动态全覆盖，更不能宣称硬件时序已经校准。

### 2. 最小公共接口，不改缓存和调度职责

现有公共边界是 [source.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-tiny-full-r1/source.h:9) 的 `KernelBinding`：`nodes(cta)` 保留 compute/control/shared/barrier、issue/completion dependency；`memory(cta, member)` 返回带原始方向、mask 对应 lane、width、range 和 source order 的 `MemoryDescriptor`。

建议新增独立准入模块产生不可变 `AdmittedCallView`，只让通过该模块的对象进入 provider。以下为接口草图，**不是当前已存在的 API**：

```cpp
// Identity and evidence are owned outside the inner address loop.
struct ObjectRebase { uint64_t source_base, target_base, extent; };
struct AdmittedCallView {
    NativeCallIdentity actual_capture;
    TemplateLease source;       // Explicit source/program/register/formula pins.
    TypedFamilyArguments args;  // GEMV / norm / SiLU / Rotary variant.
    ObjectViews objects;       // Root, offset, extent, stride, alias, evidence.
    QualificationReceipt transfer;
};
AdmittedCallView admit_new_call(const Manifest&, const SourcePool&);
std::unique_ptr<tiny_full::KernelBinding>
bind(const AdmittedCallView&, const coupling::ServiceMapper&);
```

`SourcePool` 可复用 [SourceBundle / SourceCatalog](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/driver-pooled-fusednorm-r1/streaming.cpp:18)（均匀 CTA）和 [Rotary class SourceBundle / SourceCatalog](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-mixed643-driver-r1/rotary_class_support.h:184)。它们能独立验证并加载旧封存 program/register artifacts；无需构造整个 workflow Model。保留其源文件 pin、证据 pin、source census、资源及内存上限校验。两类 pool receipt 明确没有开启 target transfer；新 call 的迁移准入另做 receipt，不能借加载旧 source 取得新资格。

旧 `Model` 构造器与验证器继续接收原封存输入；仅把验证后的内容交给公共 materializer。新准入模块交给同一个 materializer。不要新增 `skip_validation`，不要把新 call 包装成旧 call，不改旧 `seals()`，也不以 `fixture=true` 绕过 native evidence。

现有 [hybrid Bindings::bind](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-hybrid-cache-r1/bindings.h:115) 依赖 canonical `Prepared` 与旧 Model 的指针身份，不能直接喂新 JSON。应在它下方抽取 provider 的已验证输入构造路径，再由两套独立 admission factory 调用。fine Builder 同样消费共享 materializer；默认 cosim 的依赖、调度、背压、L1/L2 和逐 32B dirty-sector 写回保持原有实现。direct 继续使用固定 CTA/member 顺序，其 cache 后结果不能宣称与 cosim 顺序完全相同。

### 3. GEMV：最小增量是 typed rebase 构造器

入口：[PreparedAddress](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-gemv-host-r2/prepared_address.h:7) → [PreparedMemory](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-gemv-host-r2/prepared_memory.h:19) → [packet_binding::CallBinding](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-packet-integration-r1/binding.h:19) → [GemvBinding](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-tiny-basic-r1/gemv_binding.h:10)。fine 路线已经使用同一 [GEMV modeled_builder.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-gemv-host-r2/modeled_builder.h) materializer。

`PreparedAddress` 内部只需要 `p::Program&` 与每个 source object 的 `source_base/target_base/target_bytes`。新增这两个 typed 参数的内部构造器、让旧 `Model` 构造器委托即可。地址仍为 `target_base + program.address(lane,width,cta) - source_base`，保留 source offset、目标 extent、checked-add 和 service map 覆盖检查。`PreparedMemory` 的每条指令 width、lane ordinals、range grouping 来自原 source/range plan，不重写地址规则；旧 JSON oracle 可保留为验证 callback。

参数证据：[gemv model.py](/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/canonical-gemv-model-r2/model.py:15) 和 [gemv plan.json](/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/canonical-gemv-model-r2/plan.json)。该 ABI 是 **一个 152B 参数**，little-endian 解码为 38×u32。weight 指针为 word 0/1，input 为 4/5，output 为 8/9，epilogue input 为 12/13 且必须与 output 相等。其余 **120B 逐字匹配选定模板**，不得只验 K/N。`K/N`、BF16 extent、no-tail 条件、对象 128B alignment remainder、source object 到 role 的映射均保持。

旧 GEMV 有 11 个 phase/source template，覆盖下列五个 shape（K/N 单位为 BF16 元素）。配套 JSON 的 `families[0].explicit_template_candidates` 保留完整 template pin 与 code/ABI/grid；新 call 需要逐项匹配，而不是按 module 名称直接绑定。

| 用途 | K | N | grid.x | block.x/y | 旧 template 后缀 |
|---|---:|---:|---:|---|---|
| QKV | 4096 | 6144 | 1536 | 16/4 | epoch-2/3-launch-21 |
| O projection | 4096 | 4096 | 1024 | 16/4 | epoch-2/3-launch-26 |
| gate/up | 4096 | 28672 | 7168 | 32/4 | epoch-2/3-launch-28 |
| down | 14336 | 4096 | 1024 | 32/4 | epoch-2/3-launch-30 |
| lm_head | 4096 | 128256 | 16032 | 16/8 | epoch-1-launch-405 / epoch-2/3-launch-362 |

不同 phase 的两个相同 shape 模板不能在未验证原 source/register/control 与 120B scalar 内容前自动合并；保留显式 lease 与 qualification。

必须隔离的旧约束在 [model_plan.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-gemv-driver-r2/model_plan.h:18)：固定 PID/start、259 calls、固定 plan/template/seals 和旧 workflow key。新输入只替换身份与指针证据，不把旧 validator 的这些检查改成任选。某些 logits/output 与 Prefill gather 的旧 scratch 绑定是结构推断，不能自动继承为新进程实测 root；缺少实际 view/root 时该 call 继续 unresolved。

### 4. Plain / Fused Norm：共享公式，保留 shared/barrier

入口：[NormRegisterBinding<Policy>](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-tiny-norm-r1/norm_binding.h:22)，全局地址取自各族 `Model::records_for/address`；源寄存器拓扑、shared lanes 与 barrier 原样保留。把这些依赖改成持有 immutable source + typed `NormAddressView`，旧构造器照旧校验后委托。fine [PlainNorm Builder](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-norm-driver-r1/modeled_builder.h) / [FusedNorm Builder](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-fusednorm-driver-r1/modeled_builder.h) 应调用同一 typed 地址函数。

| 参数 index | PlainNorm | FusedNorm | 大小 |
|---:|---|---|---:|
| 0 | input | input | 8B |
| 1 | weight | residual | 8B |
| 2 | output | weight | 8B |
| 3 | hidden_size | d | 4B |
| 4 | input_stride_elements | input_stride | 4B |
| 5 | output_stride_elements | residual_stride | 4B |
| 6 | weight_bias_bits | weight_bias_bits | 4B raw float bits |
| 7 | eps_bits | eps_bits | 4B raw float bits |

字段来源是 [norm model.py](/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/canonical-norm-model-r1/model.py) / [fusednorm model.py](/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/canonical-fusednorm-model-r1/model.py) 与对应旧 plan 的 `typed_arguments`，不是从新 tensor 名字猜测。两族 **ABI SHA 相同、参数语义不同**，必须 code SHA + ABI 联合选 decoder。

首轮保持 hidden/d/stride=4096、BF16、bias bits=0、eps bits=`0x3727c5ac`、block=[32,16,1]，以及原记录 PC/opcode/mask/width。地址公式为 `base(role) + 2 * (cta * role_stride + 8 * (32*warp + lane))`；weight 的 stride=0。所有 lane 的完整 16B 访问仍应在 view 内。Plain/Fused 每 CTA 2991/3391 nodes、64/80 global records、34/98 shared records、32 barrier nodes 等原 source census 不动。

旧 [PlainNorm phase dispatch](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-norm-driver-r1/model_plan.h:23) 把非 Prefill 路由到 Decode1，[FusedNorm phase dispatch](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-fusednorm-driver-r1/model_plan.h:23) 则直接用 phase 查表。新输入用显式 `template_id` 并保留真实 Decode3..32 phase，不能靠 phase 改名。固定 PID、3/192 calls、旧 total CTA 限额属于旧 envelope；新 envelope 用捕获 count 和原 source/live graph 边界，不整体放开上限。

### 5. SiLU：直接复用已编译的 744 条指令跨度

入口：[SiLU PreparedMemory](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-silu-driver-r1/prepared_memory.h:9) 同时服务 [SiluBinding](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-tiny-basic-r1/silu_binding.h:26) 和 [fine Builder](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-silu-driver-r1/modeled_builder.h)。新增 typed source-record/formula/object-view 构造器，保留旧 `Model` 委托路径。只编译一次每个 memory record 的 first/stride/bytes/object bounds，CTA 展开仍是 checked `first + cta*stride`。

ABI：out u64、input u64、d u32（三个独立参数），见 [silu model.py](/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/canonical-silu-model-r1/model.py) / 旧 plan `typed_arguments`。固定 d=14336、输入每行 57344B、输出每行 28672B、block1024、input/out 非重叠。[原地址公式](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-silu-driver-r1/model_plan.h:44) 使用 warp/lane、occurrence、part：16B 指令 `element=(tid+1024*occurrence)*8`；2B 指令 `element=8192+tid+1024*occurrence`；地址 `base + cta*stride + part + 2*element`。保留 744 records 与 full32-lane、width 2/16B 的 bijection。

当前 `PreparedMemory` 只接受 CTA count 1/32；本轮优先的 1024 次 Decode call 都是 1 CTA，不需要放宽此守卫。新 Prefill 是形状变化域，若后续准入才在新 typed envelope 接受实际 count 并验证全 grid，不能删除旧 1/32 guard 就宣布通过。旧 96 calls、P32/D1/D2 phase 与总 CTA1088 约束继续属于旧封存路径。

### 6. RoPE：复用地址公式必须同时保留 CTA class

入口：[RotaryBinding](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-tiny-rotary-r1/rotary_binding.h:44) 持有 class `SourceBundle`；[Model::address](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-rotary-driver-r1/model_plan.h:25) 可抽为 `RotaryAddressView`。它不能退化成统一 CTA 平均模型；`catalog.bindings[cta]` 的 early-exit、mask、body record、warp node count、register dependencies 必须保留。

18 个独立参数来自 [rotary model.py](/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/canonical-rotary-model-r1/model.py:17)：index0..5 为 q/k/q_rope/k_rope/cos_sin_cache/pos_ids（各8B）；6..9 为 nnz/num_qo_heads/num_kv_heads/rotary_dim（各4B）；10..17 为 q、k、q_rope、k_rope 各自的 stride_n/stride_h（各8B）。Decode 首轮维持 nnz=1、Q32/KV8/dim128、q Nstride4096、k Nstride1024、Hstride128，且 q=q_rope、k=k_rope。

**raw pos_ids 指针不等于 position 内容。** 必须关联同运行的 device-control 值或有明确同步证据的 positions 观测；不能硬填旧32/33。新 Decode 的候选 positions1024..1055 只能在实际捕获/控制证据核对后写入 manifest。地址中的 cos/sin 访问依赖 `positions[idx]*128`，其余 tensor 地址依赖 CTA 的 q/k head 域、strides 和 PC 分支；不能单纯按 phase 推断。

旧 source catalog 只含 Prefill/D1/D2，且绑定精确 CTA grid。新 Decode candidate 的 grid 与旧相同，可在独立 class/mask/control 验证后显式 lease 合适 source；新 Prefill grid 的扩大不在首轮 8,257 范围。不要把 Decode32 改名为 Decode2，也不要用旧160-CTA Prefill class 表声称覆盖新5120-CTA grid。

### 7. 新 manifest 与准入顺序

建议新 schema `P1024D32_TYPED_BINDING_INPUT_V1`，当前尚无该解析入口。只保存一次 source catalog，再保存轻量 call views；不复制每 CTA DAG/完整 Model。required fields 见配套 JSON。关键关系如下：

1. `actual_capture_identity` 使用参数采集运行本身的 PID/start/context/stream；raw 参数 journal 与该运行的 native before/return、code/ABI、尺寸和 chronology 严格连接。observer-r2 的 PID2561177/start876347196 仅是候选快照。若参数采集是新进程，提供显式跨采集 phase/ordinal/code/geometry 对照，不能替换实际 launch identity 或复用旧指针。
2. `typed_call_bindings` 保存 raw vector pin、decoder code/ABI、typed scalars、对象 root/offset/extent/stride/alias、显式 template_id、new witness receipts。根/私有 scratch/allocator lifetime 未覆盖时标 unresolved，不根据指针相近就拼对象。
3. 所有已选择 calls 共用实际 source VA 的 canonical union/service map，保留重叠 alias 和根生命周期证据；服务地址只是 packed address space，不是硬件物理地址。旧 full1138 service-map seal 保持原值，不复用于新进程。
4. 新 raw 参数闭合后可建立待验证 typed view；只有新独立 witness 逐 instruction/CTA/warp/PC-occurrence/mask/direction/width/lane address 通过，才授予对应访存迁移资格。旧公式自产 hash 不是独立 witness。compute/register/control/shared/barrier 迁移另验，不由地址或 R/W 总和自动取得。
5. 未覆盖的 call 必须 fail closed。允许明确列出遗漏 call 的局部验证；禁止把局部调用相加伪作整模型、把 NCU 总量吻合当 trace 正确、或用 NCU 时间拟合准入。

最小实施顺序：GEMV typed rebase（利用现成 PreparedMemory）→ SiLU typed PreparedMemory → norm typed 地址 view → RoPE typed view/class lease → 独立 factory 接 common KernelBinding → 完成其余4,855 calls 的单独适配。每步保留旧 sealed corpus 的逐条 descriptor/projection 回归，并新增新参数/witness证据。五族仍按现有 compute/cache 调度执行，不以 memory-only replay 替代默认精确 cosim。

应拒绝：同 ABI 不同 code 的 norm；SHA/arg-size/index 不符；跨进程指针；GEMV 120B 非指针内容变化、output alias/tail 不符；越界或无 root 的 scratch；SiLU d/alias/mask/width 变化；RoPE 无 position 数据、early-exit class 不符；新 Prefill grid 硬套旧模板；R/W 总数相同但 lane/order 不同；源 pin 在 finish 前改变；局部模型冒充全工作负载。

本次仅新增此方案与配套 JSON，未修改运行时、旧数据或报告，未做 GPU/SSH/模拟运行。静态匹配是复用候选证据；后续真实采集的独立验收决定可否准入。

## Native P1024/D32 host argument capture: next implementation contract


Implementation update: the host producer, generated plan, independent consumer and CPU tests are now implemented in `capture/native-arguments-r1/`. Remote build/capture is pending. See [implementation status](native-p1024d32.md); the design snapshot below is retained as the implementation contract, not the latest progress report.

Status: design only, 2026-09-19; observer-r2 has now passed independent metadata census audit. See `validation/native-p1024d32-census-audit.json` and its Markdown companion. This document does not admit P1024/D32 to TileGen and does not introduce a new executable or change a frozen capture package.

The completed run contains **13,112 measured launches = Prefill 408 + 32 × Decode 397**, 33 measured kernel decoded-code hashes and 32 symbols. The larger static population (330 functions / 282 hashes) includes related functions. Exact old-workflow comparison finds 29 shared measured hashes and four new ones: CUTLASS GEMM (64 Prefill calls), two Ampere GEMM variants (32 calls each), and MergeStates (32 calls per Decode, 1024 total). There are 10,679 old code/ABI/launch-configuration matches, 1281 old code/ABI matches with changed launch configuration, and 1152 new-code calls. These are metadata reuse candidates, not dynamic-address equivalence.

Observed plan inputs are 58,905 argument slots, **3,975,666 bytes of expected host argument payload**, maximum 18 arguments / 1240 bytes per argument / 1248 bytes per launch, and API counts `cuLaunchKernel=10967`, `cuLaunchKernelEx=2145`. Those payload bytes are computed from the real ABI sizes and have **not yet been captured**. Metadata used 299,144,830 bytes before finish under the 1 GiB cap. The old 8 MiB argument-journal limit is unsuitable: hex alone requires 7,951,332 bytes before 58,905 argument descriptors/hashes and launch metadata. Final serialized bounds still need to be generated and sealed in the new plan.

The next artifact should capture the actual **host parameter byte vectors immediately before each measured CUDA launch**, while preserving the current uninstrumented native SGLang workload. A successful result proves argument transport and launch correspondence. It does not prove dynamic memory addresses, executed instructions, typed object layouts, allocation lifetimes, or simulator correctness.

### 1. Exact sources to reuse

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

### 2. Produce the plan from successful observer-r2 evidence

Inputs are the completed `runs/observer-r2/controller.json`, `native-census.json`, `observer/process-*/{finish.json,launch-journal.jsonl,functions.jsonl,static-instructions.jsonl,scope-journal.jsonl,lifecycle.jsonl,allocation-journal.jsonl}`, and `native/artifacts/{manifest.json,module_calls.json,...}`. Require the real successful controller, its census SHA, all six journal hashes, exact workload/source pins and 33 epoch closures before generating anything.

The proposed `make_argument_plan.py` takes these files, never a prompt-length scaling factor. It emits a new `SG_NATIVE_ARGUMENT_CAPTURE_PLAN_V1` JSON plus generated header. Preserve native event order; keys are `(epoch_id, epoch_launch_ordinal)` / `epoch-E-launch-O`. Each entry seals:

- Phase, forward ID, role, raw module scope, actual decoder layer and module kernel ordinal. Derive module ordinal by counting launches under `(epoch_id, current run call_id)`; derive decoder ancestry from `module_calls.json`, including shared Rotary instances.
- Exact decoded-SASS SHA and kind, argument size vector and layout SHA, CUDA API name, grid/block, static/dynamic shared sizes, and launch attributes.
- Reference symbol and source native launch ID for audit; do not use function name alone for identity.

Plan-level fields seal workload contract SHA; source controller/census/finish/journal/artifact hashes; phase order; per-epoch launch counts; API counts; total arguments/raw bytes; maximum arguments, argument width and bytes per launch; measured kernel-code/ABI population. Verify contiguous ordinals independently from the journals. Function IDs, native launch IDs, PIDs, handles, pointer values and module call IDs are **current-process bindings**, not expected cross-process constants. A fresh run may have a different initialization launch count. Preserve stream/context topology by rebinding within the fresh process rather than comparing opaque handles across processes.

If a launch attribute contains an opaque event/handle, do not silently normalize it: initially reject that plan as unsupported or add a separately specified identity binding. Current supported simple attributes should remain exact.

All values above come from successful observed metadata. The successful run is now available to generate the plan, but no argument plan/header has been generated by this design audit. Its resulting counts must not be copied from old `1138=408+365+365`, guessed as `408+32×365`, or inferred from the number of transformer layers.

### 3. Minimal host producer API

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

### 4. Values that must be measured, never supplied by the plan

| Data | Required source / remaining boundary |
|---|---|
| Actual kernel scalar and struct bytes, pointer bit patterns, strides, dimensions, leading dimensions, KV/page-table pointer fields, flags | Copy real host parameter buffers in the fresh entry callback. Names, shapes and ABI sizes cannot determine their values. |
| Current launch/scope/function/context/stream identity and return status | Current callback and current-process scope/module records; join to the plan by observed ordinal/code/ABI, not old process IDs. |
| Typed field boundaries, signedness, padding, pointer relocation and tensor semantic role | Later versioned ABI decoder backed by the exact source/type layout and same-process tensor/allocation evidence. Raw bytes alone do not establish these. |
| Device data referenced by pointers, page-table contents, per-lane active/predicate masks, dynamic PCs, effective addresses, actual CTA/SM order | Not provided by this host-only capture. Require separately collected and qualified native witnesses or typed source lowering with its own validation. |
| Per-kernel NCU timing or traffic | Separate profiling evidence and a justified phase/launch join; parameter capture runtime is not a kernel timing measurement. |

### 5. Bounds, validation, and failure behavior

Generate exact expected measured totals from the plan: `N = launches`, `A = sum(argument_count)`, `B = sum(sum(argument_sizes))`, per-epoch counts, and API populations. Use overflow-checked arithmetic. Enforce plan bounds before reading/copying host buffers; enforce entry/order limits before incrementing state. Finish requires exactly N entries and successful paired returns, A arguments, B raw bytes, every planned epoch, no duplicate/missing key and no pending launch.

Keep independent hard safety ceilings as well as these exact totals. Determine journal/row limits from the new observed maximum ABI and a conservative bound for the actual serialized schema (hex adds `2×B`, plus hashes and bounded metadata), and seal the chosen limits. Unit-test boundary acceptance and one-byte-over rejection. Do not retain the old 8 MiB argument/8192-byte row/32 MiB launch/8 MiB module/10,000 module-call caps without checking the new census. Old maxima 18 arguments, 1240 bytes per argument and 1248 bytes per launch were P32/D2 observations, not universal ABI limits.

The r2 total metadata cap is 1 GiB and its reserve is 128 KiB; a seventh journal consumes that same total if written with `State::emit`. Admit a run only when r2's observed bytes plus the sealed argument budget and finish reserve fit the new cap. Otherwise create a separately reviewed larger-cap source revision or a separately bounded ledger file; never bypass accounting. Preserve native artifact, log, disk/RSS, overall wall-time, launch/function/instruction and process-cleanup limits. Plan compilation itself must reject oversize or unsupported metadata before a GPU run.

Consumer checks are independent of producer counters: verify all seven file SHA/lengths, newline/row bounds, duplicate-key rejection, exact source/build/contract, 33 scopes, all before/return pairs and argument references, actual raw-byte lengths/hex/SHA, current-process ancestry and argument totals. Independently count all launches enclosed by each epoch, including detecting an incorrectly unmarked launch inside an epoch. Require the new final status, for example `PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY`; a metadata-only finish is insufficient.

On unknown ABI/packed transport, plan drift, inaccessible/null parameter, missing/duplicate launch, bad return, quota, signal, deadline or incomplete closure, the run fails and no qualified result is published. Retain partial files with a failed receipt for diagnosis. Only terminate this run's owned children through the existing controller; release leases after owned-process drain. A host read fault must yield child failure, never synthetic bytes or a successful footer.

### 6. Minimum tests and independent rerun

Before any GPU execution, port the existing host mock with a small synthetic multi-epoch plan. Test real independent scalar/struct buffers; mutation of buffers after `before` must not change captured bytes. Cover direct/Ex callback layouts, before/return reference identity, packed/null transport, code/layout/grid/scope/attribute drift, negative/overflow widths, missing/extra/reordered/duplicate entries, failed returns, cross-process binding, shared-module ancestry, newline/hash/hex corruption, row and aggregate quotas. Compare the generated header's normalized plan to its JSON, not just a duplicated code loop.

Then use one fresh host-only capture against the already sealed r2 plan. Treat it as an independent launch/ABI holdout: all measured ordering/code/layout/geometry/scope invariants must match exactly or the plan fails without auto-learning. Across processes, pointer bytes and struct padding need not match; raw argument SHA is an integrity hash for that run. Do not claim byte-for-byte cross-run determinism. A further independent capture can test stable typed scalar fields only after their decoder has been justified. No number of matching metadata/argument runs substitutes for a dynamic memory witness.

### 7. Smallest future change boundary

Create a new `capture/native-arguments-r1/` package with a copied r2 observer plus the argument hook/ledger; `make_argument_plan.py`; generated `argument-plan.json`/header; generalized host-only argument consumer; CPU tests; original GPU-free builder with explicit source closure; thin controller and manifests. Keep original workload, observer-r1/r2, old C sampler and production simulator untouched. Reuse controller/resource/model checks. Preserve diff/source hashes and new binary build receipt. Build under CPU45's existing lock with GPU visibility empty; a subsequent GPU capture needs its own existing GPU lease and explicit dispatch after other sampling finishes.

This completes a transport input step only. P1024/D32 typed bindings, memory/program lowering, selected-CTA dynamic witnesses, stage compute estimates and NCU comparison remain separate qualification steps.

