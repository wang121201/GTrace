# Direct trace → HBFSIM 纯访存回放

已在独立融合分支 `codex/tilegen-trace-cosim-20260918-r1` 增加 `replay.py` 和独立 `tilegen_replay` 可执行程序。使用已有 `direct` 模式生成的 cache 后 trace，不创建 Model、解压模板、重建访存指令、重复执行 cache 或调度计算 DAG。B8 仍暂停。

```text
原生访存规则 → 功能 L1/L2 → dram.tgn → 有限队列 → HBFSIM → 完成 / 流量 / 带宽
                direct              memory-only replay
```

## 计算与等待的范围

| 项目 | 本次回放 |
|---|---|
| 地址、顺序、读写方向、请求宽度 | 逐条保留 direct trace；读 128 B，写 32 B |
| 计算指令、warp 依赖、GPU stall、计算/访存 overlap | 不执行 |
| L1/L2、MSHR、dirty flush | 不再次执行；只消费已在文件里的 cache 后请求 |
| 请求到达 | trace 无原始时间戳；全部在模拟 t=0 就绪，按文件顺序尽快准入 |
| 背压与内存时序 | 保留有限 credits、bank/row 冲突、读写切换和 DRAM 命令等待 |
| 跨 kernel | 连续回放，不设 kernel barrier，不重置行状态 |
| 最后排空 | 文件读完后等待所有已提交请求完成；无额外 cache flush |

“去掉 stall”指去掉 GPU 的依赖与调度停顿。HBFSIM 自身的内存服务等待必须保留，才能得到有意义的服务时间和带宽。默认沿用原 global service，每次推进一个时钟；仅当所有已准入请求都已有确定完成时间时跳到下一完成事件。

每个 channel 最多 32 个 32 B burst credits；128 B 读占 4 个，32 B 写占 1 个；全局最多 4096 个未完成 parent。当前 trace 项受阻时，后面的请求不能绕过它。注入顺序及 credits 会影响带宽，因此结果不是硬件峰值，也不自动构成完整推理耗时的严格下界。

## 本机验证结果

以下是单次 CPU 测量，包含 trace 读取、SHA/格式校验及 HBFSIM drain；编译、原 trace 生成另计。MB/GB 使用十进制。输入来自已有 Llama3-8B BF16 / B1 / P32-D2 工作流的**有界子集**。

| 样本 | 条目 | DRAM 读 MB | DRAM 写 MB | 回放 CPU 分钟 | 回放 elapsed 分钟 | 模拟内存时间 μs | 总带宽 GB/s |
|---|---:|---:|---:|---:|---:|---:|---:|
| 20 families，各 1 CTA；global | 68,258 | 8.74 | 0.00 | 0.00595 | 0.01926 | 61.10 | 142.99 |
| Decode2 的 GEMV / SiLU / GEMV，共 1025 CTA；global | 591,652 | 75.60 | 0.03 | 0.02161 | 0.02195 | 225.05 | 336.05 |
| 同一 Decode trace；independent | 591,652 | 75.60 | 0.03 | 0.01781 | 0.01825 | 225.05 | 336.05 |

Decode 写字节为 **33,920 B**，读字节为 **75,595,776 B**；该样本不是完整单次 decode token，更不是完整 P32/D2。global 与 independent 的完整物理统计、完成周期、逐 call 统计、请求形状 hash 精确一致。本次默认保持 global，不把一次 CPU 差别推广为稳定加速比。

带宽计算：`(读字节 + 写字节) / (最后物理完成 ns − 首次准入 ns)`，单位 GB/s。Decode global 读、写分别为 **335.90 / 0.15 GB/s**，使用同一个总时间分母。逐 call 的首尾时刻只是合成内存注入/完成窗口，窗口可能重叠，不能称为 kernel 执行时间。

已有 Decode direct 生成 CPU **25.10 s（0.41837 min）**；此次 global 回放 **1.30 s（0.02161 min）**；跨两次测量相加约 **26.40 s（0.43998 min）**。此前完整精确 cosim 相同选中子集的 CPU 中位数为 **45.97 s（0.76613 min）**，两者精度口径不同，不能声称是等价 cosimulation 加速。同一 trace 可反复回放而无需再次支付 25.10 s 的前处理/生成成本。

## 流量与精度边界

HBFSIM 回放不会重新计算 cache 命中，也不会根据带宽或 NCU 修改 trace 字节。请求数、方向、地址、宽度和顺序形状 hash 均闭合，所有已准入请求最终完成、credits 归零。

此前精确 cosim 对同一选中子集的写流量是 4,864 B，而 direct 为 33,920 B。差别已经在两者不同的 cache 访问顺序与在途合并中产生；本次回放保留 direct 的 33,920 B，不会把它拟合成精确 cosim 或 NCU 的值。`source_final_dirty_flush=false`；文件中没有的 resident dirty sector 不会在 EOF 凭空添加。

当前 GDDR6 意图配置仍是既有未做硬件时序校准的通用 banked 模型，地址是 packed service 地址，不是已恢复的 GPU 物理 channel/bank 映射。本次未做 GPU 采样、NCU 拟合或完整 1138-call 重跑。原 direct 收据只封存配置路径；新回放额外保存当前配置文件副本、SHA 和解析后的完整内存身份，不能倒推证明历史配置字节未变。

## 运行方法

在仓库根目录执行，输出路径须尚不存在：

```sh
python3 build.py --replay --output build/replay-r2 --native --thin-lto
python3 replay.py \
  --source-run build/shared-frontend-validation/decode-direct \
  --input ../validation-inputs/decode-512.input \
  --output build/my-decode-replay
```

已构建的二进制可直接用于第二条命令；也可用 `--binary /absolute/path/tilegen_replay` 指定新构建。`--drain independent` 为可选原生服务实现；默认 `global`。wrapper 默认 trace 上限 8 GiB、最多 20 亿回放周期、3600 秒主机运行期限；均可显式设置。

入口要求同一 direct 运行的 `result.json`、`run-receipt.json`、`dram.tgn` 及原 `.input`。原输入只读取首行 control JSON，整文件做 SHA 验证，不解压模型模板。trace 以 64 KiB 缓冲逐条读取；live 请求和 call 元数据有界，整个 trace 不会加载进内存。

来源链包括原输入 SHA、control context SHA、trace footer/record/whole-file SHA、配置快照 SHA、逐 call 名称与流量守恒。SHA/尾部校验未结束前，模拟结果只是进程内临时状态；只有验证及 drain 全部成功才发布 `result.json`。失败保留诊断与 `.partial`，收据不会标记成功。

实现：`source/trace_replay.h`、`source/trace_replay_main.cpp`。原 direct/cosim/cosim-fast 实现未改动。验收摘要见 `validation/memory-only-replay.json`；实际输出位于 `build/replay-validation/`。

验收已通过：回放逐 tick 参考对照 **11,677 项检查**，普通构建与 ASan/UBSan 均通过；全套五个 CPU 单测通过。CLI 的 2 个正例、15 个来源/损坏/预算负例均通过，失败不会发布正式结果，原输入及配置前后 SHA 未变。CMake 入口已同步，本机使用 clang 构建验证。
