# 完整 LLM producer / prepare 窄审补充

本地只读审阅，无编译、远端或实验。审阅版本 SHA 见 [producer-prepare-source-pins.json](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/llm/review/producer-prepare-source-pins.json)。

**结论：相对旧 Llama producer，既有 source/API/compact-program 语义没有改变；完整 history 与逐 launch 资源覆盖校验保留。发现的 Qwen provider 准入风险已在随后的源码修正中关闭，见文末复查。**

## 必须处理的 Qwen 差异

[new whole_stream.py](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/llm/executor-r1/whole_stream.py:314) 在 `--fast-gemv` 时无条件导入 Qwen 和 Llama 两套 provider。Qwen 复用旧 `cpu-replay-stage-r1/tree/kernel-complete-input-r1`；[该旧 run spec 本地回收件](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/write-gap-fix-20260920-r1/l2-policy-unified-p32d2-20260921/age-sweep-r1/results/a64000000h288/spec.json) 的 157 条 source pins 没有 Llama GEMV provider。这不能证明远端文件一定不存在，但**文件存在与其来源 pin 均未在本次只读证据中闭合**。当前 `--preflight-only` 不调用 emit，因而不能暴露此问题。

最小修正：依据已校验 entries 的 binding schema，仅加载实际使用的 provider；将被加载文件加入 source pin 校验。无需改公式、模板、原事件或旧冻结树。已告知 root。不得仅给 Qwen K 加一个未 pin 的旁路来源来让 import 成功。

## 已确认保留

对 [旧 Llama producer](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/write-gap-fix-20260920-r1/llama-p32d2-20260921/cpu-llama-package-r1/tree/llama-p32d2-20260921/executor-r1/whole_stream.py) 与 [新 producer](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/llm/executor-r1/whole_stream.py) 做 AST 比较，以下八个函数正文完全相同（非运行测试）：`preflight`、`api_effects`、`source_policy`、`normalized_event`、`bind_static_opcode`、`serial_down_schedule`、`gemv_command`、`gemm_command`。

- timeline 仍由所有 nodes 加 contract phases 按唯一 event ordinal 排序，初始化/phase-external/Warmup/Measured 均保留；entry集合与原生kernel集合相等、process/code/raw ABI/单stream验证不变。
- 新 allocation metadata 只是按原时间顺序发送已有 allocation_API_observation；不新增 GPU memory event，不改 memory_API effects，不 flush L2。allocation字段仍标 host_API_return，不能称 CUDA异步完成时刻。
- per-kernel sidecar验证 schema、graph SHA、scope=per_kernel_launch、launch ID唯一，且 ID集合等于**全部**native kernels（含初始化与warmup），不只是 measured。范围级NCU共享内存统计不能直接塞进这个逐launch接口。
- compact GEMV/GEMM/Prefill dispatch顺序、policy及旧provider引用规则不变；support根现在显式指向冻结Llama树，Qwen自己的GEMM/provider仍指向其冻结K。
- sidecar和executor前后SHA复查；原graph/registry/runtime、extra support文件继续校验。新增allocation/metadata记录会改变传输JSON字节及stream SHA，故不能要求与旧字节流逐字相同；应比较既有source事件序列指纹、API数/bytes和原流量账。

## 已发现并已修正的启动问题

初读时 observed-only 模式未提供假设环境变量，但 Adapter 构造会调用 assumed_shared()并拒绝 r2/r4。审阅中 root/Kuhn 已增加 `TILEGEN_ADA_REQUIRE_OBSERVED=1`、`bootstrap_before_first_kernel_no_shared_claim`，每个 begin_kernel 强制真实 observed 字段；prepare已设置此环境。初始bootstrap配置不得被当实际carveout，第一kernel之前只有API旁路。此处当前源码设计已解决，仍需负责方的小fixture验证，不在本审阅中伪称运行PASS。

L1手动bypass sector miss账也已补；per-SM统计明确 `global_only; per_SM counters not exported`。此前主审计记录的是发现时快照，本段记录随后修复状态。

## prepare / 单核预算

[prepare_xmu.py](/Users/wgs/Documents/Codex/2026-09-17/zhi/work/gtsim-ada-r4-llm-20260922-r1/repo/llm/tools/prepare_xmu.py) 只建fresh目录、c++编译、执行只读preflight并写四个spec，不调用模拟/NCU本身。它从两份真实旧spec复制argv；本地回收spec已确认 argv[2]确为Python producer，替换该项合理，原graph/registry/runtime及fastflags保持。原sources逐条SHA校验后，新增本次repo代码、binary、旧spec本身和sidecar pins。两臂固定h288、age64M、noGPU，单核spec为Qwen CPU8/9、Llama CPU10/11，线程环境均为1。

**脚本自身不获取CPU租约/设置affinity；这依赖外层既有shared run_job控制器。** prepare编译也必须由该控制器占一个核；四个spec应分别经run_job启动，不直接运行argv。每job 18,000秒、16GiB；四臂若并行是四个CPU核，不应写成总共单核。没有增加GPU入口。

## 8/16 KiB carveout 外推

sidecar接受8/16KiB是实际资源观测值；映射到L1容量却是本次显式外推，不是旧r4 serial实验新测得的档位。当前adapter用 nominal=131072−shared，r2四set和r4十六set分别向下取ways，再应用既有scale1000/1062：

| observed shared | r2 effective / ways | r4 effective / ways |
|---:|---:|---:|
| 8192 B | 122880 B / 240 | 129024 B / 63 |
| 16384 B | 114688 B / 224 | 120832 B / 59 |

新description已用 `uncalibrated_extrapolation_8_16KiB` 区分这两档；32/64/100也仅是旧serial读模型档位向LLM外推。实际shared观测不自动证明L1容量公式/策略正确，仍需本轮同源LLM结果检验。固定L2与无compute/stall的范围保持主接口审计所述。

## 修正后复查与本地小测试

源码复查确认：emit仅按entries中的binding schema导入实际需要的GEMV provider，main对相同schema选择的provider目录补充.py/.json/.h pins并于finally复核。旧Qwen树不再必须有Llama provider。observed-only bootstrap、每kernel强制sidecar、bypass sector miss计数和global-only统计标识也已落实。

独立 [test_producer_contract.py](test_producer_contract.py) 最终 **9 tests PASS**，耗时0.029秒，收据 [producer-test-receipt.json](producer-test-receipt.json)。测试使用合成小输入，未执行任何缓存进程：

- 旧/new普通READ、WRITE、zero-global-effect LDGSTS、已知control、H2D API流相等；新流只多allocationmetadata及begin_kernel的资源字段。
- Qwen与Llama两类compact GEMV command分别与旧producer相等，新端只加载匹配provider；此项不代替C++ typed公式/缓存结果的独立验证。
- 完整初始化/warmup/measured三launch资源fixture准入；缺初始化、额外launch、duplicate、graph SHA错、range-level scope、非法shared均拒绝，Popen未调用。

其余八个原source/API/command/preflight函数AST保持一致，见前文。实际完整graph与远端旧source pins仍由prepare及既有job controller验证；本测试没有代替它们。

## safe_status_xmu 聚合出口窄审

当前 [safe_status_xmu.py](../tools/safe_status_xmu.py) 仅输出任务root、固定case/profile、状态/错误类别、时钟、计数、opaque SHA/fingerprint、固定配置及逐phase流量；不输出error正文、native symbols、source records、graph_node、allocation ranges或perkernel原始metadata。final_counters只接收顶层数字，含真实VA的L1_adapter_observation为嵌套对象而被排除；current configuration经runner源核查均为固定参数/枚举字符串，无VA字段。shared_kernel_counts只汇总每个carveout档位的kernel数量。

本次检查对**当前固定schema与源pins**成立；不是任意未来字段的自动脱敏承诺。`configuration`按当前整体对象导出，因此若日后添加地址/符号字段需重新审查，当前无此阻断。该脚本读取和打印aggregate，不启动进程或GPU。
