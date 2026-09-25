# Smoke 测试说明

按**从零到完整回放**的顺序分档。每档标明是否需要封存输入、预期耗时、判定标准，以及
**2026-09-25 瘦身后的实测结果**。

判定标准一律以工具输出的 `status` 字段和收据 JSON 为准，不以退出码为唯一依据。

## 0. 环境前提

| 项 | 要求 |
|---|---|
| 系统 | Linux x86_64（本轮 48 核验证机） |
| Python | 3.10+（stdlib only；工具脚本无第三方依赖） |
| 编译器 | C++20。**注意**：本机 `clang++` 14 与 `g++` 11 各有已知局限，见 [§9](#9-已知阻塞项既有非瘦身引入) |
| 磁盘 | 完整 build 约 200 MB；完整 sweep 每 case 约 30–700 MB |

所有命令都从**仓库根目录**运行。除 S0/S1/S2 外，其余档位需要**封存输入**（见 §7）。

---

## 1. S0 — 归档完整性（离线，< 1 s）

复算封存证据的 SHA-256、确认去重结果、检查文档链接。不编译、不仿真、不使用 GPU。

```sh
python3 tools/smoke_archive.py .
```

**判定**：末行 `S0 archive integrity: N pass / 0 fail`，退出码 0。

| 检查组 | 内容 |
|---|---|
| 1 | `run-r1/baseline`、`run-r1/candidate` 仍匹配 `receipt.json` 里的 SHA pin |
| 2 | `receipts/result-r1/data.json` == `58eabb56…3e19` |
| 3 | 2026-09-25 删掉的 3 个重复副本确实不在；无 `.d` 回流 |
| 4 | 两份 `acceptance-*.json` 可解析且带 r4 列 |
| 5 | 所有相对 Markdown 链接可解析 |

**实测**：`9 pass / 0 fail`（瘦身后）。

---

## 2. S1 — 依赖闭合扫描（离线，~1–2 s）

不编译，只验证所有非系统 include 都落在 `source/` 内。它仍然要调用编译器做
`-MM` 依赖扫描，所以**必须指定一个能用的编译器**：

```sh
python3 build.py --output build/deps-check --compiler g++ --deps-only
```

> ⚠️ 不要省略 `--compiler`。默认走 `build-config.json` 的 `clang++`，而本机 clang14
> 找不到 libstdc++ 头（见 §9 阻塞项 B3），22 个 TU 会全部以
> `fatal error: 'cstdint' file not found` 失败，状态是 `FAIL_BUILD` —— 那是编译器
> 问题，不是依赖不闭合。

**判定**：`"status": "PASS_SELF_CONTAINED_DEPENDENCY_SCAN_NO_BUILD"`。

**实测**：`PASS`，`wall_seconds = 1.11`，`local_dependency_files = 191`。
说明源码树的 include 闭包是自包含的，**这一点没有被瘦身破坏**（删掉的只有
`.d`/`.o`/重复收据）。

---

## 3. S2 — CPU 单元测试（离线，~80 s）

```sh
python3 tests/run_unit_tests.py --output build/unit-tests --compiler g++
```

19 个 C++ 程序，全部为 CPU 工作，不执行 GPU 或模型工作流。

**判定**：读 `build/unit-tests/summary.json` 的 `tests` 字段，逐项看
`compile.returncode` 与 `run.returncode`。

**实测（瘦身后）**：**10 通过 / 9 编译失败**

| 通过（10） | 检查数 |
|---|---|
| `cache_geometry_test` | 227,156 |
| `dirty32_test` | 28,392 |
| `native_trace_test` | 18,981 |
| `trace_replay_test` | 11,677 |
| `cta_validation_test` | 11,530 |
| `stage_replay_test` | 4,428 |
| `replay_backend_test` | 3,842 |
| `direct_phase_profile_test` | 25 |
| `replay_phase_report_test` | 27 |
| `direct_cache_smoke` | 10 项具名检查 |

合计 **306,058 项数值检查 + 10 项具名检查全部通过**。

| 编译失败（9，既有问题） |
|---|
| `ada_l1_clock_test`、`ada_l1_policy_test`、`ada_l1_test`、`ada_profile_test`、`ada_address_mapping_test`、`ada_direct_cache_test`、`ada_fine_partition_test`、`ada_calibrated_profile_test`、`ada_calibrated_timing_test` |

**这 9 个失败与瘦身无关**：用瘦身前的提交 `2f9ab10` 跑同一命令，得到完全相同的 9 个失败
（见 §10 对照记录）。根因见 §9 阻塞项 B1。

---

## 4. S3 — 完整原生构建（离线）

```sh
# 本机 clang14 需要显式指向 libstdc++（见 §9 阻塞项 B3）：
cat > /tmp/clangwrap <<'EOF'
#!/bin/sh
exec /usr/bin/clang++ -isystem /usr/include/c++/11 \
     -isystem /usr/include/x86_64-linux-gnu/c++/11 "$@"
EOF
chmod +x /tmp/clangwrap

python3 build.py --output build/native --compiler /tmp/clangwrap --jobs 2 --native --thin-lto
# 或
python3 build.py --output build/native-gcc --compiler g++ --jobs 2 --native
```

**判定**：`build/build-receipt.json` 的 `status == "PASS_BUILD"`，产出 `tilegen_native`。

**实测（瘦身后）**：**FAIL_BUILD**（两个编译器各卡在不同 TU，见 §9 阻塞项 B2）：

| 编译器 | 失败 TU | 错误 |
|---|---|---|
| clang14 | `08` = `hbfsim-latest/upstream/src/physical/address_heatmap.cpp` | `[member]` 捕获结构化绑定——clang14 未实现 P1091R3（C++20） |
| clang14 | `12` = `.../physical/hbm/hbm_device.cpp` | `optional::emplace()` 无参不可构造 |
| g++11 | `00` = `canonical-full-runtime-r4/streaming.cpp` | `'statistics' was not declared in this scope; did you mean 'p28::statistics'?` |

21/22 个 TU 中 **20 个编译通过**（clang 路线），失败集中在上表。这不是瘦身引入的：
瘦身只删除派生产物与文档，未改动任何 `.h`/`.cpp`。

---

## 5. S4 — Ada 功能回放（离线，~4 s）

```sh
python3 ada_profile.py --compiler /tmp/clangwrap --profile r2-adaptive \
  --input tests/fixtures/ada-sector.jsonl \
  --output build/ada-example-r1 --emit-trace
```

**判定**：`receipt.json` 的 `status`，以及 `result.json` 的读写字节与
`validation/ada-accelsim-r1/local-fixture-result.json` 一致。

**实测（瘦身后）**：**FAILED_COMPILE**，19 个错误，全部同源（§9 阻塞项 B1）：

```
ada_tuner_profile.h:58: error: no member named 'sector32' in 'GTSim::PerSmL1Config'
ada_tuner_profile.h:58: error: no member named 'write_allocate' in ...
ada_tuner_profile.h:58: error: no member named 'dirty_protection_percent' in ...
ada_tuner_profile.h:61: error: no member named 'accelsim_rtx4000_ada_v1' in 'GTSim::L2GeometryConfig'
ada_calibrated_profile.h:52: error: unknown type name 'PerSmL1ReplacementPolicy'
```

用 `--compiler g++` 会先卡在另外一个问题上（`nlohmann/json` 初始化列表推导，
`{"issue_cycle",nullptr}`）。

---

## 6. S5 — 结构适配器 fixture 重编重跑（**需封存输入**，~19 s）

重建 6 个核心 TU + 2 个 fixture，并跑 baseline / candidate 冷启序列比对。

```sh
python3 archive/hbserve-gddr-p2-rerun-20260922-r1/control/structure-adapter/prepare.py
python3 archive/hbserve-gddr-p2-rerun-20260922-r1/control/structure-adapter/run_cpu.py
```

**判定**：`run-r1/receipt.json` 的 `status == "PASS_OWNED_ADA_STRUCTURE_ASYNC_COMPONENT"`，
且 373,904 项检查全过。已归档的收据记录：18.8151 s、274 项输入 pin 不变、十步全零退出、
runtime stderr 为空。

> 这两个脚本按原始交付的**绝对路径**读取封存输入（`work/...`），因此**不能只靠本仓库运行**。
> 需要先按 §7 恢复输入。

---

## 7. S6 — sweep 重放与判等（**需封存输入**）

单 case：约 23 分钟（`P64/D2`）；完整 7 case：约 143 分钟（6 并发）。

```sh
# 单 case（最小）
<r4-spec argv> --fast-prefill-sweep ... --output <case>/repro-YYYYMMDD
# 逐 case 独立进程，原样 argv + 9 个环境变量，taskset 绑核
```

**判定**：`status.json` 的 `status == "PASS_COMPLETE_NATIVE_GRAPH_CACHE_EXECUTION"`，
且 `phase-traffic.json` 与 `receipts/acceptance-{prefill,decode}-sweep.json` 逐格对齐。

### 判等规则（必须遵守）

1. 行标签：prefill 表用 `P64`/`P256`/`P512`；decode 表用 `D2`/`D4`/`D8`/`D16`。
2. 窗口**自动判定**——用 `r4 Read` 的值去匹配 `phase-traffic.json` 的
   `DRAM_read_bytes`，不要硬编码表序号。
3. 容差必须**单位感知**：`GB → ±5 MB`，`MB → ±5 KB`。用错会产出假差异。
4. 每个 case 比 4 个窗口：`Measured/Full`、`Measured/Prefill`、
   `Measured/Decode1`、`Measured/Decode2`，读 / 写双向。

**不可对齐的三类列**（语义上不可能相等，不是失败）：

| 列 | 原因 |
|---|---|
| `NCU Read` / `NCU Write` | 是 Nsight Compute 硬件采集的**输入**，是被对齐目标，不是本分支输出 |
| `CPU min` / `墙钟 min` | 依赖宿主与并行度；6 并发下同一 case 可从 23 min 变 51 min |
| 整文件 `summary SHA256` | 含上面三个宿主相关字段，须改用**规范化哈希**（剔除 `CPU_minutes`、`wall_minutes`、`snapshots_path`）：`11622c61911dbcbd72a7d9b2b6bdbdeb537919c1de2740f8599df049225ca426` |

**实测（2026-09-25）**：7/7 case `PASS_COMPLETE_NATIVE_GRAPH_CACHE_EXECUTION`，
**76/76 数值单元格与两份 acceptance 表一致，零差异**。

### 封存输入

| 用途 | 位置 |
|---|---|
| 最小可复现包（225 文件，MANIFEST 全通过） | `~/nvidiagds/codex-runs/gddr-p2-repro-20260924/` |
| sweep 重放产物 | `~/nvidiagds/codex-runs/{gtsim-ada-r4-prefill,gtsim-ada-r4-p128}-20260922-r1/cases/*/repro-*/` |
| 原始 Mac 工作树 | `/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/` |

---

## 8. S7 — 归档算例重建与有界重跑（离线，约 2.5 分钟）

针对 GDDR P2 归档算例本身：用本分支的代码重建 `fixture`，再用真实 plan 跑有界时长，
断言**已产出的前缀行与归档结果逐字段相同**。这是唯一一条端到端验证归档算例的路线
（S3 只构建 `tilegen_native`，S5/S6 还依赖另一套封存输入）。

```sh
cd archive/hbserve-gddr-p2-rerun-20260922-r1
python3 smoke/smoke.py                   # 三级全跑，不可用级别自动 SKIP
python3 smoke/smoke.py --stage build     # 只做构建，约 20 秒
python3 smoke/smoke.py --seconds 120     # 有界重跑 120 秒
```

| 级别 | 内容 |
|---|---|
| `integrity` | `TREE-SHA256.tsv` 里 224 个编译输入，`receipts/result-r1/{data.json,dashboard.html,receipt.json}` 的 pin，`MANIFEST.tsv` 中本仓库内的部分 |
| `build` | 按原始 macOS 布局组装工作树 → 实体化 2 处 `-ivfsoverlay` → 打 `p28::statistics` 补丁 → 编译 21 个 TU → 链接 |
| `run` | 校验 plan 的 sha256，然后有界运行，并与归档结果逐字段比对前缀行 |

**判定**：末行 `--- N ok, 0 skipped, 0 failed ---`，退出码 0。

**实测（2026-09-25，48 核验证机，g++ 11.4）**：**11 ok / 0 skipped / 0 failed，2 分 26 秒**

| 项 | 结果 |
|---|---|
| `build` | 21 个 TU 用 16.6 s 编译，链接出 `fixture` 6,247,432 B，sha256 `5090e74cd7c7a040…` |
| `run`（60 s 有界） | `phase=Measured/Prefill  kernels=24  nodes=44/1173` |
| 前缀比对 | **前 44 行与归档结果逐字段相同**（只排除宿主耗时字段） |

该二进制 sha256 与完整 144 分钟重跑的产物**完全相同**，说明有界重跑足以判定构建正确性。

没有 `bubblewrap` 时自动退回 `--rewrite-paths`（可移植但非逐字节保真，只做构建冒烟）。
约 1.3 GB 的运行输入不在仓库内，缺失时 `run` 级别会 SKIP。

---

## 9. 已知阻塞项（既有，非瘦身引入）

### B1 — `PerSmL1Config` API 缺口（影响 S2 的 9 个测试、S4）

提交 `35c1367`（"Add opt-in Ada r4 serial CLOCK replay…"）新增了 7 个使用新版
`PerSmL1Config` 成员的文件：

```
source/work/tilegen-full-r1/core-native-copy-r2/include/ada_r4_profile.h
source/work/tilegen-full-r1/core-native-copy-r2/include/ada_calibrated_profile.h
source/work/tilegen-full-r1/core-native-copy-r2/include/ada_tuner_profile.h
llm/executor-r1/llm_l1_adapter.h
tests/ada_l1_clock_test.cpp
tests/ada_l1_policy_test.cpp
tests/ada_calibrated_profile_test.cpp        (+ ada_calibrated_timing_test.cpp)
```

但它们需要的成员 / 类型**在任何树里都不存在**：

```
PerSmL1Config::{sector32, write_allocate, dirty_protection_percent, replacement, hash_policy}
PerSmL1ReplacementPolicy, PerSmL1HashPolicy
L2GeometryConfig::accelsim_rtx4000_ada_v1
```

`source/.../core-native-copy-r2/include/per_sm_l1.h` 最后一次改动是 `2f9ab10`，
仍是旧版。**本地 41 份 `per_sm_l1.h`（13 个不同 SHA）全部不含这些成员。**

→ 结论：`ada_profile.py` / `ada_r4.py` / 上述测试**无法从本仓库编译**。r4 sweep 之所以能跑，
是因为它走的是另一套 `build_runtime.py` + VFS overlay + 绝对 Mac 路径的构建，而不是 `build.py`。

**建议**：要么补上对应版本的 `per_sm_l1.h`（需要确认与 sweep 时实际编译的头逐字节一致），
要么把这 9 个测试与 3 个 profile 头标为「需外部 overlay」并从默认测试集移出。

### B2 — `build.py` 双编译器不兼容（影响 S3）

见 §4 表格。三处都是**源码级**问题，不是环境问题：

| 位置 | 问题 |
|---|---|
| `hbfsim-latest/.../address_heatmap.cpp:942` | lambda 用 `[member]` 捕获结构化绑定，需 clang ≥ 17（P1091R3） |
| `hbfsim-latest/.../hbm_device.cpp:1950` | `optional<CommandProgress>::emplace()` 无参；两编译器可构造性判定不同 |
| `canonical-full-runtime-r4/../driver-prefill-gemm-next-r1/streaming.cpp:122` | 未限定的 `statistics`（应为 `p28::statistics`）+ `json` 赋值推导失败 |

**建议**：`build.py` 增加按 TU 选择编译器，或修掉这三处（第一处是 clang14 过旧，升级工具链即可）。

其中第三处（未限定的 `statistics`）**已有验证过的修法**：改成 `p28::statistics`
（与仓库提交 `5211abc` 同一改动），语义不变。S7 的归档构建就只打了这一行补丁，
g++ 随后 21/21 个 TU 全部编译通过、链接成功，并跑出与归档逐位一致的结果。

### B3 — clang14 默认 libstdc++ 路径错误（影响 S3/S4 的 clang 路线）

clang14 默认搜索 `/usr/include/c++`（**不带版本号，本机不存在**），实际只装了
`/usr/include/c++/11`。症状：`fatal error: 'array' file not found`。

**规避**：用 §4 的 `/tmp/clangwrap`，或 `-isystem /usr/include/c++/11 -isystem /usr/include/x86_64-linux-gnu/c++/11`。

### B4 — g++ 与 `nlohmann/json` 初始化列表（影响 S4 的 g++ 路线）

`source/ada_cache_replay.cpp:23` 的 `J({…,{"issue_cycle",nullptr}})`
在 g++11 下无法推导 `CompatibleType`。clang 可以。

---

## 10. 对照记录（2026-09-25）

瘦身前提交 `2f9ab10` 与本轮瘦身结果 `b6c89d7`（+ 文档提交）的对照：

| 检查 | 瘦身前 `2f9ab10` | 瘦身后 | 结论 |
|---|---|---|---|
| 文件数 / 体积 | 679 / 19 MB | 625 / 13 MB | −8% 文件、−32% 体积 |
| `build.py --deps-only` | PASS（192 依赖） | PASS（192 依赖） | 不变 |
| `tests/run_unit_tests.py` | 10 通过 / 9 编译失败 | 10 通过 / 9 编译失败 | **完全相同** |
| 失败测试清单 | 同 9 个 | 同 9 个 | 既有问题 |
| S0 归档完整性 | 9 pass（去重前口径不同） | 9 pass / 0 fail | 证据完好 |
| 7-case sweep 判等 | 76/76 | 76/76 | 不变 |

结论：**瘦身没有改变任何可构建性、测试结果或验收数据**；减少的是派生产物、
逐字重复的大文件，以及重叠的设计文档。

---

## 11. 相关文档

- 构建与运行：[README.md](README.md)
- 设计说明索引：[docs/README.md](docs/README.md)
- 阶段性历史：[HISTORY.md](HISTORY.md)
- 归档收据布局：[archive/hbserve-gddr-p2-rerun-20260922-r1/receipts/README.md](archive/hbserve-gddr-p2-rerun-20260922-r1/receipts/README.md)
