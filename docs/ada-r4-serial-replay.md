# r4 shared-cache串行回放入口

`source/ada_r4_serial_replay.cpp`直接调用共享 `PerSmL1Cache`，没有复制冻结参考模型的替换或hash算法。每case冷启动，单SM、32 B sector、同步fill，仅全局标量读取；`.cg`逐请求旁路。不包含写、DRAM服务、GPU时钟、多warp并发或LLM推理。

固定两个模型：`LRU_u128_s4_h0_c1000` 与 `CLOCK_u128_s16_h2_c1062`。前者为r2串行参考（4sets、LRU）；后者为冻结r4候选（16sets、CLOCK、hash2），三档分别50/33/14ways。参数由共享profile helper提供，`--describe`从实际配置输出六组capacity/sets/ways/replacement/hash供外层固定版本校验。模型不是实际GPU缓存结构的声明。

## 接口

```text
ada_r4_serial_replay --describe
ada_r4_serial_replay --self-test
ada_r4_serial_replay CASES_TSV
```

CASES_TSV无表头，每行五列：`case_id nominal_l1_bytes cg expected_requests request_path`。nominal只允许98304/65536/28672，分别映射shared32768/65536/102400；实际观测值还必须由后续NCU join核验。request文件是little-endian `.u32` allocation-relative字节offset。driver只从XMU现有文件读取，不导出请求。

文件大小必须严格等于expected_requests×4，offset需4B对齐；空请求、空cases、重复case、非法列、短文件或附加内容拒绝。cache请求tag为offset向下128B对齐值，allocation-relative索引字段也是该对齐值，sector取原offset模128。每次miss立即完成共享cache发出的ticket，再处理下一条。

每case输出两条JSONL，`schema=ADA_R4_SHARED_SERIAL_CASE_V1`、`status=PASS`。包含case ID、固定模型参数、hits/misses/requests、L2 read sectors、CPU/wall秒数。CPU/wall包含该case冷cache构造和回放，不包含请求文件读取；它们是宿主耗时，不能当GPU时长/带宽。输出及错误中不包含地址、输入路径或请求内容。异常退出的已完成前缀不能当作完整384条件验收。

## 远端原地比较

```text
python3 -B tools/evaluate_ada_r4.py \
  --replay-jsonl REPLAY_JSONL \
  --replay-receipt WRAPPER_RECEIPT_JSON \
  --hardware HARDWARE_AGGREGATES_JSON \
  --frozen FROZEN_AGGREGATES_JSON \
  --output FRESH_OUTPUT_DIRECTORY
```

比较器必须读取同次wrapper的receipt：`PASS_PROCESS_ONLY`、returncode=0、case_count=384、source/input_unchanged均为true、GPU_executed=false，且prediction_sha256与当前JSONL逐字SHA一致；receipt自身SHA也纳入输入pins。预测文件完整但wrapper失败时仍拒绝，不把完整前缀或变化后的输入当作闭合结果。

比较器只在XMU读取原硬件aggregate和冻结预测，不读取原请求。完整性要求为384 cases×两模型、每case五次NCU、实测shared五次一致且与参数相符、source请求数守恒。每条预测和冻结结果核对hits/misses/capacity；不一致时输出 `FAIL_REFERENCE_EQUIVALENCE`，不通过调整参数消除差异。

`summary.json`仅包含非敏感聚合、SHA和CPU/wall；标准输出也是该聚合。`case-derived.jsonl`保存派生计数与误差，留在远端。fresh0..359单独报告overall、shared、stride y、order z、read-group-stability；`.cg`360..371和跨时段锚点372..383另列。WAPE使用绝对误差求和，有符号总量误差另列，禁止正负相抵伪装成WAPE。分母为0时百分比是null。保留不稳定条件，没有参数再选择。

NCU分母为L2全局读sector五次中位数；不能将其乘32B冒充实测DRAM字节，也不能外推写回或时序精度。reference一致仅证明移植行为匹配，不代表模型已满足硬件误差目标。

## 本地合成测试

driver `--self-test`44项覆盖计数、sector部分命中、cold重建、`.cg`、六组静态容量、空/短/坏长度/非法协议；`tests/test_evaluate_ada_r4.py`十二项覆盖receipt逐字段门禁、完整分母、重复/缺失、观测错配、source数量、冻结预测不符以及WAPE与有符号误差区别。它们没有使用或生成正式硬件结果。
