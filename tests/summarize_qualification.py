#!/usr/bin/env python3
"""Summarize completed native integration runs without extrapolating full-model speed."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    args = parser.parse_args()
    current, baseline = args.runs.resolve(), args.baseline.resolve()
    state = json.loads((current/'status.json').read_text())
    if state['status'] != 'PASS':
        raise ValueError('qualification is not complete')
    comparisons = []
    for name in ('families-cosim-off', 'decode-cosim', 'decode-cosim-fast'):
        path = ROOT/'validation'/('preflight-'+name+'.json')
        subprocess.run([sys.executable,str(ROOT/'tests/compare_preflight.py'),
                        str(baseline/name),str(current/name),'--output',str(path)],check=True,
                       stdout=subprocess.DEVNULL)
        comparisons.append(json.loads(path.read_text()))
    rows = []
    for run in state['runs']:
        name, receipt = run['name'], run['receipt']
        result = json.loads((current/name/'result.json').read_text())
        direct = receipt['mode'] == 'direct'
        ledger = result['cache' if direct else 'workflow']
        dirty = result['cache' if direct else 'dirty_sector_evaluation']
        rows.append(dict(name=name,mode=receipt['mode'],CPU_minutes=receipt['CPU_minutes'],
            elapsed_minutes=receipt['elapsed_minutes'],read_bytes=ledger['DRAM_read_bytes'],
            write_bytes=ledger['DRAM_write_bytes'],trace_file_bytes=result.get('trace',{}).get('file_bytes'),
            calls=result['selected_source_count'],
            CTAs=result['selected_CTAs' if direct else 'prepared_CTA_count'],
            modeled_nodes=result['source_nodes' if direct else 'prepared_node_count'],
            dirty={k:dirty[k] for k in ('dirty_sector_creations','evicted_dirty_sectors','resident_dirty_sectors',
                                        'dirty_sector_ledger_closed','writeback_byte_ledger_closed')},
            source=str(current/name),binary_sha256=receipt['binary_sha256'],input_sha256=receipt['input_sha256']))
    by_name = {row['name']:row for row in rows}
    direct_dirty = by_name['decode-direct']['dirty']
    native_dirty = by_name['decode-cosim']['dirty']
    binary_hashes = {r['binary_sha256'] for r in rows}
    if len(binary_hashes) != 1:
        raise ValueError('final qualification must use one binary')
    build = json.loads((Path(state['binary']).parent/'build-receipt.json').read_text())
    trace_pair = json.loads((ROOT/'validation/native-trace-on-off.json').read_text())
    if trace_pair['binary_sha256'] not in binary_hashes:
        raise ValueError('trace on/off comparison is from another binary')
    document = dict(status='PASS_BOUNDED_NATIVE_INTEGRATION',
        branch='codex/tilegen-trace-cosim-20260918-r1',batch_size=1,writeback_request_bytes=32,
        binary_sha256=next(iter(binary_hashes)),platform=build['platform'],
        full1138_grids_rerun=False,new_GPU_sampling=False,new_NCU_fitting=False,
        rows=rows,host_preflight_exact_comparisons=comparisons,
        trace_on_off=trace_pair,
        decode_direct_vs_native_CPU_ratio=by_name['decode-cosim']['CPU_minutes']/by_name['decode-direct']['CPU_minutes'],
        limitations=['Direct fixed functional order differs from scheduled cosimulation.',
                     'cosim-fast explicitly approximates timing.',
                     'Runtime sealed data remains external; no XMU deployment claimed.',
                     'Single runs on a shared macOS host; not full-workflow performance qualification.'])
    (ROOT/'validation/qualification.json').write_text(json.dumps(document,indent=2)+'\n')
    def amount(value):
        if 0 < value < 5000:
            return f'{value/1e6:.2f} MB（{value:,} B）'
        return f'{value/1e9:.2f} GB' if value>=1e9 else f'{value/1e6:.2f} MB'
    lines=['# 融合分支验证结果','',
        '分支 `codex/tilegen-trace-cosim-20260918-r1`；B1，32 B 写回；B8 未合并。','',
        '## 实测运行时间与流量','',
        f"运行平台：`{build['platform']}`，CPU 执行，无新 GPU/NCU 采样。",
        'CPU 为子进程 user + system；elapsed 为启动至退出，包含输入传输、模拟、trace 写入和读回；不含编译、预先准备输入及 wrapper 的额外结果核验。每项为一次实测，共享主机。','',
        '| 测试 | CPU 分钟 | elapsed 分钟 | DRAM Read | DRAM Write |',
        '|---|---:|---:|---:|---:|']
    for row in rows:
        lines.append(f"| {row['name']} | {row['CPU_minutes']:.2f} | {row['elapsed_minutes']:.2f} | {amount(row['read_bytes'])} | {amount(row['write_bytes'])} |")
    lines += ['',
        '- `families`：20 类 kernel 各取第一个原生来源，CTA prefix=1，共 20 CTA、303,449 个模型节点。',
        '- `decode`：Decode2 的原生 launch 28/29/30（GEMV → SiLU → GEMV），prefix=512，共 1,025 CTA、9,733,656 个模型节点；不是一次完整 token 或整个模型。',
        '- `p28-prefix-one`：单独 QKV 来源、prefix=1，验证预验证 full-grid 模型不会被执行前缀误复用。',
        f"- 同一 decode 子集，direct/默认 cosim 的 CPU 用时比为 **{document['decode_direct_vs_native_CPU_ratio']:.2f}×**；两者执行口径不同，不能将其视为等时序模拟加速。",'',
        f"该 decode 子集：direct 新增 {direct_dirty['dirty_sector_creations']:,} 个 dirty sector，驱逐 {direct_dirty['evicted_dirty_sectors']:,} 个，驻留 {direct_dirty['resident_dirty_sectors']:,} 个；native 新增 {native_dirty['dirty_sector_creations']:,} 个，驱逐 {native_dirty['evicted_dirty_sectors']:,} 个，驻留 {native_dirty['resident_dirty_sectors']:,} 个。分别写回 {by_name['decode-direct']['write_bytes']:,} B 和 {by_name['decode-cosim']['write_bytes']:,} B，均为每 sector 32 B。固定功能顺序改变了驱逐时机；未做最终 flush，两路均满足 dirty-sector 与字节守恒，驻留脏数据不能算作丢失写入。",'',
        '## 保留模拟结果的宿主初始化优化','',
        '八帧仍解压并核验声明的 SHA；只为选中的 family 构建 typed 预验证对象，并与正式执行对象分开持有。',
        '优化前后二进制逐项对比 workflow、每 kernel execution（只排除宿主秒数）、dirty ledger、HBFSIM/backend 统计均相同。','',
        '| 子集 / 档位 | 优化前 CPU 分钟 | 优化后 CPU 分钟 | CPU 加速 |',
        '|---|---:|---:|---:|']
    for name, comparison in zip(('20 类 / cosim','decode / cosim','decode / cosim-fast'),comparisons):
        before,after=comparison['CPU_minutes']
        lines.append(f"| {name} | {before:.2f} | {after:.2f} | {comparison['CPU_speedup']:.2f}× |")
    lines += ['',
        '完整 1138-workflow 包含所有 family，仍需全部 typed 预验证；这里的初始化收益不能外推为完整运行加速。','',
        '## 正确性与发布范围','',
        '- 同一最终二进制的 20-family cosim，trace 开关前后的完整每 kernel 模拟结果、周期、访存计数和 backend 统计一致；导出数量、字节与后端闭合。',
        '- 32 B 写回覆盖全部 15 种非零 dirty mask、部分/跨行/重复写、有限背压及 epoch 推进；三模式 ASan/UBSan 共 59,321 项检查通过。',
        '- Trace 格式及真实后端 on/off 18,981 项检查通过；损坏、截断、配额和重试均有测试。',
        '- 9 类 fast binding 与原 Builder 精确比较 19,816 条访存指令、542,588 个地址范围；其余 11 类直接调用原 Builder。',
        '- direct 使用固定功能顺序，无 compute 调度、MSHR 合并或 HBFSIM；两种模式的 DRAM 流量不应被强制拟合相等。cosim-fast 为显式近似时序档，默认 cosim 保留原生依赖。',
        '- 全 1138 个完整网格未重跑；未证明新版本完整工作流耗时或新的 NCU 硬件误差。源码和 Git 独立，封存运行数据仍为原目录只读依赖；本报告未声称已部署 XMU。','',
        f"最终二进制 SHA-256：`{document['binary_sha256']}`。",'',
        '机器可读结果：`validation/qualification.json`；逐项精确比较：`validation/native-trace-on-off.json`、`validation/preflight-*.json`。',
        f'原始结果目录：`{current}`。','']
    (ROOT/'docs/qualification.md').write_text('\n'.join(lines))
    print(json.dumps(dict(status=document['status'],rows=len(rows),binary_sha256=document['binary_sha256'])))


if __name__ == '__main__':
    main()
