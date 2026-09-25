# TileGen 原生 trace / HBFSIM 融合分支

本分支在独立源码树中重建 TileGen 的原生访存路径，并把同一套地址流接入 HBFSIM
cosimulation、纯访存回放与 Ada 缓存档。三条路径共用地址规则，**过 cache 流量不保证
逐条相同**。

当前新增 **r4 串行读缓存候选**：`ada_r4.py` 使用共享 GTSim L1 回放冻结请求，比较
r2 LRU 与 r4 CLOCK/hash2。它只验证 L2 读入口 sector；r2 默认与旧 direct/cosim 配置
保持原样。见 [r4 参数、运行及验证边界](docs/ada-r4-serial.md)。

分支：`codex/tilegen-trace-cosim-20260918-r1`。B8 合并暂停，本分支固定 B1。

当前缓存结构档为 `TILEGEN_PAPER_ADA_GEOMETRY_R1`：三个入口共用 L1 **32 KiB/SM、
64 ways、4 sets、128 B line、store bypass、kernel-flush**；L2 **40 MiB、20 logical
slices ×1024 sets ×16 ways、128 B line、32 B dirty**，使用封存 PAPER_ADA 的
quotient/XOR 索引和组内 LRU。完整配置写入每次结果的 `cache_configuration`。这统一
结构和生命周期，尚未统一 MemGen 的 sector/known-byte 有效性和 lazy write
allocation；TileGen 仍为 128 B fill/RFO，写回每请求 32 B。此档**不表示硬件精度已
校准**。历史报告的 64 KiB/全相联结果保持原样。

以 C 最新原生运行时为唯一源码基线，复用 A 正式版与 C 同源的原生访存规则。同一套
Model、Prepared、binding / Builder 提供快速缓存后地址流和 HBFSIM cosimulation。
所有写回请求均为 **32 B**；读填充及 store RFO 仍为 **128 B**。

> 按时间顺序累积的阶段性记录（"最新衔接…""最新增加…"等）已移入
> [HISTORY.md](HISTORY.md)。它们描述当时状态，不作为当前验收依据。

## 目录速览

| 目录 | 内容 |
|---|---|
| `source/` | 独立源码基线：原生运行时、direct 路径、HBF 后端与封存配置 |
| `llm/` | LLM 侧 executor、fast-prefill / fast-prefill-sweep 契约与 warmup 工具 |
| `configs/` | Ada 各档 profile（tuner-v1、r2-adaptive、r2-shared*、r3-fifo-*、r4-serial） |
| `capture/` | 硬件侧采集包：observer、host argument capture、P1024D32 |
| `tests/` | CPU 单元测试、端到端脚本与夹具 |
| `tools/` | 导入器、评估器、报告生成器 |
| `validation/` | 验收收据（JSON）；判等基准见下表 |
| `provenance/` | 来源映射、依赖扫描与导入记录 |
| `native_transfer/` | 模板/回归输入的解码与冻结 |
| `archive/` | GDDR P2 隔离重跑包：代码、构建、运行与冒烟测试见其 [README](archive/hbserve-gddr-p2-rerun-20260922-r1/README.md) |
| `docs/` | 设计说明，索引见 [docs/README.md](docs/README.md) |

复跑对账的判等基准：`archive/hbserve-gddr-p2-rerun-20260922-r1/receipts/acceptance-{prefill,decode}-sweep.json`。

## 运行档

| 参数 | 执行方式 | 时序范围 |
|---|---|---|
| `replay.py` | 已有 direct cache 后 trace → HBFSIM；不重新建模/cache | 全部请求就绪，按原序尽快填充有限内存队列；无计算/依赖/GPU stall |
| `replay.py --mode stage-overlap` | direct `--phase-ctas 48` 导出的 trace + 静态计算 profile → HBFSIM | 阶段访存完成后计算，下一组可预取；跨 kernel 屏障，固定 direct cache，无 warp 依赖执行 |
| `--mode direct` | 原生地址；9 类免逐 CTA DAG，11 类单 CTA 投影；功能 L1/L2 | 固定 call / CTA / member 顺序；立即完成；不运行计算调度或 HBFSIM，时间字段未知 |
| `--mode cosim`（默认） | 原生计算/依赖图、已有精确主机加速、GTSim L1/L2、HBFSIM | 保留计算、访存停顿及重叠，有限队列/准入 |
| `--mode cosim-fast` | C 原 hybrid 路径与 fine fallback | **显式近似时序档**：q16、memory-phase16、epoch8、independent drain；不宣称与全 fine 周期相同 |

direct 与 cosim 共用地址规则，**过 cache 流量不保证逐条相同**：跳过计算和在途请求会改变跨 warp/CTA 顺序、MSHR 合并及 LRU。需要实际调度顺序的地址流时使用 `cosim --trace`。
硬件时序尚未校准；原有 estimated-address、依赖资格等限制保留。后端和阶段模型没有使用 NCU 流量或带宽拟合系数；P1024/D32 新采集的执行状态以独立收据为准。

三种 replay 档的差别与共用边界见 [replay 档说明](docs/replay-modes.md)。

## 构建与使用

要求 C++20、zlib、Python 3。默认直接 clang 构建，无须 CMake。

```sh
python3 build.py --output build/cache-alignment-r1 --jobs 2 --native --thin-lto
python3 run.py --input /absolute/path/workload.input --output build/direct-run --mode direct
python3 run.py --input /absolute/path/workload.input --output build/cosim-run --mode cosim --trace
python3 run.py --input /absolute/path/workload.input --output build/fast-run --mode cosim-fast
```

输出目录必须新建；已有二进制可以直接运行。`--binary` 可指定其他构建。
`run.py` 给出子进程真实 CPU 和 elapsed 的秒数、分钟数；编译、输入准备另计，结果核验用时另列。
零退出码还须通过结果身份和 trace SHA 核验才算成功。`build.py` 核对构建前后源码 SHA 不变。
另提供 CMake/CTest 配置；本次本机采用 clang 构建，没有执行本机 CTest。

固定输入为 B1、Llama3-8B BF16、P32/D2 的原生 1138-call 模型。
**源码/Git 独立，运行数据尚未搬迁封包**：transport 含八个压缩模板，模型仍按原 pin 读取本机封存的工作流、程序和配置。不是任意模型、shape 或 batch 的入口。
源码编译不再依赖原目录；运行输入仍为只读封存数据。

完整运行需完整输入和 `--full-workflow`。wrapper 默认 trace 配额 8 GiB；旧 full cosim 流量按当前 144 B 记录估算约 **48.12 GiB**，仅用于容量预算，不能当成本分支完整运行实测。
full 导出需显式设置 `--max-trace-bytes 68719476736`（64 GiB 上限）并准备空间。
超配额会失败并保留 `.partial`，只有读回校验完成才发布正式文件。两种模式都不做最终 dirty flush。

分档 smoke 测试（从零到完整回放）见 [SMOKE.md](SMOKE.md)。

## 复跑入口

```sh
python3 tests/run_unit_tests.py --output build/unit-tests
python3 tests/run_unit_tests.py --output build/unit-tests-asan --sanitize address,undefined
python3 tests/prepare_inputs.py --source-runtime /absolute/path/canonical-full-runtime-r4 --output build/validation-inputs
python3 tests/run_direct_projection.py --build build/shared-frontend-r2 --input build/validation-inputs/families.input --output build/projection
python3 tests/run_direct_projection.py --test prepared-memory --build build/shared-frontend-r2 --input build/validation-inputs/decode-512.input --output build/prepared-memory-check
```

准备脚本复用原封存的 CPU lowering，记录准备时间和输入 SHA，不进行 GPU 采样。
该准备器沿用上游 Darwin 内存监控，数据准备仍依赖当前本机封存目录。

## 当前状态与剩余工作

新目标为当前 SGLang native 的 **B1 / BF16 / 32 层 / P1024 / D32**，同源硬件采集包见 [采集说明](capture/p1024d32/README.md)。33 阶段输入与自然 CUDA event 已采集；正式 NCU 已完成 3 组 × 6 范围（Full、Prefill、D1、D8、D16、D32），保存原始报告、CSV 和来源校验记录。不同范围来自独立运行，不能用 Full−Prefill 推导 Decode 流量；NCU 时间与自然运行时间分别报告。

输入采集、NCU 测量与新形状 TileGen 地址模型的资格检查是不同步骤；**完整 P1024/D32 TileGen 尚未准入**，新 attention/GEMM/merge kernel 的参数、原生动态证据、地址绑定及阶段/容量适配见 [P1024D32 准入与绑定](docs/native-p1024d32.md)。状态与正式 NCU 汇总见 [进度记录](validation/native-p1024d32-progress.json)，结果文档见 [阶段后端与新负载报告](/Users/wgs/Documents/Codex/2026-09-17/zhi/outputs/tilegen-stage-backends-20260918/index.html)。

## 独立分支与实现

- 基线提交 `0e21251f126510744d1b319f043e7e6b2dae5e1f`：抽取 22 个翻译单元、171 个实际源码依赖（约 4.19 MB），展开原 VFS overlay。原工作目录未修改，B8 候选仍在另一个仓库。
- `provenance/source-map.json` 逐文件记录逻辑路径、实际来源、SHA 和 include 改写。上游以封存文件组织，所以这是有来源记录的独立导入及集成提交，没有伪造 Git 合并祖先。
- `source/direct_native.h`：9 类复用原 native binding，其余 11 类通过原 Builder 按一个 CTA 提取访存并释放，没有新增模型地址公式。
- `source/direct_cache.h`：复用原 L1、dirty-mask 规则，使用立即完成的功能 L2 LRU。两种模式共用 `source/native_trace.h`。
- 原生 L2 把连续 dirty run 改为逐个 32 B 请求，有限待准入队列上界改为 `4(B+1)`。后端只增加可选观察器，成功准入后记录一次，重试不重记。
- 有界子集只构建实际选中 family 的 typed 预验证模型；八帧仍全部解压并验证声明的 SHA。预验证与执行模型分开持有，避免 full-grid 模板被 prefix-CTA 执行错误复用。完整 1138 流程仍包含全部 family，不能把这项子集收益外推给完整流程。

## XMU Accel-Sim Ada 配置入口

`ada_profile.py` 提供独立的 adaptive L1 / 32 B sector / lazy-write 功能缓存回放；配置来源、运行示例及与 structure-only GTSim 适配器的区别见 [Ada 配置与内部参考时延](docs/ada-calibration.md)。该入口不表示旧 cosim 已完整实现 Accel-Sim 时序，也未完成本配置的 NCU 精度验收。

当前默认 profile 为 `r2-adaptive`，保留 `--profile tuner-v1`；r3 FIFO 静态配置仅显式实验选择。原 `run.py` direct/cosim 默认未变。fine 侧尚未实现的部分见 [Ada fine 时序缺口](docs/ada-fine-timing-gaps.md)。

## 复现状态

GDDR P2 隔离重跑包已归档在 [archive/](archive/hbserve-gddr-p2-rerun-20260922-r1/README.md)，
其 sweep 数值已在独立机器上重新跑通并与 `acceptance-*.json` 逐格对齐；判定见
[receipts/README.md](archive/hbserve-gddr-p2-rerun-20260922-r1/receipts/README.md)
与 [SMOKE.md](SMOKE.md)。
