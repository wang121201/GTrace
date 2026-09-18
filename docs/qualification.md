# 融合分支验证结果

分支 `codex/tilegen-trace-cosim-20260918-r1`；B1，32 B 写回；B8 未合并。

## 实测运行时间与流量

运行平台：`macOS-26.5.1-arm64-arm-64bit`，CPU 执行，无新 GPU/NCU 采样。
CPU 为子进程 user + system；elapsed 为启动至退出，包含输入传输、模拟、trace 写入和读回；不含编译、预先准备输入及 wrapper 的额外结果核验。每项为一次实测，共享主机。

| 测试 | CPU 分钟 | elapsed 分钟 | DRAM Read | DRAM Write |
|---|---:|---:|---:|---:|
| families-direct | 0.79 | 0.80 | 8.74 MB | 0.00 MB |
| families-cosim-off | 1.24 | 1.25 | 8.74 MB | 0.00 MB |
| families-cosim-on | 1.20 | 1.20 | 8.74 MB | 0.00 MB |
| decode-direct | 0.42 | 0.42 | 75.60 MB | 0.03 MB |
| decode-cosim | 0.81 | 0.82 | 75.60 MB | 0.00 MB（4,864 B） |
| decode-cosim-fast | 0.48 | 0.48 | 75.60 MB | 0.00 MB（4,864 B） |
| p28-prefix-one | 0.45 | 0.45 | 1.32 MB | 0.00 MB |

- `families`：20 类 kernel 各取第一个原生来源，CTA prefix=1，共 20 CTA、303,449 个模型节点。
- `decode`：Decode2 的原生 launch 28/29/30（GEMV → SiLU → GEMV），prefix=512，共 1,025 CTA、9,733,656 个模型节点；不是一次完整 token 或整个模型。
- `p28-prefix-one`：单独 QKV 来源、prefix=1，验证预验证 full-grid 模型不会被执行前缀误复用。
- 同一 decode 子集，direct/默认 cosim 的 CPU 用时比为 **1.94×**；两者执行口径不同，不能将其视为等时序模拟加速。

该 decode 子集：direct 新增 1,152 个 dirty sector，驱逐 1,060 个，驻留 92 个；native 新增 1,152 个，驱逐 152 个，驻留 1,000 个。分别写回 33,920 B 和 4,864 B，均为每 sector 32 B。固定功能顺序改变了驱逐时机；未做最终 flush，两路均满足 dirty-sector 与字节守恒，驻留脏数据不能算作丢失写入。

## 保留模拟结果的宿主初始化优化

八帧仍解压并核验声明的 SHA；只为选中的 family 构建 typed 预验证对象，并与正式执行对象分开持有。
优化前后二进制逐项对比 workflow、每 kernel execution（只排除宿主秒数）、dirty ledger、HBFSIM/backend 统计均相同。

| 子集 / 档位 | 优化前 CPU 分钟 | 优化后 CPU 分钟 | CPU 加速 |
|---|---:|---:|---:|
| 20 类 / cosim | 1.19 | 1.24 | 0.96× |
| decode / cosim | 1.25 | 0.81 | 1.53× |
| decode / cosim-fast | 0.90 | 0.48 | 1.87× |

完整 1138-workflow 包含所有 family，仍需全部 typed 预验证；这里的初始化收益不能外推为完整运行加速。

## 正确性与发布范围

- 同一最终二进制的 20-family cosim，trace 开关前后的完整每 kernel 模拟结果、周期、访存计数和 backend 统计一致；导出数量、字节与后端闭合。
- 32 B 写回覆盖全部 15 种非零 dirty mask、部分/跨行/重复写、有限背压及 epoch 推进；三模式 ASan/UBSan 共 59,321 项检查通过。
- Trace 格式及真实后端 on/off 18,981 项检查通过；损坏、截断、配额和重试均有测试。
- 9 类 fast binding 与原 Builder 精确比较 19,816 条访存指令、542,588 个地址范围；其余 11 类直接调用原 Builder。
- direct 使用固定功能顺序，无 compute 调度、MSHR 合并或 HBFSIM；两种模式的 DRAM 流量不应被强制拟合相等。cosim-fast 为显式近似时序档，默认 cosim 保留原生依赖。
- 全 1138 个完整网格未重跑；未证明新版本完整工作流耗时或新的 NCU 硬件误差。源码和 Git 独立，封存运行数据仍为原目录只读依赖；本报告未声称已部署 XMU。

最终二进制 SHA-256：`ef7150b433fe815e21921477fd9686fedf33ca50b289451b9539b6a83ab3e6bb`。

机器可读结果：`validation/qualification.json`；逐项精确比较：`validation/native-trace-on-off.json`、`validation/preflight-*.json`。
原始结果目录：`/Users/wgs/Documents/Codex/2026-09-17/zhi/work/tilegen-trace-cosim/repo/build/qualification-r2`。
