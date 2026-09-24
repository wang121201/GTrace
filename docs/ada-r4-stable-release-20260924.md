# Ada r4 串行 CLOCK 稳定版（2026-09-24）

本页记录 `stable/ada-r4-serial-clock` 分支的来源、稳定化范围、等价性证据与复现步骤。
稳定版内容与原始候选提交等价，但基线前移到了 r2 已修复的尖端。

## 1. 本分支是什么

Ada r4 把冻结的 `CLOCK_u128_s16_h2_c1062` 接入 GTSim 共享 `PerSmL1Cache`，用于比较 L1 过滤后进入 L2 的 32 B 读 sector 数量。设计说明见
[ada-r4-serial.md](ada-r4-serial.md) 与 [ada-r4-serial-replay.md](ada-r4-serial-replay.md)。

| 项目 | 原始候选 | 本稳定分支 |
|---|---|---|
| 提交 | `35c1367` | `bcdd0ba` |
| 基线 | `7533322` | `e412508` |
| namespace 修复 `5211abc` | **缺失** | 含 |
| 源 SHA 重封存 `e412508` | **缺失** | 含 |
| 副本数 | 1（仅一个克隆，未进入任何受跟踪仓库） | 2（本仓库 + `import/ada-r4-original-35c1367`） |

原始提交 `35c1367` 全机器仅存在一份，且父仓库中不存在该对象。为消除单点丢失风险，它已原样取入本仓库并保留为分支
`import/ada-r4-original-35c1367`；稳定版则是把它移植到当前尖端的结果。

## 2. 等价性证据

稳定化**改变了基线、未改变行为**。同机同输入下逐字段比对 `predictions.jsonl`：

| 检查项 | 结果 |
|---|---|
| 输出行数 | 768 = 384 cases × 2 models，两侧一致 |
| 确定性字段差异（23 个字段） | **0 处** |
| hits | 两侧均为 3,180,305 |
| misses | 两侧均为 5,293,295 |
| requests | 两侧均为 8,473,600 |
| L2_read_sectors | 两侧均为 5,293,295 |
| `compiled-profiles.json` | 逐字节相同 |
| driver `--self-test` | 两侧均 `PASS_ADA_R4_SHARED_SERIAL_SELF_TEST`，44 项 |

唯一差异在 `CPU_seconds` / `wall_seconds`，属宿主计时噪声。整个流程（编译 + `--describe` + `--self-test` + 384 cases 回放）耗时约 7.6 s wall。

因此 `predictions.jsonl` 的整文件 SHA 会因计时字段而每次不同；**验收必须比对计数与 SHA 以外的派生量，不能只比整文件 SHA**。
原始运行的整文件 SHA 为 `b5f61ef16399ae517cb70785f5dcba716b5f44f66b4975e2930902460ace5dd7`（对应其自身一次计时），仅作记录。

## 3. 血缘更正：本仓库是 GTSim 派生，不是 Accel-Sim 派生

本仓库目录名含 `accelsim`，且 `configs/rtx4000-ada-accelsim-v1/`、`docs/ada-accelsim-profile.md` 引用 Accel-Sim。
**但它不是 Accel-Sim 的衍生仓库。** 核查如下：

| 检查项 | 结果 |
|---|---|
| `simulator/` `gpu-simulator/` `util/` `sass_ram/` | 全部不存在 |
| `build.py` / `CMakeLists.txt` / `build-config.json` / `replay-build-config.json` 中 `accelsim` | 命中 **0 次** |
| `namespace GTSim` | **48** 个文件 |
| 提及 `GTSim` | **172** 个文件 |

派生依据：

- `source/work/tilegen-full-r1/core-native-copy-r2/include/tma.h` 注释逐字写
  "Compatibility include for historical GTSim code"，并提供 `namespace GTSim` 下的 `TMARequest`/`TMAUnit` 别名。
- 11 个头文件与 GTSim `include/` 同名共有，且均为**增量扩展**：
  `dag_node.h gpu.h memory.h noc.h pipeline.h scheduler.h simulator.h sm.h subpartition.h tma.h tmem.h`。
  例：`memory.h` 1,423 行 → 2,571 行；`simulator.h` 564 → 863；`sm.h` 146 → 152。
- `build-config.json` 的编译单元与 GTSim `src/` 同名：`gpu.cpp` `scheduler.cpp` `simulator.cpp` `sm.cpp` `subpartition.cpp`。

Accel-Sim 只作**只读参考基准**：`provenance/accelsim-ada-reference.json` 以固定版本
（framework `d930ad6d02c09bb56867132583735aba0389cff4`、GPGPU-Sim `91880c53383d5a6a6742bfb1be2c5f34e39c7871`）
逐文件 pin 住 `gpu-cache.h/.cc`、`shader.h/.cc`、`addrdec.cc`、`gpgpusim.config`、`trace.config`；
`docs/ada-accelsim-profile.md` 以固定行号引用（`shader.cc:4407–4517`、`gpu-cache.h:344–489`、`addrdec.cc:79–128`）。
该页同时声明本实现**未完成** Accel-Sim 的周期级机制。

本分支的构建与运行**不需要** Accel-Sim。

## 4. 复现

需要 Python 3 与 C++20 编译器；`ada_r4.py` 只用 `g++` 编译 `source/ada_r4_serial_replay.cpp` 一个翻译单元，
并冻结校验 `configs/rtx4000-ada-r4-serial/source/` 下两个源文件的 SHA：

- `candidate_parameters.json` = `5f4c729a924a2898afc829379d878b8aefe6496265c92e03412cb6acfbeb59c1`
- `cache_policy_replay_r4.cpp` = `9319661e7abe613cc9bdb81ce488d1b3448aab6e87ad085e394fe92e8db042aa`

```sh
python3 ada_r4.py \
  --input /path/to/evaluation_cases.tsv \
  --output /absolute/new/run-directory \
  --compiler g++
```

输出目录必须不存在。`receipt.json` 成功状态为 `PASS_PROCESS_ONLY`；`predictions.jsonl` 每 case 两条
（`LRU_u128_s4_h0_c1000` 与 `CLOCK_u128_s16_h2_c1062`）。请求数据不随结果复制。

与真实 NCU 比较（本机需 `--hardware` 与 `--frozen` 聚合文件）：

```sh
python3 -B tools/evaluate_ada_r4.py \
  --replay-jsonl REPLAY_JSONL \
  --replay-receipt WRAPPER_RECEIPT_JSON \
  --hardware HARDWARE_AGGREGATES_JSON \
  --frozen FROZEN_AGGREGATES_JSON \
  --output FRESH_OUTPUT_DIRECTORY
```

## 5. 冻结结果（384 条件）

`status = COMPLETED_SERIAL_ONLY_NOT_DEPLOYED`；384 条件 = 360 新 CA + 12 CG 控制 + 12 跨时段锚点；NCU kernel 1,920，native check 1,920。

| 模型 | fresh CA WAPE | ≤2% 或 2 sector | 最大绝对误差 | `.cg` 控制 |
|---|---:|---:|---:|---:|
| **r4 `CLOCK_u128_s16_h2_c1062`** | **12.013 %** | 98 / 360 | 5,416 | 0.0 %（12/12） |
| r2 基线 `LRU_u128_s4_h0_c1000` | 21.528 % | 78 / 360 | 8,008 | 0.0 %（12/12） |
| `FIFO_u128_s16_h2_c1062` | 11.289 % | 95 / 360 | 5,416 | 0.0 %（12/12） |
| `LRU_u128_s16_h2_c1000` | 14.738 % | 89 / 360 | 4,220 | 0.0 %（12/12） |
| `LRU_u128_s64_h0_c1000`（r3） | 18.141 % | 82 / 360 | 6,962 | 0.0 %（12/12） |
| `FIFO_u128_s64_h0_c1000`（r3） | 17.097 % | 74 / 360 | 6,373 | 0.0 %（12/12） |
| `RANDOM_u128_s64_h1_c937` | 15.163 % | 83 / 360 | 13,228 | 0.0 %（12/12） |
| `SRRIP_u128_s64_h1_c937` | 16.232 % | 88 / 360 | 8,008 | 0.0 %（12/12） |
| `BRRIP_u128_s64_h0_c937` | 30.821 % | 64 / 360 | 13,854 | 0.0 %（12/12） |

r4 相对 r2 基线把 WAPE 从 21.53 % 降到 12.01 %，但「总体与所有子组均 ≤10 %」的联合目标**未通过**。
本轮 288 个 r3 条件已参与开发，不再是独立留出集；未根据新数据重新挑参。

`.cg` 全 12 条件 WAPE 为 0，说明逐请求旁路控制通路自洽。

## 6. 边界

- 只覆盖 L1 过滤后进入 L2 的 32 B **读** sector 数量。未设定 GPU 延迟、L2 写回、SM 调度、MSHR 或 HBFSIM 参数。
- 不包含写、原子、并发语义；单 SM、冷启动、同步 fill、全局标量读。
- 不能推断 DRAM 写误差、LLM decode 写流量、带宽或 compute/memory overlap 已改善。
- `CPU_seconds` / `wall_seconds` 是宿主耗时，不是 GPU 时长或带宽。
- 有效 L1 参数是串行读校准结果，不声明硬件物理组数或容量。
- `ada_profile.py` 默认仍为 `r2-adaptive`；r4 是显式 opt-in 入口，不会被静默应用到旧 LLM 或带宽结果。
- 本稳定化已验证行为等价，但**未**重新评估硬件精度；第 5 节数值来自原始 campaign。

## 7. 产物位置

| 内容 | 位置 |
|---|---|
| 本稳定分支 | `stable/ada-r4-serial-clock` |
| 原始候选提交 | `import/ada-r4-original-35c1367`（`35c1367`） |
| 原始运行产物 | `gtsim-ada-r4-evaluation-20260922-r1/serial-replay/`、`evaluation/`、`unit-tests/` |
| 校准 campaign | `rtx4000-ada-cache-calibration-20260922-r4/`（含 `analysis/evaluation_cases.tsv` 与请求文件） |
| 两份 sweep 报告 | `gtsim-ada-r4-prefill-20260922-r1/`、`gtsim-ada-r4-p128-20260922-r1/` |
