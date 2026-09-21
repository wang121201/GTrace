# RTX 4000 Ada：r3 参数候选、串行缓存模型与留出验证

更新时间：2026-09-22（服务器日期）。**本轮完成 300 条件、1500 次硬件计数采集、108 组模型比较、10 次完整 Accel-Sim 校验；新候选不提升为默认配置。** 默认 32 KiB 共享划分下，留出误差变大；总体改善不足以替代 r2。

## 术语与证据范围

GPU（Graphics Processing Unit）指本机 RTX 4000 Ada 图形处理器。SM（Streaming Multiprocessor）指流式多处理器；CTA（Cooperative Thread Array）是线程块；warp 是 32 个线程的执行组。本轮每个内核仅一个线程发出读取，每个地址依赖前一次读取值。L1/L2 是一级/二级缓存；DRAM（Dynamic Random-Access Memory）是显存。NCU（NVIDIA Nsight Compute）提供硬件计数；SASS 指 GPU 机器指令汇编；CUDA（Compute Unified Device Architecture）是 NVIDIA 计算平台。KiB=1024 字节，MiB=1048576 字节；sector 指 32 字节计数单位。cache line 指模型中的 128 字节缓存行，最多有四个独立有效 sector。

LRU（Least Recently Used）淘汰最久未访问的缓存行；FIFO（First In First Out）按分配顺序淘汰缓存行；RANDOM 指使用固定种子的 xorshift32 伪随机数从一个集合的有效路中选受害者。集合（set）是候选地址可放入的一组槽位，集合索引为 0 到集合数减一；相联路（way）是同一集合可驻留的行数。MSHR（Miss Status Holding Register）跟踪尚未完成的未命中请求；本轮串行试验不识别其数量。XOR（exclusive OR）是异或，本轮备选哈希为行号与其右移集合位数的异或；线性哈希直接取行号低位。这些是有效模型定义，不是硬件结构声明。

r1/r2/r3 是实验轮次，不是准确度等级。配置编号 01–09 只作索引。JSON（JavaScript Object Notation）保存结构化结果；CSV/TSV 分别是逗号/制表符分隔表格；SHA-256 为文件完整性散列。WAPE（Weighted Absolute Percentage Error，加权绝对百分比误差）= 全部条件的预测绝对误差之和 / 硬件中位数之和 ×100%，范围为 0 到无上限。本轮主指标限定为从 L1 发往 L2 的全局读 sector，不把重试、输出写入或指令读取混进分母。

## 回答配置究竟改了什么

r2 的 `01_measured_adaptive` 相对 tuner 只改两项参数：

```text
-gpgpu_shmem_option      0,8,16,32,64,100 -> 32,64,100
-gpgpu_l2_rop_latency    238 -> 239
```

第一项取消 0/8/16 KiB 共享划分，使零动态共享内存时模型 L1 从 128 KiB 变为 96 KiB；第二项是 r1 指针追逐差分延迟取整后的 L2 附加 cycles（模型时钟周期），不是总延迟。默认仍为 LRU、384 个 L1 MSHR、192 个 L2 MSHR、40 MiB L2 和 32 字节 sector。trace.config 中的指令延迟没有在 r2 改动。r2 没有修改缓存算法的实现代码，其他 r2 文件只是固定共享划分、FIFO 或 MSHR 数量的对照。

r3 新增了独立串行参考回放器 `cache_policy_replay_r3.cpp`，实现同步填充的 sector 有效位、LRU/FIFO/随机替换与可变集合组织，用于区分假设。它不是已经接入 memgen 的后端，也没有改变 Accel-Sim 核心源码。实际导出的新 Accel-Sim 配置仍使用其已有算法与选项。

## r3 新增的三份可运行配置

| 文件夹 | 适用的实测共享划分 | 模型 L1 | 集合 × 相联路 × 行大小 |
|---|---:|---:|---|
| 07_fifo_static_shared32 | 32 KiB | 96 KiB | 64 × 12 × 128 B |
| 08_fifo_static_shared64 | 64 KiB | 64 KiB | 64 × 8 × 128 B |
| 09_fifo_static_shared100 | 100 KiB | 24 KiB | 64 × 3 × 128 B |

每个文件夹均含完整 `gpgpusim.config` 与 `trace.config`。相对 r2 默认，它们改变三项：

```text
-gpgpu_adaptive_cache_config 0
-gpgpu_unified_l1d_size 0
-gpgpu_cache:dl1 S:64:128:<12或8或3>,F:T:m:L:L,A:384:48,16:0,32
```

`adaptive=0` 表示使用显式固定容量；`unified_l1d_size=0` 关闭模拟器中的统一容量扩展计算，不表示真实硬件没有统一缓存。F 表示已有 FIFO 替换，其他读写和队列字段沿用 r2；MSHR 没有借本轮数据重新确定。必须根据 NCU 实测共享划分选择配置，不能把这三个静态文件用于全部内核。

**24 KiB 是模型取整后的拟合容量。** 64 集合 ×128 字节每路占 8 KiB，名义 28 KiB 只能按参考网格向下取整为三路，即 24 KiB。原 Accel-Sim 自适应逻辑反而会四舍五入为四路、32 KiB，所以这里必须明确固定容量。这不是识别了物理 24 KiB L1。模型训练最优存在 FIFO/LRU、线性/XOR 共四个并列组合；按提前规定的字典序选中 FIFO，不构成 FIFO 胜过 LRU 的证据。

`selected_accelsim_mapping.json` 记录所有差异和配置散列；`selected_reference_models.json` 是查看留出误差前冻结的选择，不能根据后来的留出结果偷偷换模型。

## 测试设计与完成情况

总计 300 个条件：288 个 `.ca`（允许 L1 缓存）条件与 12 个 `.cg`（绕过 L1 命中路径）对照。三个共享划分分别扫描八个容量点；步长为 32/128 字节，顺序为正向循环/固定随机循环/每遍正反交替，地址偏移为 0/4096 字节，每内核八遍。工作集 KiB 是地址跨度，不是“实际读取的数据字节数”；128 字节步长只触及每条 128 字节行的一个 sector。

只用 `.ca` 正向且偏移 0 的 48 条件拟合，其余 240 条件留出；12 个 `.cg` 单列控制，不混入留出误差。候选网格包含三种策略、32/128 字节分配单位、4/16/64 集合、线性/XOR 索引、0.875/1/1.125 容量比例，共 108 组。随机策略固定三颗种子（11/29/47），按三次预测均值评分，不能挑选最有利种子。

- 每个条件做 5 次正式原生数值验证，共 1500 次全部正确；请求序列导出另有 300 次数值检查。每条链的终点和独立求和公式均核验。
- 每个条件做 5 次 NCU，共 1500 次。使用物理 GPU 2，应用重放、冷缓存、未锁频；已释放该 GPU。
- CUDA 的 memcheck（非法访存）与 initcheck（未初始化读取）均零错误。参考回放器的五个手算契约用例在普通构建和 AddressSanitizer/UndefinedBehaviorSanitizer（地址与未定义行为检查）构建下均通过。
- L1 访问数与地址 oracle（独立地址计数）、L2 冷读未命中与唯一 sector 数、L1 读未命中到 L2 读请求守恒，分别 **1500/1500** 精确；`.cg` 控制 **60/60** 精确。
- 六个采样内核的 **94208 个实际 SASS 读地址** 与独立导出序列逐项一致，活动掩码均只有一个线程。反汇编确认前一条读取值参与下一条地址计算。
- 六次完整 Accel-Sim 回放与参考 LRU 的读访问、命中、L2 请求完全一致；四次导出配置回放与冻结参考预测完全一致。四次属于配置映射检查，不能冒充四个新硬件条件。
- 同一条件五次测量的极差 ≤ max(2 原始计数单位，中位数×2%) 定义为稳定。读取计数组稳定 **163/300**，DRAM 读字节稳定 **16/300**，DRAM 写字节稳定 **300/300**；写零值稳定不说明写模型已被识别。本轮不校准 DRAM 字节，不将 L2 未命中×32 字节冒充其硬件实测值。

## 留出结果与不提升默认的原因

以下每行训练分母 48、留出分母 240；“通过”指绝对误差 ≤ max(2 sectors, 硬件中位数×2%)。所有条件均保留，包括测量波动较大的边界。

| 模型角色 | 训练 WAPE | 留出 WAPE | 留出通过 |
|---|---:|---:|---:|
| best_overall | 15.48% | 21.84% | 98 / 240 |
| r2_lru_baseline | 16.53% | 23.54% | 96 / 240 |
| best_LRU | 15.48% | 21.38% | 98 / 240 |
| best_RANDOM | 30.15% | 19.71% | 90 / 240 |

best_overall 是按训练选择的 FIFO 候选；r2_lru_baseline 是旧参数的串行 LRU 参考；best_LRU/best_RANDOM 是提前保留的同类策略训练最优对照。随机策略在留出上较好，也不能据此把已经使用过的留出集变成新的训练选择依据。本轮 21.84% 不能与 r2 的 15.78% 直接比较，因为测试集合和执行方式不同。

| 实际共享划分 | r2 参考留出 WAPE | 冻结 FIFO 候选留出 WAPE |
|---|---:|---:|
| 32 KiB | 20.97% | 25.39% |
| 64 KiB | 21.21% | 18.34% |
| 100 KiB | 36.80% | 19.23% |

每个划分在此表均有 80 个留出条件。默认共享划分从约 20.97% 退化到 25.39%，因此不能只依据总误差从 23.54% 降为 21.84% 宣布升级。新配置保留为实验候选，默认继续保留 r2。当前结果不足以仅靠这组容量、集合数和 LRU/FIFO/随机开关描述全部观察；仍需研究替换状态、分配粒度与访问顺序。串行模型的改善也不能直接外推到多 warp 的 hbserve 展开流，本轮没有完成该端到端验收。

## 失败记录与交付

导出配置的第四次回放已成功结束，但最初手算校验把 28 KiB 大于 24 KiB 简化成“全部未命中”，错误预期 7168。实际为 4480：64 集合中有 32 集合各四条行而溢出，另外 32 集合各三条行仍可保留命中。它与检查前已冻结的参考预测完全一致。纠正的是校验器手算预期，模型、硬件数据和该次模拟器输出未修改或重跑；初始源码与错误说明保留。

远程证据根目录：`/home/xmu/nvidiagds/codex-runs/rtx4000-ada-cache-calibration-20260922-r3`。本地镜像：`D:\codexdataspace\remote-sync\xmu\rtx4000-ada-cache-calibration-20260922-r3`。`analysis/summary_r3.json` 为全部分组统计，`hardware_aggregates.json` 保留五次硬件重复，`training_predictions.csv` 和 `selected_predictions.csv` 保留模型预测，`reference_crosscheck.json` 保留真实模拟器交叉检查。配置见本目录各子文件夹，原始计数在 `ncu/`，实际 SASS trace 在 `tracing/`，可复现程序在 `source/` 和 `bin/`。

图表在 `D:\codexdataspace\reports\rtx4000-ada-cache-calibration-20260922-r3\serial-cache-comparison.png`，展示未参与拟合的固定随机循环。单一最新总报告仍为本地 session 下 `outputs/RTX4000_Ada_tuner_validation.md`；其中明确区分三轮结果。

`evidence_manifest.json` 在证据根目录记录普通文件的字节数和 SHA-256，trace 定位符号链接单独登记，不重复打包。子助手：启动 0，完成 0，活跃 0。
