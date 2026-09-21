# r2内部参考时延：单位、接入与边界

新增入口为 `make_rtx4000_ada_calibrated_internal_config(resources, profile=R2_ADAPTIVE, optional observed_shared_carveout_bytes)`，定义在 `source/ada_gtsim_structure.h`。原 `make_rtx4000_ada_accelsim_structure_config` 保持原行为；新入口选择tuner-v1也返回旧factory语义。

冻结r2契约给出L1基础34、L2附加239、DRAM附加324。取整后的名义加和为L2总273、冷miss总597。不得将L2实测总延迟272.68445/整数273填到附加239的位置，也不得把DRAM实测总延迟596.228当成附加324。两组时钟配置中core/L2同为2175 MHz；本适配器实际消费的单位是GTSim **core Cycle**，不是DRAM时钟周期或ns。

## 实际生效字段

| 字段 | 旧factory | 新calibrated-internal参考 | 执行位置 |
|---|---:|---:|---|
| `per_sm_l1.hit_latency_cycles` | 34 | 34 | L1命中后从当前core Cycle起计 |
| `l2_hit_latency_cycles` | 272 | **273 = 34+239** | L2命中决策后起计；fine L1 miss没有先等待34cycle |
| `memory_model_semantics.l2_miss_latency` | END_TO_END_FROM_MISS_DECISION | 同左 | 内部fill完成时直接唤醒，不再追加L2 hit延迟 |
| `end_to_end_miss_latency_cycles` | 604 | **597 = 34+239+324** | 实际DRAMModel读miss ready time = issue_cycle +597 |
| `l2_miss_penalty_cycles` | 604 | 597 | 内部DRAM构造参数；end-to-end读miss由上一行控制 |

因此新入口实际执行273/597，并非只把239写进JSON。这里将分级常量压到原fine模型的决策至完成区间，是明确的近似适配；它没有重建Accel-Sim流水级。服务credit不足仍可能延后完成，597不是有竞争时保证的端到端时长。

旧A3B4默认已经采用end-to-end语义。只有另一种 `LEGACY_ADDITIVE_DRAM_THEN_L2_HIT` 才在DRAM完成后再加L2 hit延迟。新入口明确保留end-to-end，防止误得597+273。DRAMModel的ready time加法使用core Cycle；带宽的core/DRAM频率换算不自动换算此延迟。

## 保留内容与使用限制

新入口用校准profile选择L1容量、ways及LRU/FIFO，并复用原资源分配与新Ada几何。fine仍为whole-line有效性、L1 store bypass、write-miss RFO和原fill时分配；没有自动获得direct的sector/lazy语义。旧带宽rate、队列容量、排队顺序及MSHR实现也保持。

这是 **internal-reference** 接口，未接入外部HBFSIM。外部backend已提供自己的完成时间，不能将597或273盲目再叠加其服务时间，也不能只改此factory就声称HBFSIM时延已校准。functional direct replay仍然不执行时钟，它报告的参考延迟保持 `executed=false`。

`TILEGEN_DIRTY_SECTOR_MODE=2`要求backend声明有限admission，而现有内部DRAMModel接口没有此上限。此轮测试使用容量64的薄wrapper，只管理credit并原样转发真实DRAMModel的时延、带宽和完成算法；没有新增生产backend或解除原构造约束。普通默认Simulator构造在mode2下仍不能凭本factory绕过有限backend合同。

## CPU验证

`tests/ada_calibrated_timing_test.cpp` 通过实际L2Cache + DRAMModel验证：

- 冷fill在issue之后597cycle完成，node在同一cycle完成，未追加273。
- 同地址再次读取且绕过L1时，L2 hit在决策后273cycle完成，无第二个DRAM请求。
- 普通step和span1 `service_epoch` 的相对时延一致。
- 请求与完成字节闭合，仍为128B read fill；未修改缓存traffic规则。
- v1仍为272/604，r2/r3资源配置与静态24 KiB/FIFO可进入fine结构配置；无匹配observed时拒绝固定profile。

O2测试31项通过，成功标记 `PASS_ADA_CALIBRATED_TIMING`。这些是代码单位与执行行为检查，不是硬件时序或LLM精度验收。

## 来源

- [r2延迟原始契约](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/reference/r2/configs/parameter_contract.json)
- [新旧factory](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/source/ada_gtsim_structure.h)
- [A3B4实际语义](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/model_semantics.h:198)
- [真实DRAMModel ready time](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:972)
- [fine miss完成分支](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:1686)
- [时延测试](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260922-r2/repo/tests/ada_calibrated_timing_test.cpp)
