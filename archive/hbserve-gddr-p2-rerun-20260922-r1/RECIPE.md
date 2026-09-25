# GDDR P2 (17.10 Gb/s) 复现包

产生 `outputs/ada-gddr-p2-rerun-r1/result-r1/dashboard.html` 的**最小可运行集**。
本包不含 764 MB 的准入证据（pins）实体文件，只带其 `path/bytes/sha256` 清单。

## 1. 包结构

| 目录 | 文件 | 大小 | 说明 |
|---|---:|---:|---|
| `source/` | 195 | 3.46 MB | 编译闭包（21 个 TU + 全部头文件依赖，含 VFS overlay 重定向后的真实文件；已按绝对路径去重） |
| `inputs/` | 22 | 101.88 MB | 运行期 `loader` 实读的 19 个输入 + `plan.json` + 2 个 `.cfg` |
| `receipts/` | 7 | 1.36 MB | 凭据与验收基准（见下） |
| `MANIFEST.tsv` | 1 | ~40 KB | 包内每个文件的 `path/bytes/sha256` |
| `RECIPE.md` | 1 | — | 本文件 |
| **合计** | **225** | **106.70 MB** | |

`receipts/` 内容：

| 文件 | 用途 |
|---|---|
| `pins-manifest.json` | 全部 1355 个 admission pin 的 `path/bytes/sha256`（293 KB，**代替 764 MB 证据**） |
| `minimal-manifest.json` | 最小运行集清单 |
| `data.json` / `report.md` / `receipt.json` | 目标结果本身（分析产物） |
| `acceptance-prefill-sweep.json` / `acceptance-decode-sweep.json` | 两份 sweep HTML 抽出的验收基准（另一条对齐目标，见 §7） |

## 2. 原始运行环境（实测）

| 项 | 值 |
|---|---|
| platform | `macOS-26.5.1-arm64-arm-64bit` |
| compiler | Apple clang 21.0.0 (clang-2100.1.1.101) |
| 链接单元 | `history_runtime.cpp`（**文本包含** `canonical-full-runtime-r4/streaming.cpp`）+ 21 个预编译 `.o` |
| 产物 | `fixture` = Mach-O arm64；21 个 `.o` = LLVM thin-LTO bitcode |
| 运行时 | 4282.49 s（本次）；墙钟总计 4284.31 s |

> ⚠️ **现有 `fixture` 与 `.o` 都是 macOS arm64 产物，Linux 不能链接**。在 xmu 上必须用 `source/` 从源码重建。

## 3. 构建

原始 flags 共 16 条（`build-receipt.json` 记录）：

```
-std=c++20 -O3 -Wall -Wextra -pthread
-DTILEGEN_DIRTY_SECTOR_MODE=2
-DTILEGEN_SOURCE_MEMORY_SEMANTICS=1
-DTILEGEN_EF_INSERTION_COUNTERFACTUAL=1
-I<ROOT>/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/tilegen-full-r1/core-native-copy-r2/include
-I<ROOT>/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/tilegen-evaluation-r1/driver
-I<ROOT>/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/tilegen-norm-shared-r1/adapter
-I<ROOT>/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/gddr6-support/delivery-stage/sources/third_party/gtsim-prepared/third_party
-I<ROOT>/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/hbfsim-latest/upstream/src
-I<ROOT>/work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot/source/work/gddr6-support/hbf_coupling
-mcpu=native
-flto=thin
```

编译时另加两个 VFS overlay（只重定向 3 个头文件）：

```
-ivfsoverlay <ROOT>/work/ada-cosim-alignment-20260922-r1/structure-adapter/overlay.json
-ivfsoverlay <ROOT>/work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-write-attribution-r1/overlay.json
```

21 个 TU（`00.o` 不属于链接集）：

```
01-06: source/.../tilegen-full-r1/core-native-copy-r2/src/{gpu,scheduler,simulator,simulator_session,sm,subpartition}.cpp
07   : source/.../hbfsim-latest/upstream/src/app/system_config.cpp
08-21: source/.../hbfsim-latest/upstream/src/{physical/{address_heatmap,base_die_link,
        external/cxl_ssd,external/external_backing_device,hbm/hbm_device,hbf/hbf_device,
        resource_calendar,gap_calendar,simulation_session}.cpp,
        host/{hbf_gc,hbf_zones,hbf_wear_snapshot,hbf_controller,hbf_persistent_image}.cpp}
```

链接：

```bash
clang++ $FLAGS $OVERLAYS -Wno-unused-parameter \
    "$ROOT/work/ada-cosim-alignment-20260922-r1/runtime-r2/history_runtime.cpp" \
    01.o .. 21.o -lz -o fixture
```

### xmu (Linux) 适配注意事项

1. **`history_runtime.cpp` 内是绝对 Mac 路径**（`/Users/wgs/Documents/Codex/...`）——由 `build_runtime.py` 生成时写死。两种处理：
   - 在 xmu 上原样镜像该绝对路径（`sudo mkdir -p /Users/wgs/...` 亦可），或
   - 用同包内 `build_runtime.py`（在 `source/` 内）按 xmu 布局重新生成。
2. `-mcpu=native` 需按平台调整（x86 上语义不同）；`-flto=thin` 建议改 `-flto` 或去掉。
3. 系统头（1133 项 macOS SDK/libc++）不需搬运，Linux 上由 libstdc++ 提供。
4. 需要 `-lz`（zlib）。

## 4. 运行

```bash
./fixture <ROOT>/work/ada-cosim-alignment-20260922-r1/gddr-full-p2-r1/plan.json out/result.json
```

argv 只有 3 项（binary、plan、result），**没有任何开关**。运行会产出：
`result.json`、`result.json.operations.jsonl`、`result.json.progress.json`。

## 5. 验收锚点（必须逐项对上）

| 锚点 | 期望值 |
|---|---|
| `result.json.status` | `PASS_SELECTED_CONTINUOUS_CURRENT_HISTORY` |
| `result.json.counts` | `native_kernel=1138`, `memory_api_submission=29`, `epoch_begin=3`, `epoch_end=3` |
| `result.json.rows` 长度 | `1173` |
| `result.json.CTAs` / `nodes` | `772512` / `4712768255` |
| `result.json.cycles` | `826832904` |
| `result.json.HBFSIM.physical` | `read_bytes=45241836416`, `write_bytes=146843072`, `finish_ns=380129130.2923976` |
| `result.json.host_execution_seconds` | `4282.494552875`（宿主相关，可不同） |
| `result.json.loader` 打开的路径集合 | 必须 ⊆ `receipts/pins-manifest.json` 的路径集合 |
| 后处理产物 `data.json` 的 sha256 | `58eabb56d9f20c9bf6f49589ccaa209ac4468d326ec66fb0618f76b2a6031e19` |

`data.json` 由 `work/ada-cosim-alignment-20260922-r1/gddr-full-p2-rerun-r1/report_compare.py`
（本包 `source/` 未含，属控制层，需要时单独取）生成；`dashboard.html` 由 `build_dashboard.py` 生成。

## 6. 未包含的内容

| 项 | 大小 | 说明 |
|---|---:|---|
| 其余 1336 个 admission pin 实体文件 | ~662 MB | 未被运行期读取（采集证据/中间产物）。其 `sha256` 已在 `receipts/pins-manifest.json`，需要时按清单定点取回并校验。 |
| 21 个预编译 `.o` | 5.4 MB | macOS arm64 bitcode，Linux 无用 |
| `outputs/ada-gddr-p2-rerun-r1/result-r1/*` | 1.0 MB | 目标结果，已放 `receipts/` |
| 控制层脚本 | ~3 MB | `run_full.py` / `contract.py` / `report_compare.py` / `build_dashboard.py` |

## 7. 第二条对齐目标（两份 sweep HTML，Qwen1.5B）

`receipts/acceptance-*.json` 是从两份 sweep HTML 的**表格**抽出的验收基准
（文档无 `<script>`，数据只在 `<table>` 里）：

| 文档 | 表数 | doc sha256 |
|---|---:|---|
| `qwen1p5b-r4-prefill-sweep.html` | 17 | `cfea016f2cb1b3237d09211b2a8ba01bd682d37f8af7a9f3739a51980efa84c4` |
| `qwen1p5b-r4-decode-sweep.html` | 16 | `91c976256f286199d2de5934292ab69cbd066e8780346aa135462689706b60b2` |

对齐层级：① `source stream SHA256` / `summary SHA256`（最强）→ ② `kernels/APIs/allocation/历史阶段` → ③ `Write 误差 + 逐步通过`。

> 注意：HTML 自带的"逐步通过"列显示**只有 P32/D2 是 2/2**；P64 以上及 D4/D8/D16 均记为不通过。
> 「与 HTML 数据对齐」= 复现这些数值（**含失败项**），不是要求全部通过。

对应实现位于 tilegen 的 `codex/ada-r4-latest-20260924` 分支（tip `90a66809`，14 提交 / 500 文件），
底座 `35c1367` 已在 tilegen 仓库中（`import/ada-r4-original-35c1367`）。

## 8. 校验本包

```bash
# 逐文件核对（Linux/macOS 通用）
while IFS=$'\t' read -r p b h; do
  [ "$p" = path ] && continue
  got=$(sha256sum "$p" | cut -d' ' -f1)
  [ "$got" = "$h" ] || echo "MISMATCH $p"
done < MANIFEST.tsv
```
