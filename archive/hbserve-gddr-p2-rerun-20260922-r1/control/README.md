# 同配置隔离重跑：仅延长宿主预算

本包不改 17.10 Gb/s 配置、fixture、plan、admission、缓存策略、源工作、clock、HBFSIM 或验证器。原 `contract.py` 字节原样复制。唯一行为变化是宿主总上限由 5400 延长为 7200 秒，执行截止由 5340 延长为 7140 秒，仍留 60 秒验证和清理。一小时 target 保持 3600 秒；延长上限不代表达到性能目标。

原 admission 内的旧预算仍作为来源保留。新 `host-budget.json` 独立声明有效宿主预算并固定新 controller/contract 哈希；新 receipt 的 `host_budget_override` 记录两者。模型、完整工作、日志/进度、source pins、Fine/Tiny/API 与 dirty/HBF/writer 验证原样保留。原 OwnedGroup 清理 gate 不放宽。

## 命令

默认无 `--execute` 只做保存输入预检。由主线程在已有授权下以提升权限运行，避免旧沙箱 `probe_group` EPERM：

```sh
/usr/bin/caffeinate -i /usr/bin/python3 -B /Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment/work/ada-cosim-alignment-20260922-r1/gddr-full-p2-rerun-r1/run_full.py --admission-sha b8eeb7b81e85f6479d044aa1cd22c50ba326e03eb1223f189ef2070174a607ed --host-manifest-sha b52bd3f0514b1ca50ab69ee3462d0086d17adf891e55fe13f742afae23ba8a3c --run-name run-r1 --execute
```

`caffeinate -i` 只在该命令存活期间抑制空闲睡眠，不更改系统设置，也不保证合盖/强制睡眠不会发生。运行时保留真实墙钟，不扣除睡眠；新一轮从空缓存开始，不续接前次部分状态。控制器运行在本独立目录的全新 `run-r1` 中，已有同名目录直接拒绝。

## 本地检查

`source-checks.json`：10 项检查通过，完整 1372 项原准入哈希实核；实际预检 0.704641875 秒，未执行模型或编译器，未创建 run-r1。错误宿主清单哈希被拒绝。native argv 到完整闭合验证段除固定 admission 路径与额外宿主源重核外字节完全一致；`source.patch` 是完整差异。

完整准备墙钟未测，不把这次预检时间冒称全部准备成本。正式 receipt 的实际执行和收尾时间由原 raw clock 记录。结束状态仍为原 PASS/FAIL 名称，PASS 必须实际完整 1138 kernels、29 APIs 和所有闭合门通过；目标是否达到独立字段记录。

原完整结果分析器固定旧预算，不能直接套用新 7200 秒收据；后处理应使用单独明确预算适配的新入口。原失败结果与原分析器均不修改。
