# RTX 4000 Ada：缓存配置与内部参考时延

本页合并原先三份内容相互重叠的 Ada 配置说明：

| 来源 | 主题 | 当前状态 |
|---|---|---|
| `ada-calibration.md` | tuner-v1 功能缓存与结构入口 | 历史，需显式 `--profile tuner-v1` |
| `ada-calibration.md` | r2 校准基线（默认）、r3 FIFO 实验 | **当前默认 `r2-adaptive`** |
| `ada-calibration.md` | r2 内部参考时延的单位与接入 | 生效 |

三份原文的正文按原样保留在下面各节，未做改写。原先在每份文档开头分别重复一遍的
共用边界，在此统一说明，不再逐节重复。

## 三者共用的口径

- 这些配置是**封存 Accel-Sim tuner / 校准交付的导入**，不是对 NVIDIA 未公开硬件
  设计的还原，也没有宣称还原真实 L2 hash。
- 功能回放入口 `ada_profile.py` 执行 32 B sector、known-byte、lazy-write 规则与
  adaptive L1，但**不执行 GPU 计算时钟、warp stall、HBFSIM 或周期级带宽**。
- fine 结构入口 `gtsim-ada-accelsim-structure-only-v1` 继续使用既有 GTSim 异步完成
  与时序机制，仍为 128 B fill / write-miss RFO / 整 line 有效性。**它不是** direct
  sector 功能路径的时序等价版本，也**没有**新的硬件精度验收结论。
- MSHR（384/192）、互连、逐 memory-subpartition 队列与 L2 端口等只**导入了字段**，
  尚未作为与 Accel-Sim 等价的周期机制执行。不能把「字段存在」写成「机制已实现」。
- fine 侧尚未实现的部分、及其最小改动边界，集中列在
  [Ada fine 时序缺口](ada-fine-timing-gaps.md)，本页不再重复。

<!-- 以下正文合并自：ada-calibration.md ada-calibration.md ada-calibration.md -->

## 按 XMU Accel-Sim tuner 对齐的 RTX 4000 Ada 配置


本页记录历史 tuner-v1。在新 r2 工作树中复现本页需显式 `--profile tuner-v1`；当前默认及校准版本见 [r2/r3说明](ada-calibration.md)。

新增 `accelsim-rtx4000-ada-v1`：将 XMU tuner 的 SM 资源、缓存几何和地址映射明确导入，并提供可运行的 **32 B sector 功能缓存回放**。它用于检查相同访存输入经过这套缓存规则后产生多少读写请求；目前没有完成 Accel-Sim 全部周期级机制，也没有取得本配置相对真实 NCU 的模型精度结果。

原 `run.py --mode direct/cosim/cosim-fast` 的历史 PAPER_ADA 结果及含义保留。新增功能入口是 `ada_profile.py`；不能将本页的 sector 行为直接归给所有旧入口。

### 配置来自哪里

XMU 原工作树为 `/home/xmu/nvidiagds/simulators/accelsim2.0`，采用：

| 项目 | 固定版本或文件身份 |
|---|---|
| Accel-Sim framework commit | `d930ad6d02c09bb56867132583735aba0389cff4` |
| GPGPU-Sim commit | `91880c53383d5a6a6742bfb1be2c5f34e39c7871` |
| `gpgpusim.config` SHA-256 | `e09511b9d168632e975422b19a9557ff5ed4482ace160fa428f9c70455f5a891` |
| `trace.config` SHA-256 | `c4e4d8e85e9049af5694afca10b031fae4eafcdd06f0dbd3d8042d7195db890b` |

原文件逐字保存在 [source/gpgpusim.config](../configs/rtx4000-ada-accelsim-v1/source/gpgpusim.config) 和 [source/trace.config](../configs/rtx4000-ada-accelsim-v1/source/trace.config)。[profile.json](../configs/rtx4000-ada-accelsim-v1/profile.json) 保存解释后的字段和全部原始选项；[导入器](../tools/import_ada_tuner.py) 固定检查这两个 SHA，修改原配置需要建立并验证新的 profile。

本实现对照了同次取回的 XMU `gpu-cache.h/.cc`、`shader.h/.cc`、`addrdec.cc` 和 `gpu-sim.cc`。本地逐文件身份保存在仓库外的 `../reference/manifest.json`。关键位置为：缓存字符串解析 `gpu-cache.h:581`；adaptive 分配 `shader.cc:4407–4517`；lazy store `gpu-cache.cc:1747–1811`；sector 状态和 dirty 大小 `gpu-cache.h:344–489`；地址拆分 `addrdec.cc:79–128,225–227,459–518`。这些是固定版本的行号。

公开上游可参阅 [缓存解析](https://github.com/accel-sim/gpgpu-sim_distribution/blob/dev/src/gpgpu-sim/gpu-cache.h)、[缓存行为](https://github.com/accel-sim/gpgpu-sim_distribution/blob/dev/src/gpgpu-sim/gpu-cache.cc)、[adaptive L1](https://github.com/accel-sim/gpgpu-sim_distribution/blob/dev/src/gpgpu-sim/shader.cc) 和 [地址映射](https://github.com/accel-sim/gpgpu-sim_distribution/blob/dev/src/gpgpu-sim/addrdec.cc)。公开 `dev` 会变化，本地固定源码优先。Tuner 本身对部分调度、交错和 hash 参数进行搜索，输出是一套模拟配置，不能据此证明 NVIDIA 未公开的真实设计。[官方 tuner 说明](https://github.com/accel-sim/accel-sim-framework/blob/dev/util/tuner/README.md)

### 结构和容量

| 对象 | 本配置 | 实际含义 |
|---|---|---|
| SM | 48 | `48 clusters × 1 core/cluster` |
| 每 SM scheduler | 4，GTO | 计算调度分区，共 192 个；不是内存分区 |
| 每 SM 资源上限 | 1,536 threads、24 CTA、65,536 个 32-bit registers | 用于当前 kernel 的驻留 CTA 计算 |
| 内存 channel | 10 | 每 channel 两个 memory subpartition |
| L2 | 20 subpartitions × 1,024 sets × 16 ways × 128 B | 合计 40 MiB，每个 slice 2 MiB |
| L1 | 4 sets、128 B line、每 line 四个 32 B sector | ways 随共享内存分配变化 |
| 时钟参考 | core/interconnect/L2：2,175 MHz；DRAM：4,500.5 MHz | 导入参数，不表示功能回放已执行这些时钟域 |

L1 原字符串为 `S:4:128:64,L:T:m:L:L,A:384:48,16:0,32`；L2 为 `S:1024:128:16,L:B:m:L:X,A:192:4,32:0,32`。最后的 `32` 是缓存数据端口每周期字节数；`S` 才表示 sector cache，line 仍为 128 B。

#### L1 为什么不是固定 32 KiB

`4 × 128 × 64 = 32 KiB` 是字符串的初始容量。配置启用 adaptive cache，统一 L1/shared 容量为 128 KiB；因此最大 ways 为 256。当前 kernel 先按 thread、register、shared 和 CTA 上限计算驻留 CTA 数，再结合 grid 是否足够分配到 48 个 SM，计算需要的共享内存总量。

- CTA threads 向上补齐至 32；每 thread registers 向上补齐至 4。
- 驻留数取各资源上限的最小值，并受 `ceil(grid_ctas / 48)` 限制。
- 所需 shared = `shared_bytes_per_cta × resident_ctas_per_sm`，不是只看一个 CTA。
- 取能容纳所需 shared 的最小档位；其余容量给 L1。每个档位仍为四个 sets。

| Shared 档位（KiB） | 0 | 8 | 16 | 32 | 64 | 100 |
|---|---:|---:|---:|---:|---:|---:|
| L1（KiB/SM） | 128 | 120 | 112 | 96 | 64 | 28 |
| L1 ways | 256 | 240 | 224 | 192 | 128 | 56 |

实现为 [AdaTunerProfile::allocate](../source/work/tilegen-full-r1/core-native-copy-r2/include/ada_tuner_profile.h)。必须提供实际 launch 的线程、寄存器、shared 和 grid 数；未知值不能用 `0` 冒充。这里合法的 `0` 表示来源明确为零。

### 访存和地址规则

功能缓存入口已实现以下规则：

- **L1：write-through + allocate-on-miss + lazy-fetch-on-read。** Store hit/miss 都向 L2 发送写，同时保留本地 sector 状态和写入字节覆盖。部分写不会立即触发 RFO；多次写覆盖完整 32 B 后可直接供后续读取。未完整可读的 sector 遇到 read 才取完整 32 B。WT 数据已向下传播，因此 L1 淘汰不重复发写回。
- **L2：write-back + allocate-on-miss + lazy-fetch-on-read。** Store 分配并标 dirty，不立即读 DRAM；后续读取尚不完整的 sector 触发 32 B merge read，保留 dirty 状态。128 B line 被淘汰时，只为其中 dirty 的 sector 发出各一个 32 B 写回。
- **LRU 以 line 为替换对象，valid/readable/dirty 以 sector 区分。** 命中同一 line 的另一个 sector，不代表该 sector 已有效。
- **L1 write ratio 25 是 dirty line 的替换资格条件。** 它不是“25% 带宽用于写”或主动压力写回。无合格 victim 时，当前功能模型采用明确计数的 forward/no-allocate 近似；结果中的 `L1_unmodeled_reservation_stalls` 和 `reservation_approximation_observed` 会指出是否发生，不能将其宣称为原模拟器的逐周期重试。
- 每个 kernel 边界失效 L1，L2 跨 kernel 保持。结束时不强行 flush dirty L2。容量重新分配也只失效 L1，不清空 L2。

源字节地址 `a` 的 channel、slice 和 L2 set 采用固定源码的非二次幂 gap mapping：

```text
channel = (a >> 8) % 10
memory_subpartition = channel * 2 + ((a >> 7) & 1)
partition_address = (((a >> 8) / 10) << 7) | (a & 127)
L2_set = ((partition_address >> 7) ^ (partition_address >> 17)) & 1023
```

这是先除去 channel/subpartition 路由，再做 partition-local XOR；不同于历史 PAPER_ADA 的 `(a >> 8) % 20`。实现为 [ada_address_mapping.h](../source/work/tilegen-full-r1/core-native-copy-r2/include/ada_address_mapping.h)。请求中的 `memory_subpartition` 是真实的这套模拟内存编号 0–19，与 `sm` 及每 SM 的四个 scheduler 分开。

### 运行功能回放

要求 Python 3、可用 C++20 编译器；默认使用 `build-config.json` 中的 clang++。这些命令只运行 CPU，不采集 GPU 或 NCU。

`ada_profile.py` 和 `tests/run_unit_tests.py` 可使用 `--compiler g++` 显式选择已安装的 C++20 工具链，实际编译器路径记录在收据中。XMU 默认 clang14 误选不完整的 GCC12 安装，无法找到 `cstdint`；已确认完整 GCC11 标准库存在，远端验证选择 g++11，不改变架构参数。

从仓库根目录运行，输出目录必须尚不存在：

```sh
python3 tools/import_ada_tuner.py --check
python3 ada_profile.py --profile tuner-v1 \
  --input tests/fixtures/ada-sector.jsonl \
  --output build/ada-example-r1 \
  --emit-trace
```

Wrapper 编译 [ada_cache_replay.cpp](../source/ada_cache_replay.cpp)，执行回放，并保存：

| 输出 | 含义 |
|---|---|
| `result.json` | 每 kernel 的 source requested bytes、cache 后 DRAM 请求字节、L1 容量、dirty 守恒及 20 个内存分区计数 |
| `receipt.json` | 输入、profile、源码和二进制 SHA；子进程返回值；运行 CPU/wall 分钟；编译另列 |
| `postcache.requests.jsonl` | 使用 `--emit-trace` 时输出逐个 32 B 读/写回请求 |
| `compile.stderr`、`run.stderr` | 编译和执行诊断 |

`receipt.status=PASS_PROCESS_ONLY` 仅表示进程和前后来源检查通过；缓存模型结果另以 `COMPLETED_FUNCTIONAL_ONLY` 标记。CPU 时间是运行子进程 user+system 分钟，编译不包含在该运行 CPU 数字内。

输入 JSONL 包含 kernel 资源记录和显式地址范围；以下是最小格式示意：

```jsonl
{"type":"kernel","name":"example","threads_per_cta":64,"registers_per_thread":32,"shared_bytes_per_cta":0,"grid_ctas":96}
{"type":"memory","allocation_id":0,"cta":0,"warp":0,"sm":0,"pc":4096,"op":"write","ranges":[{"lane":0,"address":0,"bytes":4}]}
{"type":"memory","allocation_id":0,"cta":0,"warp":0,"sm":0,"pc":4112,"op":"read","ranges":[{"lane":0,"address":0,"bytes":4}]}
```

`address` 是送入缓存的字节地址；`allocation_id` 保留来源命名空间，不能用不同 ID 掩盖实际别名。`bypass_l1` 可按来源提供。一个 memory 记录作为一个独立 subop 执行，保留 range 请求字节的重复计数；缓存覆盖按实际地址合并。输入任意 JSONL 不会自动取得 kernel-native 的来源资格。

有 `sm` 时使用显式值；缺失时采用已声明的 `cta % 48` 功能映射，不声称这是硬件实测的 CTA 分配。回放执行输入记录的顺序，不重新运行 GPU scheduler。初始缓存为冷缓存，未执行 host memcpy/memset 对缓存前史的影响，也未执行 kernel 内 membar 的失效行为。

### 已验证结果及两个入口的边界

CPU 小夹具 [ada-sector.jsonl](../tests/fixtures/ada-sector.jsonl) 已完整闭合：2 个 kernel、42 条 source memory 记录，L1 从 128 KiB 调整到 28 KiB，L2 保持。Source requested read/write 为 160 B / 32 B；缓存后 DRAM read/write 为 **1,152 B / 32 B**，即 36 个读请求和 1 个写回请求。Dirty sector 创建 1、淘汰 1、结尾驻留 0；没有通过末尾 flush 补数。证据为 [result.json](../validation/ada-accelsim-r1/local-fixture-result.json) 和 [receipt.json](../validation/ada-accelsim-r1/local-fixture-receipt.json)。这只是功能夹具，不是 LLM 或真实硬件的精度/速度基准。

独立测试还覆盖 partial store→read→evict、跨 sector、已知字节累积、稀疏 dirty 写回、读请求与 victim 写回顺序、旧 direct 路径回归和 kernel 资源边界。可重新执行测试 runner；请使用新的输出目录，以其实际收据判断整批状态：

```sh
python3 tests/run_unit_tests.py --output build/ada-unit-check-r1
python3 tests/run_unit_tests.py \
  --output build/ada-unit-check-asan-r1 \
  --sanitize address,undefined
```

| 入口 | 本次实现 | 不能据此声称 |
|---|---|---|
| `ada_profile.py` / `ada_cache_replay` | 实际 sector/known-byte/lazy-write 功能缓存、adaptive L1、40 MiB L2、新映射、逐内存分区流量 | GPU 计算、warp stall、HBFSIM 或周期级带宽已执行 |
| [ada_gtsim_structure.h](../source/ada_gtsim_structure.h) | 将 SM 数、每 SM 四个 scheduler 的现有结构、GTO、当前 kernel 驻留 CTA 限制、时钟参考和 L2 几何接入 GTSim 配置 | 等价于上面的 sector 功能路径或完整 Accel-Sim 时序 |

结构适配器明确命名为 `gtsim-ada-accelsim-structure-only-v1`。它仍继承旧 fine 路径的 L1 store-bypass/whole-line 有效性、128 B fill/RFO、全局排队以及旧服务参数，包括 272/604 cycles 和约 311.66 GB/s。L2 的 **每 subpartition 192 MSHR、每项 merge 4**，L1 的 384/48、miss queue、端口、寄存器仲裁和互连等虽然已导入配置，尚未作为与 Accel-Sim 等价的周期机制执行。不能把“字段存在”写成“机制已实现”。

新输出格式是 `GTSIM_ADA_POSTCACHE_SECTOR_V1` JSONL，含 32 B read；它**不是**既有 HBFSIM replay 的 `TGCSIM01` legacy trace 格式，不能直接将其交给原 `replay.py` 后声称已经运行 cosimulation。后续需显式适配和验证。当前报告将 simulated latency、bandwidth 留空，并明确 `compute_executed=false`、`HBFSIM_executed=false`、`NCU_accuracy_tested=false`。

本轮未启动大模型测试。独立 XMU 交付目录约定为 `/home/xmu/nvidiagds/codex-runs/gtsim-ada-accelsim-20260921-r1`；部署状态须以交付报告和 XMU 实际收据为准。

本地正式回归为 15 个测试全部通过，端到端检查 139 项通过；归档证据位于 [local-unit-summary.json](../validation/ada-accelsim-r1/local-unit-summary.json) 和 [local-e2e-receipt.json](../validation/ada-accelsim-r1/local-e2e-receipt.json)。端到端测试命令（先用上述 wrapper 编译得到二进制）：

```sh
python3 tests/ada_profile_e2e_test.py \
  --binary build/ada-example-r1/ada_cache_replay \
  --output build/ada-e2e-check-r1
```

## RTX 4000 Ada：接入 r2 校准基线与 r3 实验配置


本次默认入口更新为 `r2-adaptive`。它对应 XMU r2 的 `01_measured_adaptive`，仍属于**部分验证的默认候选**；不是已全面匹配真实硬件的配置。r3 已完成实验，但其交付明确不提升默认，因此仅提供显式实验选项。原 tuner-v1 保留，旧 r1 工作树和历史结果未覆盖。

### 来源和实际改动

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

### 可执行配置选择

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

### 运行与复现

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

### 参考时延的实际消费范围

新增 `make_rtx4000_ada_calibrated_internal_config` 将34/239/324这些参考阶段常量映射为GTSim内置模型的L2命中273 cycles、cold miss端到端597 cycles；实际完成路径的测试验证不重复叠加命中延迟，普通step与service_epoch一致。这里是core-cycle内部参考模型，不是Accel-Sim周期等价或HBFSIM校准。旧带宽、队列以及fine的whole-line/store-bypass/RFO机制保留，详见 [内部时延合同及限制](ada-calibration.md)。

32B dirty模式要求有限backend credit；测试用64-credit薄适配器包住真实DRAMModel，未解除旧生产入口对自动内部backend的限制。功能traffic入口本身仍不执行时钟。

本地正式验证：18个C++测试程序全通过；显式v1端到端139项通过；新CLI端到端30次调用、192项检查通过。收据位于 [validation/ada-calibrated-r2](../validation/ada-calibrated-r2)。

### 如何理解校准效果

r2：66条件中，**按NCU实际shared划分选择01/02/03**后，L2全局读请求sector WAPE由68.38%降到15.78%；这不是单一默认01的成绩，默认划分22条件下01的对应WAPE仍为29.60%。实验工作集小于40 MiB L2，主要验证L1过滤和冷读唯一sector计数。不能把这些数字解释为DRAM字节误差、L2写回精度或LLM误差。

r3：冻结FIFO候选在240个留出条件上的WAPE为21.84%，r2参考为23.54%；但是32 KiB默认共享划分子集由20.97%退化到25.39%，所以未提升默认。静态FIFO的选择存在训练并列，不能据此断言真实GPU采用FIFO。

MemGen另有r3 LRU64sets候选和300条件回放；它的配置身份、映射和证据快照与本次Accel-Sim r3 FIFO交付不同，不能仅因都叫r3就当成统一配置。本轮没有改变MemGen生产分支。

本接入不宣称已经消除decode write gap。完整LLM验收仍需要匹配kernel输入、地址/alias、cache前史、shared划分和NCU采样窗口，再比较分阶段read/write，不能用上面的L1读微基准替代。

## r2内部参考时延：单位、接入与边界


新增入口为 `make_rtx4000_ada_calibrated_internal_config(resources, profile=R2_ADAPTIVE, optional observed_shared_carveout_bytes)`，定义在 `source/ada_gtsim_structure.h`。原 `make_rtx4000_ada_accelsim_structure_config` 保持原行为；新入口选择tuner-v1也返回旧factory语义。

冻结r2契约给出L1基础34、L2附加239、DRAM附加324。取整后的名义加和为L2总273、冷miss总597。不得将L2实测总延迟272.68445/整数273填到附加239的位置，也不得把DRAM实测总延迟596.228当成附加324。两组时钟配置中core/L2同为2175 MHz；本适配器实际消费的单位是GTSim **core Cycle**，不是DRAM时钟周期或ns。

### 实际生效字段

| 字段 | 旧factory | 新calibrated-internal参考 | 执行位置 |
|---|---:|---:|---|
| `per_sm_l1.hit_latency_cycles` | 34 | 34 | L1命中后从当前core Cycle起计 |
| `l2_hit_latency_cycles` | 272 | **273 = 34+239** | L2命中决策后起计；fine L1 miss没有先等待34cycle |
| `memory_model_semantics.l2_miss_latency` | END_TO_END_FROM_MISS_DECISION | 同左 | 内部fill完成时直接唤醒，不再追加L2 hit延迟 |
| `end_to_end_miss_latency_cycles` | 604 | **597 = 34+239+324** | 实际DRAMModel读miss ready time = issue_cycle +597 |
| `l2_miss_penalty_cycles` | 604 | 597 | 内部DRAM构造参数；end-to-end读miss由上一行控制 |

因此新入口实际执行273/597，并非只把239写进JSON。这里将分级常量压到原fine模型的决策至完成区间，是明确的近似适配；它没有重建Accel-Sim流水级。服务credit不足仍可能延后完成，597不是有竞争时保证的端到端时长。

旧A3B4默认已经采用end-to-end语义。只有另一种 `LEGACY_ADDITIVE_DRAM_THEN_L2_HIT` 才在DRAM完成后再加L2 hit延迟。新入口明确保留end-to-end，防止误得597+273。DRAMModel的ready time加法使用core Cycle；带宽的core/DRAM频率换算不自动换算此延迟。

### 保留内容与使用限制

新入口用校准profile选择L1容量、ways及LRU/FIFO，并复用原资源分配与新Ada几何。fine仍为whole-line有效性、L1 store bypass、write-miss RFO和原fill时分配；没有自动获得direct的sector/lazy语义。旧带宽rate、队列容量、排队顺序及MSHR实现也保持。

这是 **internal-reference** 接口，未接入外部HBFSIM。外部backend已提供自己的完成时间，不能将597或273盲目再叠加其服务时间，也不能只改此factory就声称HBFSIM时延已校准。functional direct replay仍然不执行时钟，它报告的参考延迟保持 `executed=false`。

`TILEGEN_DIRTY_SECTOR_MODE=2`要求backend声明有限admission，而现有内部DRAMModel接口没有此上限。此轮测试使用容量64的薄wrapper，只管理credit并原样转发真实DRAMModel的时延、带宽和完成算法；没有新增生产backend或解除原构造约束。普通默认Simulator构造在mode2下仍不能凭本factory绕过有限backend合同。

### CPU验证

`tests/ada_calibrated_timing_test.cpp` 通过实际L2Cache + DRAMModel验证：

- 冷fill在issue之后597cycle完成，node在同一cycle完成，未追加273。
- 同地址再次读取且绕过L1时，L2 hit在决策后273cycle完成，无第二个DRAM请求。
- 普通step和span1 `service_epoch` 的相对时延一致。
- 请求与完成字节闭合，仍为128B read fill；未修改缓存traffic规则。
- v1仍为272/604，r2/r3资源配置与静态24 KiB/FIFO可进入fine结构配置；无匹配observed时拒绝固定profile。

O2测试31项通过，成功标记 `PASS_ADA_CALIBRATED_TIMING`。这些是代码单位与执行行为检查，不是硬件时序或LLM精度验收。

### 来源

- [r2延迟原始契约](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/reference/r2/configs/parameter_contract.json)
- [新旧factory](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/source/ada_gtsim_structure.h)
- [A3B4实际语义](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/model_semantics.h:198)
- [真实DRAMModel ready time](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:972)
- [fine miss完成分支](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:1686)
- [时延测试](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/tests/ada_calibrated_timing_test.cpp)

