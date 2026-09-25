#!/usr/bin/env python3
"""One read-only rerun progress snapshot. Never opens the new result.json or extrapolates Full."""
import argparse
import datetime
import hashlib
import html
import importlib.util
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = ROOT / 'outputs/ada-gddr-p2-rerun-r1'
BASE = HERE.parent / 'gddr-full-p2-r1/progress_report.py'
BASE_SHA = '9b8ab9957b04e1f3e2f91503829d154eaac21ee663f51062c5c510bc9f037768'


def need(ok, message):
    if not ok:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text())


def base():
    need(hashlib.sha256(BASE.read_bytes()).hexdigest() == BASE_SHA, 'pinned phase snapshot reader changed')
    spec = importlib.util.spec_from_file_location('_frozen_phase_snapshot', BASE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def snapshot(run):
    need(run.resolve().parent == HERE, 'run must be a direct child of this isolated rerun directory')
    value = dict(schema='GDDR_P2_RERUN_PROGRESS_V1',
        fetched_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        qualification='LIVE_SNAPSHOT_NOT_FINAL_VALIDATION', status='NOT_STARTED',
        completed_phases=[], full_metrics=None, full_extrapolated=False,
        new_result_json_opened=False, target_seconds=3600, hard_controller_budget_seconds=7200,
        execution_deadline_seconds=7140, run_path=str(run.resolve()))
    rp = run/'receipt.json'
    if not rp.exists():
        return value
    receipt = read(rp)
    need(receipt['target_seconds'] == 3600 and receipt['max_seconds'] == 7200 and receipt['execution_deadline_seconds'] == 7140 and receipt['cleanup_reserve_seconds'] == 60, 'rerun budget identity')
    value.update(status=receipt['status'], native_receipt=str(rp),
                 native_error=receipt.get('error'), terminal_steps=receipt.get('steps', []),
                 closed_result_requires_separate_report=receipt['status'].startswith('PASS_CLOSED_'))
    pp, jp = run/'result.json.progress.json', run/'result.json.operations.jsonl'
    if not pp.exists() or not jp.exists():
        value['progress_state'] = 'NO_COMPLETED_OPERATION_PROGRESS_AVAILABLE'
        return value
    progress = read(pp)
    need(progress['status'] == 'IN_PROGRESS_NOT_FINAL', 'progress is not an original native checkpoint')
    n = progress['completed_timeline_nodes']
    need(type(n) is int and 0 < n <= 1173, 'bounded completed prefix count')
    rows, prefix = [], []
    with jp.open('rb') as f:
        for _ in range(n):
            line = f.readline()
            need(line.endswith(b'\n'), 'journal has not published the checkpoint prefix; retry')
            rows.append(json.loads(line)); prefix.append(line)
    old_run = HERE.parent/'full-measured-r1/run-r1'
    old_receipt = read(old_run/'receipt.json')
    need(old_receipt['status'] == 'PASS_CLOSED_COMPLETE_MEASURED_COLD_TUNER_V1' and old_receipt['sources_unchanged'], 'old baseline is not CLOSED')
    op = old_run/'result.json'
    expected = next(p for p in old_receipt['outputs'] if p['path'] == str(op))
    raw = op.read_bytes()
    need(len(raw) == expected['bytes'] and hashlib.sha256(raw).hexdigest() == expected['sha256'], 'closed old result changed')
    old = json.loads(raw)['rows'][:n]
    for a, b in zip(rows, old):
        need(all(a.get(k) == b.get(k) for k in ('kind','source_submission_event','source_epoch','phase','native_launch_id')), 'same completed source prefix required')
    B = base()
    phases, actual, prior = B.completed_phase_comparisons(rows, old, B.hardware_reference())
    need(actual['cycles'] == progress['cycles'] and all(actual[k] == progress['physical'][k] for k in ('read_bytes','write_bytes')), 'progress and journal counters differ; retry')
    value.update(progress_state='PUBLISHED_COMPLETED_OPERATION_PREFIX', progress=progress,
        completed_timeline_nodes=n, completed_kernels=progress['kernels'], completed_APIs=progress['APIs'],
        native_execution_seconds_at_last_completed_operation=progress['host_execution_seconds'],
        same_completed_prefix=dict(old=prior, new=actual), completed_phases=phases,
        journal_prefix=dict(bytes=sum(map(len,prefix)),sha256=hashlib.sha256(b''.join(prefix)).hexdigest(),lines=n),
        last_complete_operation={k:rows[-1].get(k) for k in ('kind','phase','owner','layer','native_launch_id')})
    return value


def render(d):
    e = html.escape
    prefix = d.get('progress', {})
    coverage = f"{d.get('completed_kernels',0)} / 1138 kernels，{d.get('completed_APIs',0)} / 29 APIs，{d.get('completed_timeline_nodes',0)} / 1173 个时间线节点"
    elapsed = d.get('native_execution_seconds_at_last_completed_operation')
    elapsed_text = '尚无完成操作' if elapsed is None else f'{elapsed/60:.2f} 分钟（截至最后已完成操作，不含当前在途操作）'
    tr = []
    for item in d['completed_phases']:
        old, new, hw = item['old'], item['new'], item['NCU_reference']
        hw_text = '未知' if hw is None else f"{float(hw['write_mean_bytes'])/1e6:.2f} MB"
        bw = '未知' if new['effective_read_write_GB_s'] is None else f"{new['effective_read_write_GB_s']:.2f} GB/s"
        tr.append(f"<tr><td>{e(item['phase'])}</td><td>{new['read_bytes']/1e9:.2f} GB</td><td>{old['write_bytes']/1e3:.2f} / {new['write_bytes']/1e3:.2f} KB</td><td>{hw_text}</td><td>{new['modeled_duration_ns']/1e6:.2f} ms</td><td>{bw}</td></tr>")
    if not tr:
        tr.append('<tr><td colspan="6">尚无成对 epoch_begin/end 的完整阶段。</td></tr>')
    terminal = ''
    if d.get('native_error'):
        terminal = '<p class="warn">原生运行失败：'+e(d['native_error'])+'。此页不补算未完成结果；终止及清理证据需另行审核。</p>'
    elif d.get('closed_result_requires_separate_report'):
        terminal = '<p class="warn">控制器已报告 CLOSED；此进度页仍未读取最终 result.json。完整结果请等待独立 CLOSED 比较器生成。</p>'
    return f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>17.10 Gb/s 重跑进度</title><style>body{{font:16px/1.6 system-ui;max-width:1100px;margin:32px auto;padding:0 20px;background:#f5f7fa;color:#203044}}section{{background:white;padding:22px;margin:20px 0;border-radius:10px}}table{{width:100%;border-collapse:collapse}}th,td{{padding:10px;border-bottom:1px solid #d7e0e8;text-align:right}}th:first-child,td:first-child{{text-align:left}}.warn{{background:#fff0d4;padding:16px}}small{{color:#607080}}a{{color:#165fa0}}</style>
<h1>17.10 Gb/s · 完整 8B B1/P32/D2 重跑</h1><section><strong>{e(d['status'])}</strong><p>{e(coverage)}</p><p>{e(prefix.get('phase','尚未发布阶段'))}</p><p>原生已完成前缀耗时：{e(elapsed_text)}</p><p>目标仍为60.00分钟；执行截止119.00分钟，含清理的硬上限120.00分钟。输入、binary、缓存与17.10配置均沿用原实验，仅宿主时限调整。</p>{terminal}</section>
<section><h2>已闭合阶段快照</h2><p class="warn">仅列成对阶段边界；不是完整运行最终验证。不外推 Full 流量、时间、带宽或完成时间，也不作 CPU 速度比较。</p><table><tr><th>阶段</th><th>新读</th><th>18.00旧写 / 本轮写</th><th>NCU写均值</th><th>本轮模型时长</th><th>本轮带宽</th></tr>{''.join(tr)}</table><small>模型时长取操作周期×40000/87 ps。NCU PF是直接prefix；D1/D2是独立cold-prefix差分估计，不是同次直接阶段计数，经验区间不是置信区间。JSON保留精确字节和边界。</small></section>
<section><a href="progress.json">快照JSON</a> · <a href="../ada-gddr-p2-partial-r1/index.html">上一轮未完成结果</a> · <a href="../ada-phase-traffic-r1/index.html">NCU基线</a><p><small>快照时间：{e(d['fetched_utc'])}。此页不会自动获取新进度；需再次运行读取命令。本读取器不打开新result.json，不执行模拟或GPU工作。</small></p></section></html>'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run',type=Path,default=HERE/'run-r1')
    args=parser.parse_args()
    d=snapshot(args.run.resolve())
    B=base();OUT.mkdir(parents=True,exist_ok=True)
    B.atomic_text(OUT/'progress.json',json.dumps(d,ensure_ascii=False,indent=2)+'\n')
    B.atomic_text(OUT/'index.html',render(d))
    print(json.dumps(dict(status=d['status'],kernels=d.get('completed_kernels'),phase=d.get('progress',{}).get('phase'),
        completed_phases=[dict(phase=p['phase'],read_bytes=p['new']['read_bytes'],write_bytes=p['new']['write_bytes'],
                              old_write_bytes=p['old']['write_bytes'],NCU_write_mean_bytes=p['NCU_reference']['write_mean_bytes'] if p['NCU_reference'] else None,
                              duration_ns=p['new']['modeled_duration_ns'],bandwidth_GB_s=p['new']['effective_read_write_GB_s']) for p in d['completed_phases']],
        full_extrapolated=False,new_result_json_opened=False)))


if __name__=='__main__':
    main()
