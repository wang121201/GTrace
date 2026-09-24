# Ada sector32 线稳定版（2026-09-24）

本页记录 `stable/ada-sector32-line` 分支的来源、三个提交的重建过程与保真证据、32 B sector
策略的实际效果、以及复现步骤。

## 1. 这条线是什么

sector32 修正的是 **L2 读服务粒度**：取消强制整行 store RFO，改为 32 B sector 的
known-byte / dirty-byte 掩码。原 r4 的 L1 串行读过滤、40 MiB `PAPER_ADA_L2_V1_20x1024x16`
分组映射、EF288、64M L2 访问老化、完整 GLOBAL/API 图与源执行顺序都保持不变。

策略由 `TILEGEN_L2_DATA_POLICY=sector32|old128` 选择（新 CLI 默认 `sector32`）。
契约与账目定义见 [llm/tests/sector32-cache-contract.md](../llm/tests/sector32-cache-contract.md)；
C++ L1 适配层入口见 [llm/CPP-L1-ADAPTER.md](../llm/CPP-L1-ADAPTER.md)。

> **这不是完整的 Accel-Sim 参数/时序等价实现。** MSHR 与 GPU 并发未纳入，本轮无 HBFSIM
> cosimulation，只做 functional traffic。

## 2. 三个提交原本是孤本

sector32 与后续两个 age 提交，在**任何受跟踪仓库中都不存在**。运行目录里只有物化快照与
一份不可变源清单：

| 运行目录 | 记录的原始提交 | 记录的分支 | 文件数 |
|---|---|---|---:|
| `gtsim-ada-sector32-20260922-r1` | `0b49db6722ed` | `codex/gtsim-ada-sector32-20260922` | 492 |
| `gtsim-ada-age-20260922-r1` | `17f03b070ecb` | `codex/gtsim-ada-sector32-age-20260922` | 496 |
| `gtsim-ada-set-age-20260922-r1` | `90a66809089b` | `codex/gtsim-ada-set-age-20260922` | 500 |

`repo-manifest.json`（schema `SECTOR32_IMMUTABLE_SOURCE_V1`）逐文件给出 bytes 与
SHA-256，因此可以**逐字节验证**重建结果。

### 2.1 血统已实测确定

把每个快照与候选父提交比对（git blob 身份）：

| 快照 | vs `35c1367`（r4） |
|---|---|
| sector32 | **相同 421 / 改动 0** / 新增 71 |
| age | **相同 421 / 改动 0** / 新增 75 |
| set-age | **相同 421 / 改动 0** / 新增 79 |

即 `35c1367` 是**精确父提交**，三个快照在其之上纯增量。链路：

```text
0e21251 → … → 7533322 → 35c1367 (r4)  →  0b49db67 (sector32)
                                   →  17f03b07 (age)
                                   →  90a66809 (set-age)
```

增量内容：sector32 新增 `llm/`（70 个文件，C++ L1 适配层与 Python 驱动）+
`tests/prefill_o_adl_test.cpp`；age 新增 4 个 `llm/tools/age_*.py` 并改
`llm/executor-r1/whole_stream.py`；set-age 新增 4 个 `llm/tests/set_age_*` 并改 6 个文件。

> 这同时解释了此前的疑问：sweep 需要的 `llm/fast-prefill-sweep-r1/` 等先前只见于
> `*.tar.gz` 的驱动，其实**已随 sector32 提交进入版本库**。

## 3. 重建与保真证据

三个提交在本仓库重建为 `import/ada-sector32-originals` 分支，父提交为 `35c1367`。
每个提交的 message 都记录了原始 SHA 与原始分支名，映射关系不会丢。

重建后**独立复验**：对每个重建提交执行 `git archive` 解包，再逐文件比对原始 manifest：

| 重建提交 | 原始 | 一致 / 总数 | 不一致 | 缺失 | 多余 |
|---|---|---:|---:|---:|---:|
| `c5cd8a4` | `0b49db67` | **492 / 492** | 0 | 0 | 0 |
| `3b41ecf` | `17f03b07` | **496 / 496** | 0 | 0 | 0 |
| `f74248d` | `90a66809` | **500 / 500** | 0 | 0 | 0 |

另对快照本身直接校验：492/492、496/496、500/500 一致，且磁盘无 manifest 未声明的多余文件。

**重建提交的 SHA 与原始不同**（无法复原原始 author/committer 时间与 message），因此
任何按原始 SHA 取文件的代码都必须可覆盖 —— 见 §5。

## 4. 稳定化

原始三个提交都建立在 `35c1367` 上，而该提交**缺** r2 之后的 `5211abc`（命名空间修复）与
`e412508`（源 SHA 重封存）。稳定化把它们移植到 r4 稳定版尖端：

```text
9e32a54  set-age
3dff1d7  age
42a096f  sector32
29a9d98  r4 归档说明
bcdd0ba  r4（= e412508 + r4）
e412508  重新封存
5211abc  命名空间修复
```

移植为纯增量（全部落在 `llm/` 与 `tests/`），与 r4 / 重封存改动**无文件重叠**，无冲突。

## 5. 为重建而做的代码修改（唯一一处）

`llm/tests/run_set_age_tests.py` 原先硬编码 `frozen_commit='17f03b07…'` 并直接
`git show` 该提交的 `llm/executor-r1/candidate_direct_cache.h`。重建仓库里没有这个 SHA，
所以改为可配置：

```python
ORIGINAL_FROZEN_COMMIT='17f03b070ecb86fa034597a6a7ac972ff319bef7'
# --frozen-commit <sha>  或  TILEGEN_SECTOR32_FROZEN_COMMIT=<sha>
```

默认值仍是原始 SHA，行为不变；取不到时给出明确报错并指出替代做法。receipt 里记录的
`frozen_sector32_reference.commit` 始终是**实际使用**的那个提交，不会伪装成原始提交。

## 6. 复现与验证结果（本机实测）

```bash
# sector32 组件 + 真实主程序 CLI（282,148 项检查）
python3 llm/tests/run_sector32_tests.py --output /tmp/s32 --cxx g++

# set-age 原型（11,524 项检查）——需要指向重建提交
python3 llm/tests/run_set_age_tests.py --output /tmp/sa --cxx g++ --frozen-commit 3dff1d7

# r4 回归：本分支仍应通过
python3 ada_r4.py \
  --input ~/nvidiagds/codex-runs/rtx4000-ada-cache-calibration-20260922-r4/analysis/evaluation_cases.tsv \
  --output /tmp/r4-regress --compiler g++
```

| 验证项 | 结果 |
|---|---|
| `run_sector32_tests.py` | ✅ `PASS_SECTOR32_COMPONENTS_AND_ACTUAL_MAIN` · **282,148** 项 · 5 项 CLI |
| `run_set_age_tests.py` | ✅ `PASS_SET_AGE_PROTOTYPE_LOCAL_COMPONENTS` · **11,524** 项 |
| `ada_r4.py` 回归 | ✅ `PASS_PROCESS_ONLY` · 384 cases |

`llm/tests/l1_integration_test.cpp` **不被任何 runner 引用**，是遗留诊断文件；其运行输出
`legacy per-instruction snapshot differs` 并返回 1，**不代表本线回归**，也不属于已记录的
测试集（`sector32-results.json` 的 `component_tests` 只含 legacy32 与 r4 两项）。

## 7. 实测效果（来自 2026-09-22 运行目录）

读服务粒度修正后，Prefill 读流量对 NCU 的偏差显著下降：

| 条件 | 阶段 | 旧 128 B | 新 sector32 | 对 NCU 偏差 |
|---|---|---:|---:|---|
| P32 | Prefill READ | 3.16 GB | **3.10 GB** | +2.07 % → **+0.14 %** |
| P128 | Prefill READ | 3.40 GB | **3.14 GB** | +9.18 % → **+0.81 %** |

- 已闭合的 **8 个自然 ROI 写流量与旧策略逐字节相同，全部通过** —— 本轮只改读服务，
  未改 dirty 创建与写回触发。
- P128 D2 自然写流量 0.94 MB 中 0.94 MB（99.99 %）来自 **age 写回**。这是模拟器内部
  归因，不能反推硬件采用相同机制。
- 新 P128 锁频对照 24 个单 pass 窗口：逐步 decode write 仍未过 <20 % 门槛
  （D1 +99.51 %、D2 +836.23 %）。两臂各 ROI write 均值变化 <2 %，不足以解释该差距。

**age 后续（10 组）**：保持 32 B sector 与 read 不变，Global/Set 固定 age 候选**均未通过**
P32/P128 逐步 write 误差 <20 % 的联合验收。因此本线交付的是**读侧的改进**，
写侧 gap 未闭合。

## 8. 边界

- functional traffic 交付，无 GPU 执行、无 HBFSIM cosimulation、无完整模型精度声明。
- 不含 MSHR / GPU 并发 / 时序；不含 Accel-Sim 完整参数与时序等价。
- L2 的 16 ways、EF288、64M age 不来自 r4 tuner，属本项目经验策略。
- 掩码保留在 functional 状态、observer 与伴随 hash 中；原 trace record 格式未扩展
  byte mask 字段，因此旧格式 postcache hash 单独不足以作为新模式的掩码身份证明。
- 未做 memory value 模拟，未提供独立数值回放 trace。

## 9. 产物位置

| 内容 | 位置 |
|---|---|
| 稳定分支 | `stable/ada-sector32-line` |
| 原始重建链 | `import/ada-sector32-originals`（3 个提交，message 记录原始 SHA） |
| 运行目录（快照 + 清单 + 结果） | `gtsim-ada-sector32-20260922-r1/`、`gtsim-ada-age-20260922-r1/`、`gtsim-ada-set-age-20260922-r1/` |
| 合同与账目 | `llm/tests/sector32-cache-contract.md` |
| 适配层说明 | `llm/CPP-L1-ADAPTER.md` |
| 组件验证记录 | `llm/tests/sector32-results.json` |
