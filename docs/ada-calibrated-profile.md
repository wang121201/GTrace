# RTX 4000 Ada：接入 r2 校准基线与 r3 实验配置

本次默认入口更新为 `r2-adaptive`。它对应 XMU r2 的 `01_measured_adaptive`，仍属于**部分验证的默认候选**；不是已全面匹配真实硬件的配置。r3 已完成实验，但其交付明确不提升默认，因此仅提供显式实验选项。原 tuner-v1 保留，旧 r1 工作树和历史结果未覆盖。

## 来源和实际改动

参考目录分别为：

- `/home/xmu/nvidiagds/codex-runs/rtx4000-ada-cache-calibration-20260922-r2/configs`
- `/home/xmu/nvidiagds/codex-runs/rtx4000-ada-cache-calibration-20260922-r3/configs`

原 `/home/xmu/nvidiagds/simulators/accelsim2.0/util/tuner/NVIDIA_RTX_4000_Ada_Generation` 未被校准任务覆盖。接入时以新交付的文件 SHA 固定版本，不能仅看旧 tuner 目录或实验轮次编号。

| 参数 | tuner-v1 | r2 默认候选 | 对本实现的影响 |
|---|---|---|---|
| shared carveout 选档 | 0/8/16/32/64/100 KiB | 32/64/100 KiB | 资源需求为零时，L1由128变为96 KiB；实际缓存容量参与命中/淘汰 |
| L2 ROP附加延迟 | 238 cycles | 239 cycles | 单独标记为附加延迟；功能 traffic 模式不执行时钟 |
| L1 替换 | LRU | LRU | 保持默认替换规则 |
| L1/L2 MSHR | 384/192 | 不变 | 未借本轮数据宣称确定真实数量 |
| L2、sector、memory topology | 40 MiB、32 B、10×2 | 不变 | 沿用已实现的地址解码和sector规则 |
| trace.config 指令参数 | 原值 | 不变 | 没有混入另一组 Tensor/SP 校准候选 |

r2 默认原配置 SHA256 为 `dabaf5334ea681c419e5ea8d820ead7c03246dea059b0ca812d163c7968ac79c`；trace 配置仍为 `c4e4d8e85e9049af5694afca10b031fae4eafcdd06f0dbd3d8042d7195db890b`。所有配置和实际差异见 [配置目录](../configs/rtx4000-ada-calibrated) 与 [来源清单](../provenance/ada-calibration-reference.json)。

## 可执行配置选择

| `--profile` | 每SM L1 | 组织及替换 | 输入要求 |
|---|---|---|---|
| `tuner-v1` | 128/120/112/96/64/28 KiB | 4 sets，LRU | 历史资源规则；用于回归 |
| `r2-adaptive`（本入口默认） | 96/64/28 KiB | 4 sets，LRU | 线程、寄存器、shared需求、grid必须完整 |
| `r2-shared64` | 64 KiB | 4×128 ways，LRU | 明确观测 shared=65536 B |
| `r2-shared100` | 28 KiB | 4×56 ways，LRU | 明确观测 shared=102400 B |
| `r3-fifo-shared32` | 96 KiB | 64×12 ways，FIFO | 明确观测 shared=32768 B；实验候选 |
| `r3-fifo-shared64` | 64 KiB | 64×8 ways，FIFO | 明确观测 shared=65536 B；实验候选 |
| `r3-fifo-shared100` | 24 KiB | 64×3 ways，FIFO | 明确观测 shared=102400 B；实验候选 |

FIFO 已在共享 L1 实现中接通：整条line分配时记录年龄，命中、sector缺失或填充完成不刷新插入顺序；dirty保护、pending ticket和默认LRU行为保持。r3 的 24 KiB 是原交付的有效模型容量，不是28 KiB的显示取整，也不是识别了真实24 KiB硬件。64 sets每增加一way需要8 KiB，不能静默将它变成28或32 KiB。

`r2-adaptive` 的 kernel 记录可携带 `observed_shared_carveout_bytes`。观测为64/100 KiB时自动绑定对应的r2固定配置，并在结果与收据中记录实际profile及SHA。无观测时按资源规则选择最小32/64/100 KiB档，结果明确标为 `resource_rule_not_observed`，不能把它当成NCU实测划分。固定和r3配置没有匹配观测值会拒绝运行；观测无法容纳全部驻留CTA的shared需求也会拒绝，不暗中降低occupancy。

```jsonl
{"type":"kernel","name":"observed-kernel","threads_per_cta":64,"registers_per_thread":32,"shared_bytes_per_cta":0,"grid_ctas":48,"observed_shared_carveout_bytes":65536}
{"type":"memory","allocation_id":0,"cta":0,"warp":0,"sm":0,"op":"read","ranges":[{"lane":0,"address":0,"bytes":4}]}
```

`shared_bytes_per_cta` 是实际每CTA请求量，`observed_shared_carveout_bytes` 是实际每SM L1/shared划分；两个概念不能相互替代。

## 运行与复现

在本仓库根目录运行；输出目录必须是新的。XMU使用已验证的g++11：

```sh
python3 tools/import_ada_calibrations.py --check
python3 ada_profile.py --compiler g++ --profile r2-adaptive \
  --input tests/fixtures/ada-sector.jsonl --output build/r2-example --emit-trace
```

显式历史对照：

```sh
python3 ada_profile.py --compiler g++ --profile tuner-v1 \
  --input tests/fixtures/ada-sector.jsonl --output build/v1-example --emit-trace
```

实验FIFO配置须提供相应观测元数据，不可将原无观测夹具直接交给固定profile：

```sh
python3 ada_profile.py --compiler g++ --profile r3-fifo-shared100 \
  --input observed-shared100.jsonl --output build/r3-fifo-example --emit-trace
```

`result.json`逐kernel报告L1容量、set/way、替换策略、carveout来源、resolved_profile、read/write流量；`receipt.json`包含请求及实际解析profile的SHA、输入/源码/二进制身份和CPU/wall分钟。compile时间另列。原有32 B sector、lazy部分写、L2跨kernel保持和不强制末尾flush继续生效。

新JSONL请求格式仍为 `GTSIM_ADA_POSTCACHE_SECTOR_V1`，不是原TGCSIM01；默认 `run.py` 的历史 direct/cosim 模式没有被静默改成这个新入口。MSHR/互连/分区队列的周期实现边界见 [fine差异说明](ada-fine-timing-gaps.md)。

## 参考时延的实际消费范围

新增 `make_rtx4000_ada_calibrated_internal_config` 将34/239/324这些参考阶段常量映射为GTSim内置模型的L2命中273 cycles、cold miss端到端597 cycles；实际完成路径的测试验证不重复叠加命中延迟，普通step与service_epoch一致。这里是core-cycle内部参考模型，不是Accel-Sim周期等价或HBFSIM校准。旧带宽、队列以及fine的whole-line/store-bypass/RFO机制保留，详见 [内部时延合同及限制](ada-calibrated-timing.md)。

32B dirty模式要求有限backend credit；测试用64-credit薄适配器包住真实DRAMModel，未解除旧生产入口对自动内部backend的限制。功能traffic入口本身仍不执行时钟。

本地正式验证：18个C++测试程序全通过；显式v1端到端139项通过；新CLI端到端30次调用、192项检查通过。收据位于 [validation/ada-calibrated-r2](../validation/ada-calibrated-r2)。

## 如何理解校准效果

r2：66条件中，**按NCU实际shared划分选择01/02/03**后，L2全局读请求sector WAPE由68.38%降到15.78%；这不是单一默认01的成绩，默认划分22条件下01的对应WAPE仍为29.60%。实验工作集小于40 MiB L2，主要验证L1过滤和冷读唯一sector计数。不能把这些数字解释为DRAM字节误差、L2写回精度或LLM误差。

r3：冻结FIFO候选在240个留出条件上的WAPE为21.84%，r2参考为23.54%；但是32 KiB默认共享划分子集由20.97%退化到25.39%，所以未提升默认。静态FIFO的选择存在训练并列，不能据此断言真实GPU采用FIFO。

MemGen另有r3 LRU64sets候选和300条件回放；它的配置身份、映射和证据快照与本次Accel-Sim r3 FIFO交付不同，不能仅因都叫r3就当成统一配置。本轮没有改变MemGen生产分支。

本接入不宣称已经消除decode write gap。完整LLM验收仍需要匹配kernel输入、地址/alias、cache前史、shared划分和NCU采样窗口，再比较分阶段read/write，不能用上面的L1读微基准替代。
