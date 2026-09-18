# Native P1024D32：已采元数据，尚未准入模型

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
