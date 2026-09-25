# 在 Linux/xmu 上重建并重跑 GDDR P2（17.10 Gb/s）结果

目标：用 `hbserve-memgen-gtsim-alignment/` 归档里的冻结代码与输入，在 xmu 上重建
`fixture` 并重跑，检查能否与 `outputs/ada-gddr-p2-rerun-r1/result-r1/dashboard.html`
（及其 `data.json`）的数据对齐。

结论见文末「结果」一节。以下记录做法与所有必须的偏离。

---

## 1. 工具链：必须用 g++ 11

| 编译器 | 结果 |
|---|---|
| `clang++` / `clang++-14` / `clang++-18` | **`<array>` 等 libstdc++ 头找不到** |
| `g++` 11.4.0 | 直接可用 |

原因：主机上 clang 默认按 GCC 12 的路径找 libstdc++，但系统只装了
`libstdc++-11-dev`（`/usr/include/c++/11`）。绕法有两种：

- `clang++-14 -isystem /usr/include/c++/11 -isystem /usr/include/x86_64-linux-gnu/c++/11`
- 或仓库里已有的 shim `~/nvidiagds/tools/clang++-gcc11`

选 **g++ 11.4**：仓库自己的 `codex/clang-linux-compat` 提交（`dc6b84a`）注明
「For comparison, g++ builds the same tree unchanged」——即 g++ 不需要为 clang 打的三处补丁：

| 位置 | clang 报错 | g++ |
|---|---|---|
| `physical/address_heatmap.cpp:942` | 结构化绑定不能进 lambda 捕获列表 | 接受 |
| `physical/hbm/hbm_device.cpp:1950` | `optional::emplace()` 对聚合体 `is_constructible` 为 false | 接受 |
| `tests/replay_phase_report_test.cpp:36` | 同上（测试，不在本链接集） | 接受 |

**所以用 g++ 时这三处都不需要改**，源码保真度更高。

---

## 2. 关键障碍：冻结的 macOS 绝对路径（30234 处 / 49 个文件）

归档里的源码与**输入数据**都写死了 macOS 绝对路径
`/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/...`。

两条路：

1. **改写字节**（把前缀替换成 Linux 路径）——**不可行**。
   `fixture` 会对 pin 住的输入做 `exact bytes/SHA` 校验：
   ```
   history loader: exact bytes/SHA .../current-history-registry-r3/assembled-r1/dispatch.json
   ```
   任何改写都会让 pin 失配（实测：改写后立刻被拒）。

2. **让原路径真的存在**（`README.md §6` 的推荐做法）。没有免密 sudo，但主机启用了
   非特权 user namespace，且装有 `bubblewrap`。于是用 bwrap 把重建出来的工作区
   绑到冻结路径上：

```bash
MAC=/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment
bwrap --tmpfs / \
  --dev-bind /usr /usr --dev-bind /lib /lib --dev-bind /lib64 /lib64 \
  --dev-bind /bin /bin --dev-bind /sbin /sbin --dev-bind /etc /etc \
  --dev-bind /home /home --dev-bind /var /var --proc /proc --dev /dev \
  --tmpfs /tmp \
  --dir /Users --dir /Users/wgs --dir /Users/wgs/Documents \
  --dir /Users/wgs/Documents/Codex --dir /Users/wgs/Documents/Codex/2026-09-14 \
  --bind <mirror> "$MAC" \
  --chdir <build> -- "$@"
```

`--tmpfs /` 是因为 `/` 属 root，bwrap 无法在真实根下建 `/Users`。

**收益**：`mirror/` 就是原 macOS 工作区的布局；所有冻结路径原样解析，
**没有任何文件为了路径而改一个字节**。

---

## 3. 两个 VFS overlay

原构建另加 `-ivfsoverlay`（只重定向 3 个头文件）。g++ 没有 VFS overlay，
于是把 3 处重定向**实体化**（对 `type: file` 的本地文件重定向完全等价）：

| 虚拟路径（被替换） | 实际内容 |
|---|---|
| `.../core-native-copy-r2/include/cache_geometry.h` | `structure-adapter/cache_geometry.h` |
| `.../core-native-copy-r2/include/ada_address_mapping.h` | `structure-adapter/ada_address_mapping.h`（原盘上不存在，靠 overlay 注入） |
| `.../core-native-copy-r2/include/writer_observer.h` | `current-history-write-attribution-r1/writer_observer.h` |

这两处替换是**语义必需**的：`structure-adapter` 加入
`ACCELSIM_RTX4000_ADA_SET_ASSOCIATIVE` 与 Ada 地址映射；`writer_observer` 是写归因
版本。快照里同名文件是上游版本，**不能**直接用。

---

## 4. 唯一一处源码补丁

```diff
- {"before",statistics(r.before)},{"after",statistics(r.after)}
+ {"before",p28::statistics(r.before)},{"after",p28::statistics(r.after)}
```
文件：`.../snapshot/source/work/tilegen-full-r1/driver-prefill-gemm-next-r1/streaming.cpp`

理由：Apple clang 21 接受非限定名 `statistics`，GCC/clang 在 Linux 上报
`'statistics' was not declared in this scope; did you mean 'p28::statistics'?`。
**仓库自己的 `codex/clang-linux-compat` 提交 `5211abc` 就是这一处改动**，语义不变
（该文件在归档里与 `codex/ada-r4-latest-20260924` 逐字节相同）。

---

## 5. 构建

22 个 TU 中，`canonical-full-runtime-r4/streaming.cpp`（index 00）由链接单元
`runtime-r2/history_runtime.cpp` 文本包含，因此只单独编译 **21 个 TU**：

```bash
FLAGS="-std=c++20 -O3 -Wall -Wextra -pthread -Wno-unused-parameter
  -DTILEGEN_DIRTY_SECTOR_MODE=2 -DTILEGEN_SOURCE_MEMORY_SEMANTICS=1
  -DTILEGEN_EF_INSERTION_COUNTERFACTUAL=1
  -I$MAC/work/unified-cache-cosim-r1/.../candidate/snapshot/source/work/...   (6 个)"
# 21 个 TU -> obj/NN.o  (并行 12)
# 链接： g++ $FLAGS $MAC/work/ada-cosim-alignment-20260922-r1/runtime-r2/history_runtime.cpp \
#            obj/01.o ... obj/21.o -lz -o fixture
```

原始 `-mcpu=native` 与 `-flto=thin` 未使用（x86 上语义不同 / 无必要）。

---

## 6. 归档 `inputs/` 不完整

`MANIFEST.tsv` 声明的 22 个运行输入**不足以**让 loader 跑起来。通过
`strace -e trace=%file` 逐个发现 loader 实际要读的 pinned 文件，缺少的有：

```
current-controls-execution-r1/prepared-r1/calls.json
current-controls-execution-r1/prepared-r1/expected.json
current-controls-execution-r1/prepared-r1/source-models.json
composite-input-r1/composed-r4/timeline.jsonl          (4.1 MB)
current-history-execution-r1/prepared-r1/input.json
current-history-loader-r1/prepared-r1/plan.json
work/tilegen-full-r1/canonical-*-model-r1/**           (15 个模型目录)
以及各 driver 的 sealed 来源清单（含 compute-norm-metadata-r1/*.py 等）
```

最终做法：扫描重建树里出现的**全部**绝对路径（2159 条），把其中在本机存在的
**2152 个文件 + 6 个目录**一次性 rsync 进 mirror（**1.3 GB**）。这样 mirror 成为一个
完整、自洽、字节原样的运行环境。

> 复现要点：`inputs/` 只是「最小运行集」的近似，**不是**完整运行输入集；
> 真正完整的是 mirror 里的那份。缺的那部分按上面的清单从原工作区取回即可。

---

## 7. 运行

```bash
./rs ./fixture $MAC/work/ada-cosim-alignment-20260922-r1/gddr-full-p2-r1/plan.json <out>/result.json
```
`plan.json` **原样使用**，sha256 = `4c34da34eae1e1ed90d7e22fd9836ec12f68766863e103913ef666f5fb49a3cf`
（与文档 pin 一致）。argv 只有 3 项，无开关。

主机：xmu（48 核，Ubuntu 22.04，g++ 11.4）。宿主耗时见「结果」。

---

## 8. 结果：与文档完全一致

**结论：`dashboard.html` 的数据在 Linux/xmu 上完整复现，且与原始 macOS 运行逐位一致。**

### 8.1 文档锚点（全部一致）

| 锚点 | 值 |
|---|---|
| `status` | `PASS_SELECTED_CONTINUOUS_CURRENT_HISTORY` |
| `counts` | `native_kernel=1138`, `memory_api_submission=29`, `epoch_begin=3`, `epoch_end=3` |
| `rows` | `1173` |
| `CTAs` / `nodes` | `772512` / `4712768255` |
| `cycles` | `826832904` |
| `HBFSIM.physical` | `read_bytes=45241836416`, `write_bytes=146843072`, `finish_ns=380129130.2923976` |

### 8.2 文档表格（阶段时间与有效带宽）逐位一致

| 阶段 | Linux 读 | Linux 写 | Linux 时间 | Linux 带宽 | 文档 |
|---|---:|---:|---:|---:|---|
| Prefill | 15.25 GB | 146.80 MB | 141.39 ms | 108.90 GB/s | 同 |
| Decode1 | 15.00 GB | 9.98 KB | 119.37 ms | 125.66 GB/s | 同 |
| Decode2 | 14.99 GB | 31.23 KB | 119.39 ms | 125.56 GB/s | 同 |
| Full | 45.24 GB | 146.84 MB | 380.15 ms | 119.40 GB/s | 同 |

精确到最后一个整数：

| 阶段 | read_bytes | write_bytes | cycles |
|---|---|---|---|
| Prefill | 15251476992 | 146801856 | 307528284 |
| Decode1 | 15000056960 | 9984 | 259639595 |
| Decode2 | 14990302464 | 31232 | 259665025 |
| Full | 45241836416 | 146843072 | 826832904 |

四行的 read/write/cycles 与归档的 macOS 原始 `result.json` **完全相同**。

### 8.3 全量逐字段比对（vs 归档原始 `result.json`）

| 项 | 数 |
|---|---:|
| 可比较叶子总数 | 175994 |
| **完全相同** | **173322** |
| 仅宿主墙钟耗时不同 | 2311 |
| 仅编译器 ABI 结构体尺寸/偏移不同（诊断字段） | 358 |
| 仅宿主总耗时刻度不同（`host_execution/preparation/finalization_seconds`） | 3 |
| **模型相关差异** | **0** |
| 只在一侧存在的键 | 0 |

即：除了「机器快慢」和「编译器结构体布局」这两类纯诊断量，**结果完全一致**。

### 8.4 宿主成本（与文档的差异属于机器差异，不是模型差异）

| 项 | macOS 原始 | Linux/xmu |
|---|---:|---:|
| 宿主执行 | 4282.49 s | 8654.73 s |
| 墙钟总计 | 4284.31 s（71.41 min） | **8657.16 s（144.29 min）** |

xmu 慢约 2.0 倍，原因已定位：该机 Xeon Silver 4410Y **固定在 2000 MHz**
（`/proc/cpuinfo` 全为 2000.000，无 `cpufreq` sysfs、无 turbo）。单线程仿真，
所以这是频率差异，不影响任何模型量。

### 8.5 保留的偏离（完整清单）

1. 编译器：`g++ 11.4`（不是 clang）。原因见 §1；g++ 不需要 clang 的三处补丁。
2. 三处 VFS overlay 用**实体化**代替 `-ivfsoverlay`（§3），与原构建等价。
3. 一行源码补丁 `p28::statistics`（§4），与仓库自身提交 `5211abc` 相同。
4. `-mcpu=native`、`-flto=thin` 未使用。
5. 输入路径由 bwrap 绑定挂载解析，**未改任何字节**；`plan.json` sha256 与 pin 一致。

除此之外，编译闭包、overlay 外部文件、全部 pin 住的运行输入都是原字节。

### 8.6 产物

| 文件 | 说明 |
|---|---|
| `build-r1/out/result.json` | Linux 重跑结果（6.99 MB） |
| `build-r1/out/result.json.operations.jsonl` | 逐操作日志 |
| `build-r1/compare.log` | 逐字段比对报告 |
| `build-r1/build-receipt.json` | 构建凭据（flags/overlay/补丁/二进制 sha256） |
| `receipts/linux-rerun/LINUX-RERUN-RESULT.json` | 本次验证的机器可读收据 |
| `receipts/linux-rerun/mirror-manifest.tsv` | 重建树内每个文件的 path/bytes/sha256 |
| `build-r1/mirror/` | 完整可重跑环境（1.3 GB，含全部 pin 输入原字节） |
