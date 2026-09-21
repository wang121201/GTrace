# Ada 配置对齐：fine 时序接口已支持什么、仍缺什么

本次先作源码审计，随后仅在 `memory.h::enqueue_dram_request` 修正新 Ada 模式的 backend 请求分区归属，并用小型 CPU mock-backend 测试；没有执行 GPU 或完整模型仿真。修改前审计基线 `memory.h` SHA256 为 `a00961b92b98ec3fff55be91d3ab2c4ff479d5f1c97e116eada1d10622e88949`。L1/direct 的并行开发按其各自交付收据验收；下面不会把它们的新功能自动归给 fine。

**准确的交付描述：共享 RTX 4000 Ada 软件地址拓扑、SM 资源与自适应 L1 配置，并在 direct 功能模式补 sector/lazy cache；fine 继续使用现有 GTSim 异步完成与依赖时序机制。尚未实现与 Accel-Sim 相同的 sector/lazy L2、逐 memory-subpartition MSHR/队列或完整跨分区时序。** 新模式对齐的是封存 Accel-Sim 配置，不宣称还原 NVIDIA 未公开的硬件 hash。

当前 fine 入口明确命名为 `gtsim-ada-accelsim-structure-only-v1`。`source/ada_gtsim_structure.h` 接入48个 SM、GTO、资源约束下的 CTA 容量、自适应 L1 容量/way、34-cycle L1 与30-cycle shared-memory 延迟及新 L2 几何；仍继承旧 L1 store bypass/整line有效性、272/604-cycle L2服务参数和311.660957 GB/s的旧校准带宽。它不是 direct sector 功能入口的时序等价版本，也没有新的硬件精度验收结论。

## 已有能力与关键差异

| 项目 | 当前 fine 实际代码 | 对齐的 Accel-Sim 参考 | 此轮应如何标注 |
|---|---|---|---|
| 地址与相联组 | `L2GroupedLru` 接受 `L2GeometryConfig`，选择 victim 时用 byte VA 的 group；新增模式为 10 channel × 2 memory subpartition × 1024 set × 16 way、128B line。PAPER 旧模式仍保留。 | `addrdec.cc` 的 10 路非 2 次幂 quotient/remainder、bank 低位组 subpartition、`partition_address` 删除该 bit 后才算 X set。 | **可对齐 group/set/victim 的位置**；相同容量并不意味着相同组映射。 |
| L2 sector 状态 | `CacheLineState` 仅 dirty 四位和 LRU iterator，没有逐 sector valid/readable/reserved 或每字节 known 状态。只要 line 在 cache，read 即命中。 | `sector_cache_block` 对四个 sector 各有 INVALID/RESERVED/VALID/MODIFIED、readable 和 fill 状态；dirty byte mask 与 readable 分离。 | 现有 **32B dirty 跟踪和逐32B写回**不等于 sector read/fill 命中模型。 |
| 写未命中 | `process_transaction` 的读 miss、写 miss 均创建同一种 `MSHREntry` 并 `enqueue_dram_request(...FILL_READ)`；默认请求整个128B。 | `L:B:m:L:X` 中 `m` 为 ON_MISS，第二个 `L` 为 LAZY_FETCH_ON_READ：写 miss 分配/更新字节、标 dirty，不立即发 RFO；部分写后的 read 根据可读性再 fetch。 | fine 仍为 **line fill + write-miss RFO**。把 direct 的 lazy flag 写进统一 profile 不能声称 fine 已切换。 |
| 预留与并发 fill | fine 已有真实待完成 map、合并 waiter、backend credit/backpressure 和真实完成唤醒。cache way 是收到 completion 后才 `insert_line`，此前 MSHR 不占 resident way。 | ON_MISS 即预留 way；all-reserved 会 RESERVATION_FAIL；`A:192:4` 限制每个 L2 cache 的 MSHR 数和每 entry merge 数。 | **不能说 fine 没有 MSHR/in-flight**；但也不能把其动态 map 当作 Accel-Sim 的192/4及 reserved-way 模型。 |
| 32B backend 请求 | dirty mode2 每 dirty bit 一条32B请求，保留 parent line key；独立 outstanding ID、有限 pending FIFO `4*(B+1)`，等待 backend 正式接受/完成。 | sector 数据流及 miss queue/return queue 有其独立容量与调度。 | 已有写回宽度契约可保留；fill 仍128B，不能把写粒度直接解释为所有服务都是32B。 |
| memory subpartition 路由 | DAG入口及内部 transaction 保留 `node.warp_id % 4`；light入口仍要求 `subpartition_id <4`。仅新 Ada 模式的出站 `L2DramRequest.l2_subpartition_id` 改为从请求原始 `key.line_addr` 解码0..19；写回用 victim key，服务地址 remap 不参与几何。旧 FA/PAPER 模式保留原 context 字段。 | 参考的 memory subpartition 是 `chip*2+(bank&1)`，0..19，与 SM 内四个计算子分区无关。 | 已补 **出站请求的 memory 目的地归属**，没有新增20路 memory queue、改变调度或实现 Accel-Sim 时序。 |
| 服务预算 | 一个 L2Cache 对象中共用 read/write queue、总预算/写子预算；pending backend 请求会阻塞后续 L2 ingress。 |20个 memory subpartition 的各自 cache、miss queue、MSHR、partition队列及 DRAM 服务。 | 几何模式只改变冲突组；当前服务并不是20套独立有限队列。 |
| L1 sector 输入 | fine 的 `enqueue_pre_l1_metadata` 只初始化旧六项 access 字段，未传新 sector/known-byte mask。新 L1 接口零 mask 兼容回退整line。 | sector 请求需保留该条访问实际 coverage；只补 L1 容量/延迟不能补 source byte mask。 | direct 新 sector 行为与 fine 需分别标注；fine 使用兼容整line请求，不能仅开启 sector flag 就当细粒度 sector 命中已接通。 |

## 后续支持 sector + lazy L2 的最小改动边界

1. **补请求语义而不复用 dirty mask。** `Transaction` / light request / metadata 路径增加实际 read sector coverage、逐sector known-byte mask与必要原子/RMW语义；从 native ranges 求出，传给 L1，再把 missing mask 传给 L2。`dirty_mask`仍只代表真实写入。旧入口默认行为保持显式兼容，不伪造满sector覆盖。
2. **新增显式 L2 状态模式。** `CacheLineState` 需要 sector valid/readable、known bytes，必要时 reserved/fill状态；read hit 判断实际覆盖。lazy store miss 应在 miss 时分配并标已写字节，不提前发 RFO；read 读取未知字节时才发所需 fetch。store 与 pending fill 交错须保持已写字节，不能被晚到 fill 覆盖或变成全可读。
3. **补 fill/完成身份。** 当前 `MSHREntry` 只有单 `dram_request_id`，所有 waiter 一起完成。sector fill 必须用 mask/子请求标识闭合，只有所需 sectors 到齐才唤醒对应 waiter；`enqueue_dram_request` 对 `FILL_READ` 禁止 offset/size 的旧约束也需改为显式新模式。ordinary `step` 与 `service_epoch` 两份完成处理必须一致修改，不能只修其中一路。
4. **把“功能 sector/lazy”与“Accelsim 时序等价”分开。** 若本轮只需要功能共享，可保留原排队/时序模型并公开其差异。若要进一步对齐192/4、ON_MISS reserved way和逐分区 queue，必须新增有限资源准入/重试及 reservation 回滚；这不是把参数填进 JSON 就完成。当前 completion 后分配与 lazy ON_MISS 不同，直接替换易造成过早驱逐、丢失 waiter 或死锁。
5. **保持来源与目的分区分离。** 本轮最小修正已保留内部 SM compute subpartition 来源，仅新 Ada 出站请求使用 address decoder 的 memory subpartition。后续若细分服务资源，再显式命名/传递 channel 与目的地字段。geometry/index 使用原 byte VA，backend 的地址 mapper 只改变服务地址；不能在 decoder 前后重复删 partition bits。key 中 matrix/allocation namespace 的身份规则也须保留，不能因换 index 无意合并不同对象。

最小 CPU 负例应包括：同line只读一sector后读另sector；部分store→read未写字节；多次store补满sector；pending fill期间store；同line不同sector独立完成/合并；17条同set并发miss占16way的 reservation/backpressure；192entry/4merge边界；写回等待backend时仍可推进已接受completion；fine/epoch结果一致。所有新模式缺省关闭，旧 PAPER/旧 fill/RFO序列先回归，不用NCU差额选缓存状态。

## 证据位置

- [fine structure-only入口与保留参数](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/repo/source/ada_gtsim_structure.h:8)
- [出站分区、victim写回与legacy回归测试](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/repo/tests/ada_fine_partition_test.cpp)
- [fine line/MSHR 状态与构造](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:1126)
- [来源子分区与 light 请求合同](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:1438)
- [fine 全局预算和异步完成](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:1544)
- [fine metadata→L1 请求](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:2016)
- [fine miss/RFO 与 pending merge](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:2206)
- [fine fill时驱逐、32B写回与backend请求](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/memory.h:2404)
- [参考 ON_MISS/RESERVED 与逐set victim](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/reference/gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-cache.cc:248)
- [参考有限 MSHR/merge](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/reference/gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-cache.cc:561)
- [参考 lazy store miss](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/reference/gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-cache.cc:1746)
- [参考 sector 状态](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/reference/gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-cache.h:291)
- [参考 Ada 配置](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-alignment-20260921-r1/reference/util/tuner/NVIDIA_RTX_4000_Ada_Generation/gpgpusim.config)

新decoder的独立 CPU 检查为 207,714 地址、1,820,265 checks。补入同 commit `hashing.cc` 的未经改写 XOR helper 联编后为 2,027,979 checks，O2 与 ASan+UBSan均通过；完整 upstream translation unit 未编译。`hashing.cc` SHA256 为 `14746fbbc84b1928c9346368af3c4ca12df64c6f405057fb11f95af9f817eec2`，其 X helper 是 `index ^ (higher_bits & (bank_set_num - 1))`。这些检查仅验证地址映射，不构成上述时序/sector功能已实现的证明。

fine出站归属的独立测试在dirty mode2下812项、mode0下508项通过，mode2同时通过ASan+UBSan。测试覆盖20个目的分区、服务地址映射前后的区分、两个32B dirty-sector写回与FA/PAPER旧字段回归；没有改变请求时序、容量或MSHR策略。测试JSON成功标记为 `PASS_ADA_FINE_PARTITION`，与统一CPU测试运行器保持一致。
