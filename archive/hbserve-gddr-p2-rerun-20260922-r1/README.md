# GDDR P2 (17.10 Gb/s) 重跑归档 — 分支说明

本目录随 `codex/hbserve-gddr-p2-rerun-20260922-r1` 分支提交，用于把**产生
`outputs/ada-gddr-p2-rerun-r1/result-r1/dashboard.html` 的代码**固定在 tilegen 仓库里。**

归档时间 2026-09-25（Asia/Shanghai）。只做复制与提交，未重新编译、未重新运行模拟。

## 1. 分支关系

| 项 | 值 |
|---|---|
| 新分支 | `codex/hbserve-gddr-p2-rerun-20260922-r1` |
| 基准分支 | `codex/ada-r4-latest-20260924`（tip `90a66809`） |
| 闭包文件 | 195 |
| 与基准一致 | 151 |
| 与基准不同 | 9 |
| 基准中不存在 | 35 |

`source/DELTA-vs-branch.tsv` 列出每个文件的分类（`SAME` / `CHANGED` / `ABSENT`）。

新分支把这 195 个编译闭包文件**全部**落到其 `source/...` 位置，因此分支内的闭包与
完整归档 `source/` 目录逐字节一致，可直接在本分支上重建 `fixture`。

## 2. 本目录内容

```
source/archive/hbserve-gddr-p2-rerun-20260922-r1/
├── README.md               本文件
├── RECIPE.md               原 GDDR P2 复现包说明（含原始构建 flags 与验收锚点）
├── DELTA-vs-branch.tsv     195 个闭包文件相对基准分支的分类
├── MANIFEST.tsv            原最小运行集清单（path/bytes/sha256）
├── receipts/               结果、凭据与目标 dashboard 产物
│   ├── data.json / report.md / receipt.json
│   ├── pins-manifest.json / minimal-manifest.json
│   ├── acceptance-{prefill,decode}-sweep.json
│   └── result-r1/{data.json,report.md,receipt.json,index.html,dashboard.html,dashboard-receipt.json}
└── control/                控制层脚本（构建 / 运行 / 对比 / 渲染）
    ├── build_runtime.py    由冻结的 history_runtime-r3.cpp 派生链接单元
    ├── run_full.py         本次重跑的控制器
    ├── contract.py         重跑契约
    ├── report_compare.py   由 result.json 生成 data.json
    ├── build_dashboard.py  由 data.json 生成 dashboard.html
    ├── check_source.py · progress_report.py
    ├── prepare.py · run_windows.py · run_cpu.py · fixture.cpp
    ├── runtime-r2/         history_runtime.cpp + derivation.json
    ├── structure-adapter/  结构适配头 + overlay.json + 凭据
    └── write-attribution/  第二个 overlay + writer_observer.h
```

**不含** 97 MB 运行输入与 21 个 macOS `.o` / `fixture` 二进制：
- 输入按 `MANIFEST.tsv` 的 `inputs/...` 条目定点取回；
- 二进制是 macOS arm64，Linux 无用，必须从 `source/` 重建。

完整归档（含 97 MB 输入、`control/` 全量、`repo/` worktree）位于 xmu：
`/home/xmu/nvidiagds/codex-runs/hbserve-memgen-gtsim-alignment/`

## 3. 结果锚点

| 锚点 | 期望值 |
|---|---|
| `status` | `PASS_SELECTED_CONTINUOUS_CURRENT_HISTORY` |
| counts | `native_kernel=1138`, `memory_api_submission=29`, `epoch_begin=3`, `epoch_end=3` |
| `rows` | `1173` |
| `CTAs` / `nodes` | `772512` / `4712768255` |
| `cycles` | `826832904` |
| `HBFSIM.physical` | `read_bytes=45241836416`, `write_bytes=146843072`, `finish_ns=380129130.2923976` |
| 宿主执行 | `4282.494552875` s（机器相关，可不同） |
| `data.json` sha256 | `58eabb56d9f20c9bf6f49589ccaa209ac4468d326ec66fb0618f76b2a6031e19` |
| `dashboard.html` sha256 | `03cfd1dff28de4fe62124e1cfd9484678b43795b62cc292209d8861c726c3731` |

一小时 target **未达到**（`one_hour_target_met=false`）。

## 4. 重建注意

- `runtime-r2/history_runtime.cpp` 内是绝对 macOS 路径，且文本包含
  `source/work/tilegen-full-r1/canonical-full-runtime-r4/streaming.cpp`；
  21 个 `.o` 不含该 TU，否则重复符号。
- 两个 `-ivfsoverlay` 均写死 macOS 绝对路径；闭包 `source/` 已是解析后内容，
  直接编译可以**不用 overlay**。
- x86 上去掉 `-mcpu=native` 与 `-flto=thin`；需要 `-lz`。

详细步骤见完整归档的 `README.md`。
