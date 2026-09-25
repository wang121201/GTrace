# 历史记录

本文件收录从 [README.md](README.md) 移出的、**按时间顺序累积的阶段性记录**。

这些段落描述的是当时的状态与口径，用于追溯演进过程；**它们不作为当前验收依据**。
当前可执行口径见 [README.md](README.md)，设计说明见 [docs/README.md](docs/README.md)。

---

## 阶段性实现记录

最新衔接：GEMV、SiLU 的 direct binding 与精确 cosim Builder 已共用 cache 前 `PreparedMemory`，并减少等价 CTA 校验的主机分配。三组配对测试中引擎执行窗口为 **1.08×**，完整子进程 CPU 时间基本持平；不能据此声称端到端明显提速。实现、验收和范围见 [共享前端报告](docs/shared-frontend.md)。

最新增加：已有 direct trace 可通过独立 `replay.py` 直接回放到 HBFSIM，跳过计算和 GPU stall，保留内存队列/时序。Decode 有界子集的 591,652 条请求回放 CPU **0.022 分钟**；完整生成与回放合计约 **0.440 分钟**，与保留计算依赖的 cosim 口径不同。见 [纯访存回放说明](docs/replay-modes.md)。

现在可在这条 direct 路径导出 CTA 组计算 profile，再用 `replay.py --mode stage-overlap` 执行阶段级计算/访存重叠。地址流不变，计算成本取原 SourceNode 的静态资源需求，运行时跳过 warp/DAG 调度；窗口、计算尾部、kernel 屏障及近似范围见 [阶段重叠说明](docs/replay-modes.md)。自动 profile 当前限九类原生 binding，不覆盖完整 1138-call 流程。

阶段回放现在支持配置独立的 GDDR6、HBM 和原生 HBF controller/NAND 后端，并输出阶段、kernel、CTA 组的读写与带宽。HBF 的请求字节、4 KB 页介质流量、控制器 HBM 流量和最终持久化尾部独立计数；它们不能混为同一个带宽。见 [多后端阶段报告](docs/replay-modes.md)。

`ada_profile.py` 新增 `accelsim-rtx4000-ada-v1`：将 XMU tuner 的 SM 资源、缓存几何与
地址映射明确导入，并提供 32 B sector 功能缓存回放。该配置后来成为 tuner-v1 历史档，
当前默认已改为 r2 校准基线，配置详情合并见
[Ada 配置与内部参考时延](docs/ada-calibration.md)。

---

## 初次融合验收（`ad8afa3` 集成基线）

最新源码的独立原公式检查、CTA 校验负测、20-family trace 精确回归及配对性能结果见
[共享前端报告](docs/shared-frontend.md) 和 `validation/shared-frontend.json`。

- 32 B：15 种 dirty mask、跨行/重复/部分写、6 种容量、step/epoch1/4/8 背压；ASan/UBSan 三模式共 59,321 项检查，每 tick 核验 dirty-sector/byte 守恒。
- Trace：18,981 项检查；后端开关前后完成序列、周期、读写及物理统计一致，覆盖重试、损坏、截断、配额和文件发布。
- 9 类 binding 对原 Builder：19,816 条访存指令、542,588 个地址范围，方向、bypass、matrix、subop、lane、地址和宽度精确一致。
- 20 类 kernel 的实际运行、decode 子集性能和同二进制 trace 开关比较，见 `validation/qualification.json`、`docs/qualification.md`。

上述为代表性来源和有界 CTA 验证，未重跑 1138 个完整网格；不把 direct/cosim 流量相等列作验收条件。格式与地址含义见 `docs/native-trace.md`。
