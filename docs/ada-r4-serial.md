# r4：显式串行读缓存候选

本分支将冻结的 `CLOCK_u128_s16_h2_c1062` 接入 GTSim 的共享 `PerSmL1Cache`。新入口 `ada_r4.py` 与 r2 LRU 基线使用同一缓存实现、同一输入和同一同步完成方式。r4 参考 C++ 源码仅用于来源记录，不参与 GTSim 二进制编译。

这次接入范围是 **L1 过滤后进入 L2 的 32 B 读 sector 数量**。没有新设定 GPU 延迟、L2 写回、SM 调度、MSHR 或 HBFSIM 参数。`ada_profile.py` 默认仍为 `r2-adaptive`，历史 `run.py` direct/cosim 入口保持原有配置。r4 不会被静默应用到旧 LLM 或带宽结果。

## 冻结配置

| 实测 shared carveout | r2 名义 L1 | r2 sets × ways | r4 有效 L1 | r4 sets × ways |
|---|---:|---:|---:|---:|
| 32 KiB | 96 KiB | 4 × 192 | 100 KiB | 16 × 50 |
| 64 KiB | 64 KiB | 4 × 128 | 66 KiB | 16 × 33 |
| 100 KiB | 28 KiB | 4 × 56 | 28 KiB | 16 × 14 |

128 B allocation unit，32 B sector；r4 使用 CLOCK 与冻结 hash2，容量按 `floor(floor(nominal × 1062 / 1000) / (128 × 16))` 个 way 取整。这些是针对串行读校准的有效参数，不代表已发现硬件的物理组数或容量。

源码和参数原件在 `configs/rtx4000-ada-r4-serial/source/`，wrapper 核验固定 SHA，并核对编译后六个 profile 的实际参数。`source/.../include/ada_r4_profile.h` 提供显式构造函数。

## 输入与执行

在 XMU 数据所在机器运行：

```sh
python3 ada_r4.py \
  --input /home/xmu/nvidiagds/codex-runs/rtx4000-ada-cache-calibration-20260922-r4/analysis/evaluation_cases.tsv \
  --output /absolute/new/run-directory --compiler g++
```

无表头的 manifest 每行五列：`case_id nominal_l1_bytes cg expected_requests request_path`。文件只含 little-endian uint32 allocation-relative byte offsets；这是单标量 4 B 读流，不具备写、原子或并发语义。每个 case 冷启动，每次 miss 立即完成相应 sector，CG 逐请求旁路。

地址偏移已包含 kernel 布局 offset，应相对最初完整 allocation 的起点。不能再减 kernel 参数指针或增加真实 VA。共享 L1 的新 hash 接口要求显式 relative line offset，同时保留全局 tag、allocation 身份和 fill ticket；缺失相对偏移会拒绝执行。

`predictions.jsonl` 输出每个 case/model 的派生计数和 CPU/elapsed 时间；`receipt.json` 记录编译、源码、二进制及输入 SHA。原始请求不随结果复制。wrapper 成功仅表示完整执行，硬件精度由独立 evaluator 判定。

## 与真实 NCU 比较

主比较使用 360 个新 CA 条件，每条件 5 次 NCU 中位数，指标为 `lts__t_sectors_srcunit_tex_op_read.sum`：

`WAPE = sum(abs(model_misses − NCU_sectors)) / sum(NCU_sectors)`。

其余 12 个 CG 控制和 12 个跨时段 anchor 分开报告。不能将 384 个条件混为同一评分，也不能只比较累计总量让正负误差抵消。需要逐条件确认 GTSim hits/misses 与冻结参考相等，并按实测 shared、stride、访问顺序分组。

r4 冻结原报告的总体 WAPE 为 12.01%，低于 r2 的 21.53%，但“总体和所有子组均 ≤10%”的联合目标未通过。旧 r3 的 288 条件本轮已参与开发，不再是独立留出集。本次不根据新测试数据重新挑选参数。

此比较不能推断 DRAM write 误差、LLM decode 写流量、带宽或 compute/memory overlap 已经改善。相关验证须使用完整程序与相应缓存状态，另行进行。
