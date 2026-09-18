# TileGen 原生 trace / HBFSIM 融合分支

分支：`codex/tilegen-trace-cosim-20260918-r1`。B8 合并暂停，本分支固定 B1。

以 C 最新原生运行时为唯一源码基线，复用 A 正式版与 C 同源的原生访存规则。
同一套 Model、Prepared、binding / Builder 提供快速缓存后地址流和 HBFSIM cosimulation。
所有写回请求均为 **32 B**；读填充及 store RFO 仍为 **128 B**。

最新衔接：GEMV、SiLU 的 direct binding 与精确 cosim Builder 已共用 cache 前 `PreparedMemory`，并减少等价 CTA 校验的主机分配。三组配对测试中引擎执行窗口为 **1.08×**，完整子进程 CPU 时间基本持平；不能据此声称端到端明显提速。实现、验收和范围见 [共享前端报告](docs/shared-frontend.md)。

最新增加：已有 direct trace 可通过独立 `replay.py` 直接回放到 HBFSIM，跳过计算和 GPU stall，保留内存队列/时序。Decode 有界子集的 591,652 条请求回放 CPU **0.022 分钟**；完整生成与回放合计约 **0.440 分钟**，与保留计算依赖的 cosim 口径不同。见 [纯访存回放说明](docs/memory-only-replay.md)。

## 独立分支与实现

- 基线提交 `0e21251f126510744d1b319f043e7e6b2dae5e1f`：抽取 22 个翻译单元、171 个实际源码依赖（约 4.19 MB），展开原 VFS overlay。原工作目录未修改，B8 候选仍在另一个仓库。
- `provenance/source-map.json` 逐文件记录逻辑路径、实际来源、SHA 和 include 改写。上游以封存文件组织，所以这是有来源记录的独立导入及集成提交，没有伪造 Git 合并祖先。
- `source/direct_native.h`：9 类复用原 native binding，其余 11 类通过原 Builder 按一个 CTA 提取访存并释放，没有新增模型地址公式。
- `source/direct_cache.h`：复用原 L1、dirty-mask 规则，使用立即完成的功能 L2 LRU。两种模式共用 `source/native_trace.h`。
- 原生 L2 把连续 dirty run 改为逐个 32 B 请求，有限待准入队列上界改为 `4(B+1)`。后端只增加可选观察器，成功准入后记录一次，重试不重记。
- 有界子集只构建实际选中 family 的 typed 预验证模型；八帧仍全部解压并验证声明的 SHA。预验证与执行模型分开持有，避免 full-grid 模板被 prefix-CTA 执行错误复用。完整 1138 流程仍包含全部 family，不能把这项子集收益外推给完整流程。

## 运行档

| 参数 | 执行方式 | 时序范围 |
|---|---|---|
| `replay.py` | 已有 direct cache 后 trace → HBFSIM；不重新建模/cache | 全部请求就绪，按原序尽快填充有限内存队列；无计算/依赖/GPU stall |
| `--mode direct` | 原生地址；9 类免逐 CTA DAG，11 类单 CTA 投影；功能 L1/L2 | 固定 call / CTA / member 顺序；立即完成；不运行计算调度或 HBFSIM，时间字段未知 |
| `--mode cosim`（默认） | 原生计算/依赖图、已有精确主机加速、GTSim L1/L2、HBFSIM | 保留计算、访存停顿及重叠，有限队列/准入 |
| `--mode cosim-fast` | C 原 hybrid 路径与 fine fallback | **显式近似时序档**：q16、memory-phase16、epoch8、independent drain；不宣称与全 fine 周期相同 |

direct 与 cosim 共用地址规则，**过 cache 流量不保证逐条相同**：跳过计算和在途请求会改变跨 warp/CTA 顺序、MSHR 合并及 LRU。需要实际调度顺序的地址流时使用 `cosim --trace`。
硬件时序尚未校准；原有 estimated-address、依赖资格等限制保留。本次没有做 NCU 拟合或新 GPU 采样。

## 构建与使用

要求 C++20、zlib、Python 3。默认直接 clang 构建，无须 CMake。

```sh
python3 build.py --output build/shared-frontend-r2 --jobs 2 --native --thin-lto
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

## 初次融合验收

以下为 `ad8afa3` 集成基线的记录。最新源码的独立原公式检查、CTA 校验负测、20-family trace 精确回归及配对性能结果见 [共享前端报告](docs/shared-frontend.md) 和 `validation/shared-frontend.json`。

- 32 B：15 种 dirty mask、跨行/重复/部分写、6 种容量、step/epoch1/4/8 背压；ASan/UBSan 三模式共 59,321 项检查，每 tick 核验 dirty-sector/byte 守恒。
- Trace：18,981 项检查；后端开关前后完成序列、周期、读写及物理统计一致，覆盖重试、损坏、截断、配额和文件发布。
- 9 类 binding 对原 Builder：19,816 条访存指令、542,588 个地址范围，方向、bypass、matrix、subop、lane、地址和宽度精确一致。
- 20 类 kernel 的实际运行、decode 子集性能和同二进制 trace 开关比较，见 `validation/qualification.json`、`docs/qualification.md`。

上述为代表性来源和有界 CTA 验证，未重跑 1138 个完整网格；不把 direct/cosim 流量相等列作验收条件。格式与地址含义见 `docs/native-trace.md`。

复跑入口（均为 CPU 工作）：

```sh
python3 tests/run_unit_tests.py --output build/unit-tests
python3 tests/run_unit_tests.py --output build/unit-tests-asan --sanitize address,undefined
python3 tests/prepare_inputs.py --source-runtime /absolute/path/canonical-full-runtime-r4 --output build/validation-inputs
python3 tests/run_direct_projection.py --build build/shared-frontend-r2 --input build/validation-inputs/families.input --output build/projection
python3 tests/run_direct_projection.py --test prepared-memory --build build/shared-frontend-r2 --input build/validation-inputs/decode-512.input --output build/prepared-memory-check
```

准备脚本复用原封存的 CPU lowering，记录准备时间和输入 SHA，不进行 GPU 采样。
该准备器沿用上游 Darwin 内存监控，数据准备仍依赖当前本机封存目录。
