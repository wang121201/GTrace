# 同一 direct trace 的 GDDR6 / HBM / HBF stage 回放

本说明对应 `build/multi-backend-validation-r3/{gddr6,hbm,hbf}` 的冻结结果，并单列 `work/p1024d32-capture/discovery-r1` 的新 SGLang discovery。报告生成器只读取 JSON 和配置快照；不重新生成 trace、不运行模拟，也不改变参数。

## 已完成的范围

B1、Decode2、三个 layer-1 MLP 调用片段：`epoch-3-launch-28` GEMV 的前 512 CTA、`epoch-3-launch-29` SiLU 的 1 CTA、`epoch-3-launch-30` GEMV 的前 512 CTA。两个 GEMV 原始 grid 为 7168 和 1024 CTA，因此不能称为完整 Decode2 或完整 8B。当前测量不是 P1024D32，也不是 B8。

共同 post-cache trace 含 590,592 个 128B read 和 1,060 个 32B write，共 75,629,696 B。原 direct cache 初态 cold，调用间不重置，结尾不额外刷新 dirty lines。回放不重复过 GPU cache。固定 direct 地址顺序与精确 cosim 地址调度不同，后者仍是独立模式。

48 CTA 构成一个 stage，共 23 stages，W2 未完成 compute 窗口。每 stage 的全部 memory 完成后才进入单一 compute lane，跨调用完整 barrier，保留后端 row/bank/controller 状态。compute estimate 复用模板流水线资源账，但不执行完整 GPU DAG、寄存器依赖调度或 GPU stall。结果资格为 `EXPLICIT_UNCALIBRATED_STAGE_APPROXIMATION`。

## 生成报告

在仓库根目录运行：

```sh
python3 validation/render-multi-backend-report.py
```

默认回放输入为 `build/multi-backend-validation-r3`；discovery 默认取工作区 `work/p1024d32-capture/discovery-r1`；默认输出为工作区的 `outputs/tilegen-stage-backends-20260918/index.html`。也可显式指定：

```sh
python3 validation/render-multi-backend-report.py \
  --input-root build/multi-backend-validation-r3 \
  --discovery-root /Users/wgs/Documents/Codex/2026-09-17/zhi/work/p1024d32-capture/discovery-r1 \
  --ncu-root /Users/wgs/Documents/Codex/2026-09-17/zhi/work/p1024d32-capture/ncu-three-groups-r1 \
  --observer-root /Users/wgs/Documents/Codex/2026-09-17/zhi/work/p1024d32-capture/observer-r2 \
  --output /Users/wgs/Documents/Codex/2026-09-17/zhi/outputs/tilegen-stage-backends-20260918/index.html
```

生成器校验 result/input/config SHA256、source result receipt、共同 trace 身份、request shape FNV、所有源字节与请求完成账本，以及 `stage_makespan + persistence_tail == full_makespan`。它不重读 85 MB trace 或重跑 source，所以原 trace 的验证证据来自冻结 result/receipt。错误将终止生成；不静默跳过后端。

若确实需要重跑模拟，应使用各目录 `run-receipt.json` 的精确 `argv`，并将结果输出到新的验证目录；本版二进制为 `build/multi-backend-r3/tilegen_replay`，SHA256 `807128398c3e379126b2cb2de60f2b3999a849d7e7ab79e0014a2987af5ea2b8`。报告刷新不执行这些命令。receipt 的 binary SHA 是执行身份；后续源码修改不自动改变冻结结果身份。

## 物理单位与时间分母

- **逻辑流量**：共同的 source post-cache read128/write32 字节。写的调用归属是 eviction trigger，不是 last writer / tensor role。
- **GDDR6 / HBM 物理流量**：原生 HbmDevice 的请求 payload，本次与 source 字节闭合。
- **HBF 物理流量**：HbfController + HbfDevice 的 NAND page payload，包含页读、RMW、mapping / GC 维护；不可与 source 字节互换。
- **HBF controller HBM**：仅服务控制器 buffer / mapping，单独记录，不能并入 source numerator。controller buffer row commands 没有逐条展开。
- **总时间**：cycle 0 至请求完成、最后 compute 和 EOF 持久化 drain 结束。总体 GB/s = source R+W / 总时间，bytes/ns 等于十进制 GB/s。
- **阶段 / 调用时间**：不含全局 EOF persistence tail。跨调用 barrier 使调用时间可求和；stage residence 互相重叠，不能求和。
- **native span**：native finish minus first arrival，包括空隙；不是物理 bus busy。
- **compute/native outstanding 交集**：compute 活跃且 source 请求尚未交付完成的半开区间并集，是代理量，不是 GPU compute / 物理总线的重叠利用率。
- **CPU 分钟**：完整子进程 user+system，包括 HBF trace 预扫描 / seed、hash、回放和 drain；排除编译、direct 生成和 wrapper 元数据准备。`result.host_seconds` 排除了部分初始化，不用于此比较。

## HBF 的关键限制

此运行逻辑 Read 75,595,776 B 对应 NAND Read 2,417,844,224 B，约 31.98×；Write 33,920 B 对应 NAND Write 53,248 B，约 1.57×。逻辑吞吐约 1.01 GB/s 不是 HBF 峰值，也不是产品或 LLM 性能验证。

当前 foreground data-read 路径检查 ready decoded-page cache；未命中即独立 `schedule_read_page`。每个 page-buffer bank 可缓存两个已完成页，未来 fill 不提前算 hit。当前数据读路径未合并同页 in-flight misses，因此同一页的多条 128B 请求可重复触发 4KB 读取。映射页 `mapping_buffer_coalesced_reads` / `mapping_cache_coalesced_misses` 属于另一层，不代表 foreground data read 合并。

代码证据（仓库相对路径）：

- `source/work/hbfsim-latest/upstream/src/host/hbf_controller.cpp`，foreground read 4414–4439；cache bridge 5113 附近；mapping merge 5990、6181 附近。
- `source/work/hbfsim-latest/upstream/src/physical/hbf/hbf_device.cpp`，99–155 的时间相关页缓存。
- `source/hbf_replay_backend.h`，原始 logical byte 请求、parent credits、future completion 和 EOF drain。

本次 1,060 个 32B 写经 write buffer 合并后形成 11 个 data programs（45,056 B）和 2 个 mapping programs（8,192 B）；不是每个 32B 写都产生一个 page program。`write_buffer_merged_bytes` 计数重叠字节，不能以其为零推断无写合并。

HBF all-touched mutable seed 预置 18,461 逻辑页，未计 preload 时间；packed source service address 原样解释为 HBF logical address。初始值、分配生命周期没有恢复。当前配置包含 full-resident mapping、1024-page write buffer、512-page flush threshold、auto-GC、thermal off。HBF credit 是最多 512 source parents，GDDR6/HBM 是最多 4096 parents 加每 channel 32 native burst credits，不能认为并发预算相同。

`drain_pending` 是 native 因果维护屏障。当前 adapter 在 admission 时推进 attached HBM buffer 的 prune frontier；EOF 没有再次 advance 只是保留更多 reservation 历史，不会把 drain 请求提前到 arrival 前。`transfer_controller_buffer` 仍使用真实 `request.arrival_ns`。可在未来大 trace 优化中推进到实际 EOF arrival 以回收状态，但不能推进到尚未发生的 future finish，也不应把这项潜在优化写成已应用修复。

## 配置与资格

GDDR6 实际仍走 HbmDevice generic 命令核心配 GDDR6 数值 overlay，真正 GDDR6 JEDEC controller 未实现；HBM 走 HbmDevice；HBF 走原生 HbfController + HbfDevice。没有为当前三点拟合服务时间。r3 新增接口几何身份准入：HBM x64 / 2 PC / BL8，GDDR overlay x16 / 1 PC / BL16；HBF 必须在配置中显式声明 hbf-standard。几何准入用于防误标，不是硬件精度资格。

GDDR6/HBM refresh=false、replication=false。HBF HBIO 1536 GB/s 是配置的外部接口 envelope，不能直接当持续 NAND 带宽。所有目标 `hardware_timing_calibrated=false`。source 历史 receipt 只锁了 cfg 路径，本次另行快照和锁定 cfg 内容；不能宣称当前快照是历史 cfg 原件的密码学证明。

结果只有 phase 级 Decode2 和 kernel family/module 归属，没有已核验的 weights / activation / KV post-cache R/W 分解。报告不使用这些语义颜色，不把未采样 KV 视作整模零流量。

## 新 SGLang P1024D32 discovery

HTML 的 `#capture-status` 已加入完整 33 阶段图表与 metadata 摘要。新工作负载为 Meta-Llama-3-8B-Instruct、32 层 BF16、B1、P1024、D32、FlashInfer native eager；单次 warmup。固定 prompt ID 为 1000–2023，decode 输入交替 944 / 291，保留 sampling 而不反馈预测 token。CUDA graph、torch compile、radix cache、overlap schedule 均关闭。数值接受状态为 `NOT_ASSESSED`。

Discovery 状态为 `PASS_CAPTURE_CLOSURE_NOT_MODEL_QUALIFICATION`，完整来源是工作区 `work/p1024d32-capture/discovery-r1/controller.json`。自然时间来自 `validate/host/process-2553204/finish.json`：Prefill 195.42323303222656 ms，Decode1–32 的单阶段时间为 46.915584564208984 至 49.32710266113281 ms。它是一次非 NCU CUDA event 测量，包含阶段内 host launch gaps；不能用作 NCU kernel-duration sum 或 NCU 带宽分母。33 段 event 相加为 1703.6007385253906 ms，只是阶段 event 和，不是另一次 full-range 测量。

另一次 metadata 运行使用 module hooks / torch.profiler；其 profiler 时间与 `instrumented_elapsed_seconds` 仅用于诊断，不作为性能基线。Metadata 含 33 阶段 JSON、195 个 parameter descriptors、32 个 module-buffer descriptors、64 个 K/V buffers、12,804 个 module-call records、6,800 个 storage-root records；torch.profiler 共 89,803 个 events，`kernel_count=13,112`。该范围含观测辅助，未闭合 per-kernel phase join 或完整 CUDA API census，不能与旧 native1138 当作同一计数口径。

生成器执行以下本地证据核查：

- controller / validate / metadata 的 input contract 完全相同，重算 canonical JSON SHA。
- 33 阶段的 input IDs、positions、sequence lengths、append slots、完整 KV token slots、输出不反馈、CUDA graph 关闭均匹配冻结输入。
- validate / metadata 的 16 个原生 SGLang Python 文件 SHA 与冻结 source reference 一致；10 个 capture package 文件内容 SHA 与 controller 一致。
- 37 个 metadata 文件大小与 SHA 全部匹配文件清单；manifest SHA 匹配 controller；两子流程退出成功、owned processes drained、GPU quiescent。
- 模型配置和 4 个权重 shard 内容 SHA 展示 controller 已记录的身份；本地报告没有再次读取远端大权重。

初次 discovery 没有 native scope ABI 或 decoded SASS identity；后续 observer r2 已补齐静态 launch / decoded SASS / argument-size layout census。仍未收集完整动态指令 / memory trace、raw argument values 或 typed pointer binding，完整 tilegraph 尚未建立。P1024D32 native TileGen 模型尚未准入，不能从这些 metadata 自动推导精确 memory / compute 程序。

正式 NCU 三组现已完成并导入，18 个 scope 样本全部 PASS，逐项 report/CSV/source/host closure 已通过。仅对应的 Full、Prefill、D1、D8、D16、D32 scope 展示结果；其余 Decode 的 NCU 仍为 N/A，所有新 TileGen 和 R/W 误差列仍为 N/A，不填入旧 P32D2 或三个 MLP kernel 片段数值。静态 observer 首轮因 metadata 配额退出并完成清理；独立 r2 现已成功闭合静态 census，仍不作为完整动态模型资格。报告不记录认证或权限配置细节。

## 正式 NCU 三组的可选接入

`--ncu-root` 默认指向工作区 `work/p1024d32-capture/ncu-three-groups-r1`。controller 缺失或尚非 `PASS_CAPTURE_CLOSURE_NOT_MODEL_QUALIFICATION` 时只显示 pending，不展示部分样本统计。PASS 后必须满足 mode=ncu、groups=3、六个 scopes（Full、Prefill、Decode1、Decode8、Decode16、Decode32）各三次；pilot 不能冒充正式三组。

接入时校验 controller 与 discovery 的相同 input contract / package / GPU / model SHA；每个样本的 `.ncu-rep` 和原始 CSV SHA；CSV 只有一个 app-range action，R/W 单位 byte、duration 单位 ns；CSV 计数与 controller 完全一致；所有实际 host replay 的输入、source、driver、phase 闭合；NCU action PID 必须有对应 host receipt。

统计以同一次 run 的 R/W/time 为原子样本，先计算每次 `(R+W)/duration`，再报告三次均值及样本标准差（ddof=1）。另列 `ratio of means = (mean R + mean W) / mean duration`，明确其一般不等于逐次带宽的均值。展示全部 18 次样本与 CSV 链接，不把独立 scope 的阶段计数求和替代 Full。

也不能将 `Full − Prefill` 作为 Decode 总量，或再除以 32 作为平均 Decode 流量。Decode1 可能与后续所选 Decode 写量明显不同，各 scope 又是独立运行与测量窗口；当前证据不能将差额直接因果归结为 L2 脏行机制。后续需要同状态窗口、边界 cache / 写回账本等独立证据。

NCU duration 与非 NCU CUDA event 的阶段和必须单独列示。Pilot 曾呈现 Full NCU duration 显著大于非 NCU event 和，但这一差异的具体原因尚未分解，不能直接断言只有 profiling overhead。正式报告将按完整三组 receipt 重算差异，不把 pilot 数值写成最终统计。NCU B/time 是该 app-range 计数口径下的比值，不等同无干扰 inference throughput 或持续 bus bandwidth，也不得用于事后拟合模拟器。

HTML 已加入 NCU Read/Write 四个独立面板（Full/Prefill 与 Decode 分开；Read 用 GB，Full/Prefill Write 用 GB，Decode Write 用 MB），以及 NCU 时间 / 同 run B/time 对照图。NCU 误差线只来自正式 group 1/2/3 的样本标准差；CUDA event 只有一次，只画单点，不生成三组误差条。主表 mean 与 SD 使用相同单位，两位小数；SD 显示 0.00 不代表原始数据无变化，精确值见 CSV。

正式三组 Full：Read 507.18 GB、Write 5.83 GB，duration 3489.19 ± 233.74 ms；Prefill：Read 22.45 GB、Write 5.79 GB，duration 217.12 ± 3.06 ms。Decode8 duration 为 151.12 ± 44.84 ms，CV 29.67%；逐次 `(R+W)/duration` 为 107.39 ± 36.38 GB/s。该时间/BW 离散以及相对非 NCU event 的差异尚未被因果分解，不能用于校准无干扰推理时间。采集 elapsed 不是 CPU user+sys；本页只将 replay receipts 中真实记录的 user+sys 称为 CPU 分钟。

独立诊断见 `validation/ncu-timing-diagnostic.json`：18 份正式 NCU host finish 的 `natural_cuda_event_ms` 均为 null，driver 只在 validate 模式创建 CUDA events，因此没有同 run event 可分解时间差。profiling 运行的主机边界区间也更长，差异不只是 CSV 格式/单位问题，但这个单调 host 时钟区间不是 GPU 时间，不能作为 bandwidth 分母，也不能单独确定 profiling、状态和测量边界各自贡献。

## 独立理论 read 对照

这一栏是根据实际 manifest 推导的一次逻辑覆盖账，不是 TileGen 输出，不由 NCU 拟合。195 个参数 root 均唯一、BF16，总大小 16,060,522,496 B；embedding table 为 1,050,673,152 B，单 token 仅取一行 8,192 B。因此非 embedding 参数（含 LM head、norm）各读一次，加 embedding 一行，为 15,009,857,536 B。

KV 读账为 `2(K/V) × 32 layers × 8 KV heads × 128 head_dim × 2 B × (1024 + decode_step)`，每个上下文 token 在全部层共 131,072 B。由此得到：

| 阶段 | KV bytes | 权重一次 + KV bytes |
|---|---:|---:|
| Decode1 | 134348800 | 15144206336 |
| Decode8 | 135266304 | 15145123840 |
| Decode16 | 136314880 | 15146172416 |
| Decode32 | 138412032 | 15148269568 |

renderer 从参数/shape 重算上述数字并检查闭合，只在正式 NCU 三组完成后填入同 scope Read 的均值、SD 和差额；TileGen 列仍为 N/A。one-pass logical footprint 忽略 activation/merge/metadata 流量、cache 命中、sector 与重复读，不能视为 DRAM 精确预测或必然下界。与 NCU 数量接近不验证 trace 的地址/mask/顺序，更不验证写流量或 latency。131,072 B/token 的 KV 新增仅是逻辑持久新增 payload，不是整个 DRAM 写量；真实写回还受缓存影响。

## 三阶段交付状态

1. 三后端接入已经完成，但验证输入仍是旧冻结 Decode2 的三个 MLP 片段。
2. 新 SGLang P1024D32 discovery 与真实 NCU 正式三组均已完成，18 样本按独立 receipt 接入。
3. 新 native TileGen 尚未准入；第 2 步完成不等于第 3 步完成。

早期缺口见 `docs/native-p1024d32-admission.md` 和 `validation/native-p1024d32-admission.json` 的 53 项证据 pin：新 attention Decode grid [9,8,1] 对旧 [1,8,1]；新 CUTLASS / Ampere GEMM、PersistentVariableLengthMergeStates 最初由符号文本差异发现。后续 r2 静态 census 及其独立审计补充 decoded SASS / ABI layout 身份；静态相同不等于动态地址、控制和绑定已经合格。需新 raw arguments、native witness、独立 heldout、bindings、phase 适配，不能只修改 P/epoch。trace 单文件硬上限 64 GiB；HBF 多离散 sparse seed 上限 1,048,576 页，整模实际规模仍须预检。

## Observer r2 静态 census

`work/p1024d32-capture/observer-r2/controller.json` 状态 `PASS_NATIVE_METADATA_CENSUS_ONLY`。Prefill 408 measured launches；每 Decode 397，32 个 Decode 共 12,704；总 measured 13,112。整个 process before/return 共 26,374，包括测量 epoch 外的 warmup/setup 等。330 inspected functions、282 unique decoded code hashes 属于整个静态检查范围，不能当作 measured kernel 家族数。

renderer 校验 controller 对 native-census 的 SHA、build receipt / binary SHA、observer finish SHA、六 journals 的字节长度 / SHA、33 phase 计数和 launch before/return 闭合；workload contract 与 discovery 同源，native SGLang Python source SHA 一致。静态 code hash 是 NVBit decoded instruction rows 的 hash，不是 cubin 文件 hash。ABI 仅记录 argument sizes / parameter layout，没有 raw values 或 typed pointer bindings。

无动态指令 instrumentation、动态 memory addresses、PC 执行 witness 或实际 SM placement；完整 callback coverage 与同进程 CUPTI crosscheck 仍未证明。13,112 与旧 profiler 计数相等不是这两者的独立同进程证明。静态 census 不会把 `native_model_admitted=false` 升级为 true。独立复核与旧工作流比较保存在 `validation/native-p1024d32-census-audit.json`。

独立审计现已 PASS：measured 范围为 33 decoded code hashes / 32 symbols（不同于 process 全范围 330 functions / 282 hashes）；其中 29 个 code hash 与旧 measured 工作流相同，4 个为新 code。10,679 个 launches 的 code / ABI sizes / grid / block / static+dynamic shared / registers / local bytes / launch attributes / CUDA API 与旧候选完全匹配；1,281 个同 code+ABI 但 launch 配置不同；1,152 个新 code launches。上述三类之和闭合为 13,112。

10,679 只是静态复用候选，不代表 raw arguments、tensor 内容、循环控制、mask 或动态地址相同。4 个新 code 包含 Prefill 的 128 个 GEMM launches（1 个 CUTLASS、2 种 Ampere GEMM），以及 Decode 的 1,024 个 MergeStates launches；符号分类是名称分组，不是动态语义证明。报告 importer 校验独立审计对 controller/census/finish/manifest/六 journals 的 pins、measured code/symbol 计数与比较账本后才展示。
