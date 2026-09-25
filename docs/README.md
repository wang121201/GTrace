# docs/ — 设计说明索引

本目录只保留**当前有效**的设计与边界说明。原先按迭代轮次累积、内容互相重叠的文档
已在 2026-09-25 按主题合并（正文逐字保留），合并前后的对应关系见下表。

## 目录

| 文档 | 主题 | 状态 |
|---|---|---|
| [ada-calibration.md](ada-calibration.md) | Ada 缓存配置与内部参考时延：tuner-v1 历史档、r2 校准基线（默认）、r3 FIFO 实验、r2 时延契约 | 当前 |
| [ada-fine-timing-gaps.md](ada-fine-timing-gaps.md) | fine 时序接口已支持什么、仍缺什么，以及 sector + lazy L2 的最小改动边界 | 当前 |
| [ada-r4-serial.md](ada-r4-serial.md) | r4 冻结串行读候选：配置表、输入格式、回放二进制接口、与 NCU 的比较口径 | 当前 |
| [native-p1024d32.md](native-p1024d32.md) | P1024/D32 准入结论、实现状态、逐 family 地址绑定方案、host argument 采集合同 | 当前 |
| [native-trace.md](native-trace.md) | Native/direct binary request trace v1 的编码与 API | 当前 |
| [replay-modes.md](replay-modes.md) | direct trace 的三种回放档：纯访存、阶段重叠、多后端（GDDR6/HBM/HBF） | 当前 |
| [shared-frontend.md](shared-frontend.md) | 快速地址生成与精确 cosimulation 的共用前端、验证与性能口径 | 当前 |
| [qualification.md](qualification.md) | 融合分支的实测运行时间与流量、宿主初始化优化、正确性与发布范围 | 当前 |
| [figures/](figures/) | 文档用图（`stage-overlap.svg`） | 当前 |

## 合并记录（2026-09-25）

| 新文档 | 吸收的原文 |
|---|---|
| `ada-calibration.md` | `ada-calibration.md`、`ada-calibration.md`、`ada-calibration.md` |
| `ada-r4-serial.md` | 原 `ada-r4-serial.md`、`ada-r4-serial.md` |
| `native-p1024d32.md` | `native-p1024d32.md`、`native-p1024d32.md`、`native-p1024d32.md`、`native-p1024d32.md` |
| `replay-modes.md` | `replay-modes.md`、`replay-modes.md`、`replay-modes.md` |

合并规则：

1. 原文正文**逐字保留**，只把每份文档开头各自重复一遍的"本配置未校准 / 非周期等价 /
   没有 NCU 精度验收"类状态声明提到合并文档开头的**共用口径**一节，不再逐节重复。
2. 每份原文的一级标题降级为合并文档的二级标题，便于跳转定位。

`validation/` 下与被合并文档同名的 JSON 收据**保持原名不变**，它们是判等基准，不随
文档改名。

## 其他入口

- 构建、运行与复跑命令：[README.md](../README.md)
- 分档 smoke 测试：[SMOKE.md](../SMOKE.md)
- 阶段性历史记录：[HISTORY.md](../HISTORY.md)
