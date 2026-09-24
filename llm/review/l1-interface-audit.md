# L1 接口迁移只读审计

结论：**旧 `sector_validity` 的逐请求 L1 过滤可以在新 `sector32` 实现中保持等价，但不是字段改名即可保证。** 实际旧 Llama runner 逐 line 同步完成 fill，因此新实现的 pending-way 保护在这个调用方式下不改变替换选择。当前开发中的 adapter 为 `legacy32` 直接保留旧 L1；这是可用的兼容对照，但不构成“新 L1 在 legacy32 参数下已经等价”的证明。

范围：本地源码只读审查，无模拟、无 GPU、无远端操作；下述测试是建议而非已运行结果。源快照 SHA 见同目录 `l1-interface-source-pins.json`。开发中的 adapter 仍可能更新；此文不代表其最终发布审查。

## 实际入口与边界

旧正式 Llama 包入口是 [runner.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/write-gap-fix-20260920-r1/llama-p32d2-20260921/cpu-llama-package-r1/tree/llama-p32d2-20260921/executor-r1/runner.h:17) → 同目录 [candidate_direct_cache.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/write-gap-fix-20260920-r1/llama-p32d2-20260921/cpu-llama-package-r1/tree/llama-p32d2-20260921/executor-r1/candidate_direct_cache.h) → cache-deps 的 [per_sm_l1.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/write-gap-fix-20260920-r1/llama-p32d2-20260921/cpu-llama-package-r1/tree/kernel-complete-input-r1/input-graph-rebuild-r1/engine/cache-deps/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h)。不能把 cache-deps 内较早的 `direct_cache.h` 误当实际含 dirty-age 的完整执行入口。实际 runner 固定 48 SM、32 KiB/SM、4 sets × 64 ways、128 B tag、32 B validity、kernel flush、store bypass；L2 为 40 MiB `PAPER_ADA_L2_V1_20x1024x16`，128 B fill/RFO、32 B writeback、EF h288，dirty-age 由运行环境设为 64,000,000 accesses。

新共享实现是 [per_sm_l1.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h)。[NEW/source/direct_cache.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/source/direct_cache.h:79) 的 `access_legacy` 没有传实际 sector mask，且自身不含旧 LLM 的全部 EF/dirty-age/owner 策略；其 `access_ada` 又启用 sector L2 fill/lazy store 和新地址映射。因此本次应迁移共享 **L1 实现** 到旧 LLM L2 runner，不能整体替换成这个通用 `FunctionalCache`。

## 逐项语义

| 项目 | 旧 sector_validity | 新 sector32 | 迁移要求 |
|---|---|---|---|
| 请求覆盖 | `requested_sector_mask`，1..15 | `sector_mask`，0 被解释为 15 | 必须显式传源 ranges 的真实非零 mask，不能传 dirty mask，也不能漏填 |
| tag hit + 部分 sector miss | 保留 tag、touch LRU、只给缺失 sector 发 ticket | 同样行为，LRU 模式下 touch；FIFO/CLOCK 各有自身规则 | legacy 参数必须显式 LRU；每 line 仍只一次 access，不按 sector 拆成多次 LRU touch |
| fill 完成 | ticket OR 入 `valid_sector_mask` | ticket OR 入 `readable_sector_mask`，补全相应 known bytes | 只 complete 本次 ticket；L2 的 128 B fill **不意味着**向 L1 人工填满四 sector |
| read-hit 判定 | tag 存在、ready 且请求 mask 全覆盖 | sector32 时直接检查 readable mask 覆盖 | 同步 fill 后的读过滤等价；不能把 `.ready` 直接用于新 sector hit 判断 |
| ready 统计 | 第一个 sector 完成后 ready=true；每次新增 mask 都计 ready_transition | 四 sector 全 ready 才 ready=true；另有 sector promotions | ready_lines/pending_lines/ready_transitions 不同口径，不可作为旧逐字段 equality 门 |
| store bypass | 不查 tag、不 touch、不置 valid，直接到 L2 | store_bypass=true 时相同 | 必须 true；同时 write_allocate=false、dirty_protection=0。若关闭 bypass，新 store 命中/known-byte 行为不等价 |
| pending reservation | 旧 LRU 可替换仍有 ticket 的 tag，旧 ticket 会 stale | sector32 保护 pending tag；无可选 way 时不 allocate，返回 reservation_failed | 当前 synchronous completion 下没有跨请求 pending；不可据此声称异步 cosim/MSHR 行为等价 |
| kernel boundary | 第二次 begin_kernel 起 flush L1，不 flush L2 | 同样；另有容量重配显式清空 | 每个原 native kernel 边界一致；容量变化只能在无 live ticket 时执行 |
| 指纹/计数 | decision hash 不含新 sector 字段；有 sector 请求统计 | hash 加 sector/known bytes/reservation/relative offset；旧 sector counters 不再原生提供 | 对照规范化访问结果及 L2/WB 序列；不同版本 decision hash 不可直接相等 |

以上对应旧 [classify/access](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/write-gap-fix-20260920-r1/llama-p32d2-20260921/cpu-llama-package-r1/tree/kernel-complete-input-r1/input-graph-rebuild-r1/engine/cache-deps/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h:214)、[complete_read/count_sectors](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/write-gap-fix-20260920-r1/llama-p32d2-20260921/cpu-llama-package-r1/tree/kernel-complete-input-r1/input-graph-rebuild-r1/engine/cache-deps/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h:429)，新 [access](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h:384)、[complete_read](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h:604)、[sector helper](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/per_sm_l1.h:726)。旧调用每个 line 在 L2 操作完成后立即 `complete_read`，随后才处理下一 source line；无需为这个 serial 模式模拟新的并发 MSHR。

## 最小迁移与当前 adapter 审阅

兼容参数应明确为：`sector32=true; store_bypass=true; write_allocate=false; dirty_protection_percent=0; replacement=LRU; hash_policy=LINEAR_GLOBAL; capacity=32768; ways=64; num_sms=48; persistence=KERNEL_FLUSH`。保留原 `(allocation_id, canonical_line)` tag、SM 分派、subop 首次 line 出现顺序及 L2 source hash；不要依据 L1 missing mask 拆分旧 L2 128 B fill/RFO 服务。

[开发中的 llm_l1_adapter.h](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/llm/executor-r1/llm_l1_adapter.h) 已采用 legacy32→旧实现、r2/r4→共享新实现，并覆盖 48 SM/store bypass。这可以让 legacy32 作为发布兼容回归。r2/r4 需 allocation-relative hash 时绑定实际 driver allocation base/generation；不得把 tensor view 起点当 malloc base，也不得截断超过 UINT32_MAX 的 relative line offset。新共享 cache 在 bypass 前也验证 relative 地址；adapter 在旁路前直接转发可避免对 `.cg`/store 强造 hash 地址，但这个 bypass 补记必须完整。

审查时发现应修正的**统计问题**：adapter 手动 bypass 分支累加 sector requests/forwarded/bypassed，未累加相应 read/write sector misses；旧 `count_sectors(valid=0)` 将全部 bypass 请求计为 misses。应补足，使 requests=hits+misses；这是观察账本问题，不改变 traffic。另 `current_->statistics().per_sm` 未合入旧形状输出，不能把空数组称为完整逐 SM 计数；可完整合并或显式标为不提供。已即时告知 root 供转交实现者。

r2/r4 用 driver allocation generation 作为 L1 namespace、legacy 原来全局 matrix0。kernel flush 且 allocator API 不在 kernel 内执行时，这不会改变同 kernel 的有效 VA tag 关系；仍建议用 allocation reuse + kernel boundary fixture核验，报告保留 namespace 变化。L2 必须继续使用原全局 VA namespace，不随新 L1 allocation ID 改动。

## r2/r4 对照口径

[冻结 serial profile helper](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/source/work/tilegen-full-r1/core-native-copy-r2/include/ada_r4_profile.h) 给定 shared carveout 下的参数为：

| shared bytes | r2 nominal bytes / sets / ways | r4 effective bytes / sets / ways |
|---:|---:|---:|
| 32,768 | 98,304 / 4 / 192 | 102,400 / 16 / 50 |
| 65,536 | 65,536 / 4 / 128 | 67,584 / 16 / 33 |
| 102,400 | 28,672 / 4 / 56 | 28,672 / 16 / 14 |

r2=LRU + global linear set index；r4=CLOCK + allocation-relative hash2 + floor 后 1.062 容量缩放。**这是容量、set mapping、replacement 联合变化，不可把所有差异归因 CLOCK。** 这里 shared100 对应 serial r4/r2 的 28 KiB，并非另一个 r3 FIFO profile 的 24 KiB。

helper 原本只描述单 SM、串行只读链。LLM 扩展显式设 48 SM、保留 store bypass；这是固定 r4 模型向 LLM 输入外推，尚不是 LLM NCU 已验证结论。实际 observed carveout 优先；如果只能给环境假设，必须显示 `assumed_explicit_environment_not_observed`，不得将 per-CTA dynamic shared bytes 等同 carveout。

两臂固定：同一完整 native graph/API/初始化与 warmup/history、source顺序/原策略/SM映射、L2几何与容量、h288/64M、128 B fill/RFO、32 B dirty/WB、无 final flush。不得偷偷启用新 Ada L2 geometry/lazy write allocate。L1变化会改变到达 L2 的请求及 EF PRNG eligible 次数；保持同算法/seed即可，不能要求后续每个 PRNG decision仍与旧输入一一对应。报告分列 CPU与wall分钟；memory-only无 compute/stall，因此不能直接称硬件kernel latency/bandwidth预测。

## 有意义的最小验证建议

1. 同 line mask1→mask2→mask3→mask1，加跨line/跨SM/conflict：比较旧/new legacy参数的 hit/forwarded/missing、tag replacement、L2请求和32B WB有序序列；新旧raw decision hash与ready统计单列。
2. `.cg` 读和store bypass（包括已有L1 tag）不改变 tag/LRU；计数验证 sector请求=hit+miss、bypassed⊆forwarded，且store脏32B账只在L2更新。
3. partialfill后 read剩余sector；一个128B L2fill不自动造成四sector L1hit；重复ticket/flush后stale按各API拒绝或retire边界检查。
4. 原参数 legacy32 同输入 fragment/完整结果保留 source+postcache指纹与全部原 traffic/dirty/owner 账；若未来将legacy32也改为共享实现，需再做一次独立桥接对照。
5. r2/r4相同 observed/assumed carveout、实际allocation reuse/alias、4GiB上界与缺失relative负例；所有L1查找有range资格，bypass无需虚构地址资格。
6. 每臂所有 kernel/API/phase数量和source payload相同；每phase累加与最终32B dirty守恒，L2参数逐字段相同。r4支持范围只是此serial caller顺序，不把该结果外推到GPU issue/MSHR/HBFSIM时序。
