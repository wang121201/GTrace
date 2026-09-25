# receipts/ — 布局说明

本目录是 GDDR P2 隔离重跑的**结果收据**。为避免同一份大文件出现两遍，约定如下：

## 规范路径

| 内容 | 规范位置 |
|---|---|
| 精确数据与来源（**判等基准**） | `result-r1/data.json` |
| 闭合分析收据 | `result-r1/receipt.json` |
| 原始文字报告 | `result-r1/report.md` |
| 仪表盘（HTML） | `result-r1/index.html`、`result-r1/dashboard.html` |
| 仪表盘生成收据 | `result-r1/dashboard-receipt.json` |

`result-r1/` 是 `control/build_dashboard.py` 认定的产物目录，其内容与原始
工作目录 `outputs/ada-gddr-p2-rerun-r1/result-r1/` 逐字节一致：

```
data.json     sha256 58eabb56d9f20c9bf6f49589ccaa209ac4468d326ec66fb0618f76b2a6031e19
```

> 2026-09-25 瘦身：原先在 `receipts/` 平铺层还各有一份 `data.json` /
> `receipt.json` / `report.md` 的副本，经确认与 `result-r1/` 下同名文件**逐字节相同**、
> 且没有任何脚本或清单引用平铺路径，故删除。判等请一律使用上表规范路径。

## 清单文件

| 文件 | 作用 |
|---|---|
| `pins-manifest.json` | 1372 项来源 pin（指向 `../work/...` 实际输入） |
| `minimal-manifest.json` | 最小传输包清单 |
| `acceptance-prefill-sweep.json` | P64/P256/P512 prefill sweep 验收表（16 张） |
| `acceptance-decode-sweep.json` | P128 D2/D4/D8/D16 decode sweep 验收表（15 张） |

两份 `acceptance-*.json` 是重跑对账的唯一判等基准；数值判等方法与 smoke 测试入口
见仓库根 [`SMOKE.md`](../SMOKE.md) 和 [`docs/ada-r4-reproduction.md`](../docs/ada-r4-reproduction.md)。
