# Host 参数 typed decoder

这是独立的 CPU 处理步骤，消费成功的 `SG_NATIVE_ARGUMENT_CAPTURE_CENSUS_V1` 和同次采集的 journals / native artifacts。它只解释 host 捕获的参数字节，不解引用设备指针，不生成访存地址，不放松旧 `Model` 的 sealed 验收，也不授予新工作负载原生模型资格。

## 使用

在 repository 根目录执行：

```sh
python3 -B native_transfer/decode.py \
  --census /absolute/run/argument-census.json \
  --observer-dir /absolute/run/observer/process-PID-START \
  --artifacts-dir /absolute/run/native/artifacts \
  --output /absolute/output/typed-parameters.json
```

`--observer-dir` 默认取 census 中的路径，下载后的远端绝对路径应显式覆盖。`--artifacts-dir` 默认优先使用 census 同目录下的 `native/artifacts`，否则使用 census inventory 路径。输出父目录须已存在。输入 census、journal、artifact、catalog 不得作为输出路径。

成功状态固定为 `PASS_TYPED_PARAMETER_DECODE_ONLY`。成功表示输入字节/身份闭合与解码步骤完成，**不表示所有调用均支持**。请同时查看：

- `measured_launches / decoded_calls / unsupported_calls / complete_decode_coverage`。
- `calls[]` 的 typed 参数、明确的指针字段、实际 phase 和 `template_candidates`。
- `unsupported[]` 的完整 call key、phase、family 与原因；不支持的 call 不会静默遗漏。
- `qualification` 中 object-root binding、dynamic memory、register/control transfer、native model admission 均为 false。

不支持的 code/ABI/几何/资源或模板 scalar 值产生逐 call 原因；坏 SHA、错误 process/launch 关联、输入 ledger 不闭合、失败 census、journal 或 artifact 改动则使 CLI 非零退出，不发布新输出。数据以 `.partial` 完整写入后原子改名；失败不会将部分结果标成 PASS。

## 验证边界

1. 必须有 census 同目录的 `controller.json`：status 为 `PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY`，其 `argument_census_sha256`、phase/count 绑定本次 census；孤立 PASS census、失败 controller 或不同 SHA 都拒绝。随后验证 census schema/status、同 process observer 目录、完整七份 journal 的长度/SHA、finish SHA/闭合状态、同 run artifact inventory 与每个 artifact SHA。验证 manifest 的 process/contract；解码结束重新核验输入文件。这里复核 consumer 的证据身份，不重复其完整静态 SASS/control 分析。
2. 逐 call 比较 raw record 与 census 的 source key、native launch ID、code/ABI/argument sizes、geometry/resources、scope 和 phase；保留 consumer 的实际 module ancestry，不以旧进程地址替换。
3. catalog 由已封存 source typed 模型导出并固定 SHA，运行时不需要加载整个旧 `Model` 或旧源文件。自定义 catalog 必须同时提供 `--catalog` 与 `--catalog-sha256`，输出记录该身份；不得把修改过的 catalog 解释为原封存 domain。
4. GEMV 使用一个 152B 参数、38 个 little-endian u32；weight/input/output 指针为 word0/1、4/5、8/9，word12/13 必须与 output alias。逐字校验其余120B，匹配11个旧模板候选、K/N/no-tail 和指针对齐。每个候选保留旧 template pin，尚未做 target transfer。
5. PlainNorm / FusedNorm 虽有相同 ABI，按 code SHA 区分角色。SiLU 保留 d、几何、full-width 指针范围与 restrict alias 条件。范围是模板要求的最小 view span，不是已观测分配或 root 资格。
6. RoPE 解码18个参数，但 `pos_ids` 仅是捕获的指针。`position_values=null`，不读设备内存，不虚构1024..1055；实际 Decode3..32 phase 原样保留。候选 source_phase 是来源标签，不会改写目标 phase。CTA class/control 和位置数据仍需后续独立准入。
7. 新 P1024 Prefill 的 norm/SiLU/RoPE 形状不在旧匹配 domain，本步骤明确列为 unsupported；没有通过放宽守卫将其接入。

`decode_census(census_path, observer_dir=None, decoder=None, artifacts_dir=None)` 返回完整 JSON 对象。较低层 `Decoder.decode(record, launch, process)` 用于后续 typed constructor 准入模块；它同样没有 object/root 或动态资格。`allow_legacy=True` 仅支持历史真实 raw schema 的 CPU 回归，生产 census CLI 不开启它。

## CPU 验证与可复现来源

```sh
python3 -B native_transfer/test_decoder.py
python3 -B native_transfer/validate_legacy.py \
  --source-root /absolute/old/work/tilegen-full-r1 \
  --output native_transfer/legacy-regression.json
```

`regression_inputs.json` 是23个真实旧捕获例子，覆盖11个GEMV模板和其余族的旧阶段。原始字节与既有 typed plan 的 `argument_line_sha256` 先连接，expected 参数直接来自独立旧 typed plan；没有用本解码器自产答案。fixture 固定 SHA，日常测试不依赖外部大数据。

`validate_legacy.py` 再覆盖真实旧 corpus 全646 calls：GEMV259、PlainNorm3、FusedNorm192、SiLU96、RoPE96。`legacy-regression.json` 保存证据 pin、CPU user+system 和结果。该回归不是新 P1024/D32 采集，也不是动态地址测试。

测试包括全部30个 GEMV 非指针 word 的逐个 mutation、alias/alignment、norm ABI 区分、scalar/domain、错误 SHA/transport/ABI/process/identity、journal/artifact 改动、RoPE phase 保留/未知 positions，以及 CLI 成功与失败发布行为。

`build_catalog.py` 与 `freeze_regression.py` 是可复跑的导出脚本，均仅读取显式 `--source-root`，只写显式 `--output`。导出文件的 source pins 记录原始 evidence；核心 runtime、capture、reporter 均未被修改。
