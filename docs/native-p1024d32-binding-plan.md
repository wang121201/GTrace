# P1024/D32 原生地址绑定最小实施方案

状态：**只读设计，尚未实现、尚未为新模型准入**。审查基线 `6ccfcc5a42851351371ef728c1013c999d271dee`；证据快照来自 observer-r2 与独立静态审计。机器可读字段、逐族代码 SHA、参数大小、47 项证据 SHA 见 [native-p1024d32-binding-plan.json](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/validation/native-p1024d32-binding-plan.json)。

## 1. 结论与适配边界

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

## 2. 最小公共接口，不改缓存和调度职责

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

## 3. GEMV：最小增量是 typed rebase 构造器

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

## 4. Plain / Fused Norm：共享公式，保留 shared/barrier

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

## 5. SiLU：直接复用已编译的 744 条指令跨度

入口：[SiLU PreparedMemory](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-silu-driver-r1/prepared_memory.h:9) 同时服务 [SiluBinding](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-tiny-basic-r1/silu_binding.h:26) 和 [fine Builder](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-silu-driver-r1/modeled_builder.h)。新增 typed source-record/formula/object-view 构造器，保留旧 `Model` 委托路径。只编译一次每个 memory record 的 first/stride/bytes/object bounds，CTA 展开仍是 checked `first + cta*stride`。

ABI：out u64、input u64、d u32（三个独立参数），见 [silu model.py](/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/canonical-silu-model-r1/model.py) / 旧 plan `typed_arguments`。固定 d=14336、输入每行 57344B、输出每行 28672B、block1024、input/out 非重叠。[原地址公式](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-silu-driver-r1/model_plan.h:44) 使用 warp/lane、occurrence、part：16B 指令 `element=(tid+1024*occurrence)*8`；2B 指令 `element=8192+tid+1024*occurrence`；地址 `base + cta*stride + part + 2*element`。保留 744 records 与 full32-lane、width 2/16B 的 bijection。

当前 `PreparedMemory` 只接受 CTA count 1/32；本轮优先的 1024 次 Decode call 都是 1 CTA，不需要放宽此守卫。新 Prefill 是形状变化域，若后续准入才在新 typed envelope 接受实际 count 并验证全 grid，不能删除旧 1/32 guard 就宣布通过。旧 96 calls、P32/D1/D2 phase 与总 CTA1088 约束继续属于旧封存路径。

## 6. RoPE：复用地址公式必须同时保留 CTA class

入口：[RotaryBinding](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-tiny-rotary-r1/rotary_binding.h:44) 持有 class `SourceBundle`；[Model::address](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/source/work/tilegen-full-r1/canonical-rotary-driver-r1/model_plan.h:25) 可抽为 `RotaryAddressView`。它不能退化成统一 CTA 平均模型；`catalog.bindings[cta]` 的 early-exit、mask、body record、warp node count、register dependencies 必须保留。

18 个独立参数来自 [rotary model.py](/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/tilegen-full-r1/canonical-rotary-model-r1/model.py:17)：index0..5 为 q/k/q_rope/k_rope/cos_sin_cache/pos_ids（各8B）；6..9 为 nnz/num_qo_heads/num_kv_heads/rotary_dim（各4B）；10..17 为 q、k、q_rope、k_rope 各自的 stride_n/stride_h（各8B）。Decode 首轮维持 nnz=1、Q32/KV8/dim128、q Nstride4096、k Nstride1024、Hstride128，且 q=q_rope、k=k_rope。

**raw pos_ids 指针不等于 position 内容。** 必须关联同运行的 device-control 值或有明确同步证据的 positions 观测；不能硬填旧32/33。新 Decode 的候选 positions1024..1055 只能在实际捕获/控制证据核对后写入 manifest。地址中的 cos/sin 访问依赖 `positions[idx]*128`，其余 tensor 地址依赖 CTA 的 q/k head 域、strides 和 PC 分支；不能单纯按 phase 推断。

旧 source catalog 只含 Prefill/D1/D2，且绑定精确 CTA grid。新 Decode candidate 的 grid 与旧相同，可在独立 class/mask/control 验证后显式 lease 合适 source；新 Prefill grid 的扩大不在首轮 8,257 范围。不要把 Decode32 改名为 Decode2，也不要用旧160-CTA Prefill class 表声称覆盖新5120-CTA grid。

## 7. 新 manifest 与准入顺序

建议新 schema `P1024D32_TYPED_BINDING_INPUT_V1`，当前尚无该解析入口。只保存一次 source catalog，再保存轻量 call views；不复制每 CTA DAG/完整 Model。required fields 见配套 JSON。关键关系如下：

1. `actual_capture_identity` 使用参数采集运行本身的 PID/start/context/stream；raw 参数 journal 与该运行的 native before/return、code/ABI、尺寸和 chronology 严格连接。observer-r2 的 PID2561177/start876347196 仅是候选快照。若参数采集是新进程，提供显式跨采集 phase/ordinal/code/geometry 对照，不能替换实际 launch identity 或复用旧指针。
2. `typed_call_bindings` 保存 raw vector pin、decoder code/ABI、typed scalars、对象 root/offset/extent/stride/alias、显式 template_id、new witness receipts。根/私有 scratch/allocator lifetime 未覆盖时标 unresolved，不根据指针相近就拼对象。
3. 所有已选择 calls 共用实际 source VA 的 canonical union/service map，保留重叠 alias 和根生命周期证据；服务地址只是 packed address space，不是硬件物理地址。旧 full1138 service-map seal 保持原值，不复用于新进程。
4. 新 raw 参数闭合后可建立待验证 typed view；只有新独立 witness 逐 instruction/CTA/warp/PC-occurrence/mask/direction/width/lane address 通过，才授予对应访存迁移资格。旧公式自产 hash 不是独立 witness。compute/register/control/shared/barrier 迁移另验，不由地址或 R/W 总和自动取得。
5. 未覆盖的 call 必须 fail closed。允许明确列出遗漏 call 的局部验证；禁止把局部调用相加伪作整模型、把 NCU 总量吻合当 trace 正确、或用 NCU 时间拟合准入。

最小实施顺序：GEMV typed rebase（利用现成 PreparedMemory）→ SiLU typed PreparedMemory → norm typed 地址 view → RoPE typed view/class lease → 独立 factory 接 common KernelBinding → 完成其余4,855 calls 的单独适配。每步保留旧 sealed corpus 的逐条 descriptor/projection 回归，并新增新参数/witness证据。五族仍按现有 compute/cache 调度执行，不以 memory-only replay 替代默认精确 cosim。

应拒绝：同 ABI 不同 code 的 norm；SHA/arg-size/index 不符；跨进程指针；GEMV 120B 非指针内容变化、output alias/tail 不符；越界或无 root 的 scratch；SiLU d/alias/mask/width 变化；RoPE 无 position 数据、early-exit class 不符；新 Prefill grid 硬套旧模板；R/W 总数相同但 lane/order 不同；源 pin 在 finish 前改变；局部模型冒充全工作负载。

本次仅新增此方案与配套 JSON，未修改运行时、旧数据或报告，未做 GPU/SSH/模拟运行。静态匹配是复用候选证据；后续真实采集的独立验收决定可否准入。
