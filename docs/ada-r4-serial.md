# r4：显式串行读缓存候选与回放入口

本页合并原先两份 r4 说明：`ada-r4-serial.md`（冻结配置、输入格式、与 NCU 的比较
口径）与 `ada-r4-serial.md`（共享 L1 回放二进制接口与远端原地比较）。两者
描述的是同一次冻结交付的配置面与执行面，正文按原样保留在下面各节。

## 共用口径

- r4 的接入范围是 **L1 过滤后进入 L2 的 32 B 读 sector 数量**。没有新设定 GPU
  延迟、L2 写回、SM 调度、MSHR 或 HBFSIM 参数。
- `ada_profile.py` 默认仍为 `r2-adaptive`；历史 `run.py` direct/cosim 入口保持原有
  配置。r4 **不会**被静默应用到旧 LLM 或带宽结果。
- 这是单标量 4 B 读流：**不含写、原子、并发多 warp、DRAM 服务或 GPU 时钟**。
- 输出中的 CPU/wall 秒数是宿主耗时，不能当作 GPU 时长或带宽。
- 参数是对串行读校准得到的**有效**参数，不代表已发现硬件的物理组数或容量。

<!-- 以下正文合并自：ada-r4-serial.md ada-r4-serial.md -->

## r4：显式串行读缓存候选


本分支将冻结的 `CLOCK_u128_s16_h2_c1062` 接入 GTSim 的共享 `PerSmL1Cache`。新入口 `ada_r4.py` 与 r2 LRU 基线使用同一缓存实现、同一输入和同一同步完成方式。r4 参考 C++ 源码仅用于来源记录，不参与 GTSim 二进制编译。

这次接入范围是 **L1 过滤后进入 L2 的 32 B 读 sector 数量**。没有新设定 GPU 延迟、L2 写回、SM 调度、MSHR 或 HBFSIM 参数。`ada_profile.py` 默认仍为 `r2-adaptive`，历史 `run.py` direct/cosim 入口保持原有配置。r4 不会被静默应用到旧 LLM 或带宽结果。

### 冻结配置

| 实测 shared carveout | r2 名义 L1 | r2 sets × ways | r4 有效 L1 | r4 sets × ways |
|---|---:|---:|---:|---:|
| 32 KiB | 96 KiB | 4 × 192 | 100 KiB | 16 × 50 |
| 64 KiB | 64 KiB | 4 × 128 | 66 KiB | 16 × 33 |
| 100 KiB | 28 KiB | 4 × 56 | 28 KiB | 16 × 14 |

128 B allocation unit，32 B sector；r4 使用 CLOCK 与冻结 hash2，容量按 `floor(floor(nominal × 1062 / 1000) / (128 × 16))` 个 way 取整。这些是针对串行读校准的有效参数，不代表已发现硬件的物理组数或容量。

源码和参数原件在 `configs/rtx4000-ada-r4-serial/source/`，wrapper 核验固定 SHA，并核对编译后六个 profile 的实际参数。`source/.../include/ada_r4_profile.h` 提供显式构造函数。

### 输入与执行

在 XMU 数据所在机器运行：

```sh
python3 ada_r4.py \
  --input /home/xmu/nvidiagds/codex-runs/rtx4000-ada-cache-calibration-20260922-r4/analysis/evaluation_cases.tsv \
  --output /absolute/new/run-directory --compiler g++
```

无表头的 manifest 每行五列：`case_id nominal_l1_bytes cg expected_requests request_path`。文件只含 little-endian uint32 allocation-relative byte offsets；这是单标量 4 B 读流，不具备写、原子或并发语义。每个 case 冷启动，每次 miss 立即完成相应 sector，CG 逐请求旁路。

地址偏移已包含 kernel 布局 offset，应相对最初完整 allocation 的起点。不能再减 kernel 参数指针或增加真实 VA。共享 L1 的新 hash 接口要求显式 relative line offset，同时保留全局 tag、allocation 身份和 fill ticket；缺失相对偏移会拒绝执行。

`predictions.jsonl` 输出每个 case/model 的派生计数和 CPU/elapsed 时间；`receipt.json` 记录编译、源码、二进制及输入 SHA。原始请求不随结果复制。wrapper 成功仅表示完整执行，硬件精度由独立 evaluator 判定。

### 与真实 NCU 比较

主比较使用 360 个新 CA 条件，每条件 5 次 NCU 中位数，指标为 `lts__t_sectors_srcunit_tex_op_read.sum`：

`WAPE = sum(abs(model_misses − NCU_sectors)) / sum(NCU_sectors)`。

其余 12 个 CG 控制和 12 个跨时段 anchor 分开报告。不能将 384 个条件混为同一评分，也不能只比较累计总量让正负误差抵消。需要逐条件确认 GTSim hits/misses 与冻结参考相等，并按实测 shared、stride、访问顺序分组。

r4 冻结原报告的总体 WAPE 为 12.01%，低于 r2 的 21.53%，但“总体和所有子组均 ≤10%”的联合目标未通过。旧 r3 的 288 条件本轮已参与开发，不再是独立留出集。本次不根据新测试数据重新挑选参数。

此比较不能推断 DRAM write 误差、LLM decode 写流量、带宽或 compute/memory overlap 已经改善。相关验证须使用完整程序与相应缓存状态，另行进行。

## r4 shared-cache串行回放入口


`source/ada_r4_serial_replay.cpp`直接调用共享 `PerSmL1Cache`，没有复制冻结参考模型的替换或hash算法。每case冷启动，单SM、32 B sector、同步fill，仅全局标量读取；`.cg`逐请求旁路。不包含写、DRAM服务、GPU时钟、多warp并发或LLM推理。

固定两个模型：`LRU_u128_s4_h0_c1000` 与 `CLOCK_u128_s16_h2_c1062`。前者为r2串行参考（4sets、LRU）；后者为冻结r4候选（16sets、CLOCK、hash2），三档分别50/33/14ways。参数由共享profile helper提供，`--describe`从实际配置输出六组capacity/sets/ways/replacement/hash供外层固定版本校验。模型不是实际GPU缓存结构的声明。

### 接口

```text
ada_r4_serial_replay --describe
ada_r4_serial_replay --self-test
ada_r4_serial_replay CASES_TSV
```

CASES_TSV无表头，每行五列：`case_id nominal_l1_bytes cg expected_requests request_path`。nominal只允许98304/65536/28672，分别映射shared32768/65536/102400；实际观测值还必须由后续NCU join核验。request文件是little-endian `.u32` allocation-relative字节offset。driver只从XMU现有文件读取，不导出请求。

文件大小必须严格等于expected_requests×4，offset需4B对齐；空请求、空cases、重复case、非法列、短文件或附加内容拒绝。cache请求tag为offset向下128B对齐值，allocation-relative索引字段也是该对齐值，sector取原offset模128。每次miss立即完成共享cache发出的ticket，再处理下一条。

每case输出两条JSONL，`schema=ADA_R4_SHARED_SERIAL_CASE_V1`、`status=PASS`。包含case ID、固定模型参数、hits/misses/requests、L2 read sectors、CPU/wall秒数。CPU/wall包含该case冷cache构造和回放，不包含请求文件读取；它们是宿主耗时，不能当GPU时长/带宽。输出及错误中不包含地址、输入路径或请求内容。异常退出的已完成前缀不能当作完整384条件验收。

### 远端原地比较

```text
python3 -B tools/evaluate_ada_r4.py \
  --replay-jsonl REPLAY_JSONL \
  --replay-receipt WRAPPER_RECEIPT_JSON \
  --hardware HARDWARE_AGGREGATES_JSON \
  --frozen FROZEN_AGGREGATES_JSON \
  --output FRESH_OUTPUT_DIRECTORY
```

比较器必须读取同次wrapper的receipt：`PASS_PROCESS_ONLY`、returncode=0、case_count=384、source/input_unchanged均为true、GPU_executed=false，且prediction_sha256与当前JSONL逐字SHA一致；receipt自身SHA也纳入输入pins。预测文件完整但wrapper失败时仍拒绝，不把完整前缀或变化后的输入当作闭合结果。

比较器只在XMU读取原硬件aggregate和冻结预测，不读取原请求。完整性要求为384 cases×两模型、每case五次NCU、实测shared五次一致且与参数相符、source请求数守恒。每条预测和冻结结果核对hits/misses/capacity；不一致时输出 `FAIL_REFERENCE_EQUIVALENCE`，不通过调整参数消除差异。

`summary.json`仅包含非敏感聚合、SHA和CPU/wall；标准输出也是该聚合。`case-derived.jsonl`保存派生计数与误差，留在远端。fresh0..359单独报告overall、shared、stride y、order z、read-group-stability；`.cg`360..371和跨时段锚点372..383另列。WAPE使用绝对误差求和，有符号总量误差另列，禁止正负相抵伪装成WAPE。分母为0时百分比是null。保留不稳定条件，没有参数再选择。

NCU分母为L2全局读sector五次中位数；不能将其乘32B冒充实测DRAM字节，也不能外推写回或时序精度。reference一致仅证明移植行为匹配，不代表模型已满足硬件误差目标。

### 本地合成测试

driver `--self-test`44项覆盖计数、sector部分命中、cold重建、`.cg`、六组静态容量、空/短/坏长度/非法协议；`tests/test_evaluate_ada_r4.py`十二项覆盖receipt逐字段门禁、完整分母、重复/缺失、观测错配、source数量、冻结预测不符以及WAPE与有符号误差区别。它们没有使用或生成正式硬件结果。
