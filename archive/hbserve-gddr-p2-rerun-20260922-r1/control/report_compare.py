#!/usr/bin/env python3
"""CLOSED-only rerun wrapper; original model/result/NCU validators stay byteexact."""
import argparse
import copy
import hashlib
import html
import importlib.util
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
BASE = HERE.parent / 'gddr-full-p2-r1/report_compare.py'
BASE_SHA = 'bde92de92251ed87c5a1706285d9b27abdbb2afc25a4ff5bfc7ce76427418c9f'
MANIFEST = HERE / 'host-budget.json'
MANIFEST_SHA = 'b52bd3f0514b1ca50ab69ee3462d0086d17adf891e55fe13f742afae23ba8a3c'
NEW_BUDGET = dict(target_seconds=3600, max_seconds=7200, execution_deadline_seconds=7140, cleanup_reserve_seconds=60)
OLD_BUDGET = dict(target_seconds=3600, max_seconds=5400, execution_deadline_seconds=5340, cleanup_reserve_seconds=60)


def load_base():
    if hashlib.sha256(BASE.read_bytes()).hexdigest() != BASE_SHA:
        raise ValueError('original saved validator source changed')
    spec = importlib.util.spec_from_file_location('_frozen_gddr_comparison', BASE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def install_rerun_provenance(C):
    """Change only host budget assertion and preparation provenance, never model gates."""
    original_gate = C.receipt_gate

    def receipt_gate(rec, expected_status):
        if expected_status == C.OLD_STATUS:
            return original_gate(rec, expected_status)
        # Reject RUNNING/FAIL before opening any new result, journal or plan.
        C.need(expected_status == C.NEW_STATUS and rec.get('status') == C.NEW_STATUS,
               'rerun is not CLOSED PASS; new result must remain unread')
        C.need({k: rec[k] for k in NEW_BUDGET} == NEW_BUDGET, 'exact authorized rerun host budget')
        mp = C.pin(MANIFEST)
        C.need(mp['sha256'] == MANIFEST_SHA, 'frozen host manifest changed')
        m = C.read(MANIFEST)
        C.need(m['schema'] == 'GDDR_P2_SAME_INPUT_HOST_BUDGET_OVERRIDE_V1'
               and m['original_budget'] == OLD_BUDGET and m['effective_host_budget'] == NEW_BUDGET
               and m['model_changes'] == [] and m['compile_executed'] is False, 'host-only override provenance')
        expected = dict(manifest=mp, effective_host_budget=NEW_BUDGET,
                        original_budget=OLD_BUDGET, controller_pins=m['controller_pins'])
        C.need(rec['host_budget_override'] == expected and rec['admission'] == m['original_admission'],
               'actual rerun receipt/host override/original admission join')
        for q in [m['original_admission'], m['original_controller'], m['original_contract']] + m['controller_pins']:
            C.checked(q)
        C.need((HERE/'contract.py').read_bytes() == Path(m['original_contract']['path']).read_bytes(),
               'original result/plan contract must remain byteexact')
        admission = C.read(m['original_admission']['path'])
        C.need(rec['pins'] == admission['pins'] and rec['plan'] == admission['plan']
               and rec['binary'] == admission['binary'], 'all original frozen model provenance retained')
        # A local copy only permits the original fixed-budget assertion to run.
        # The actual receipt is untouched; its real 7200/7140 values are checked above.
        projection = copy.deepcopy(rec)
        projection['max_seconds'] = 5400
        original_gate(projection, expected_status)

    def preparation(new_run, admission_pin, host):
        C.need(Path(new_run).resolve() == HERE/'run-r1', 'only this isolated rerun is accepted')
        p = HERE/'source-checks.json'
        doc = C.read(p)
        C.need(doc['status'] == 'PASS_SOURCE_AND_SAVED_PREFLIGHT_ONLY'
               and doc['native_executed'] is False and doc['compiler_executed'] is False,
               'source-only rerun preflight record')
        for q in doc['pins']:
            C.checked(q)
        C.need(admission_pin in doc['pins'], 'source preflight/original admission join')
        seconds = C.finite(doc['elapsed_preflight_seconds'])
        return dict(receipt=C.pin(p), fresh_plan_rebinding=False, fresh_compilation=False,
                    measured_source_preflight_seconds=seconds,
                    source_preflight_is_entire_preparation=False,
                    full_external_preparation_seconds=None,
                    preparation_plus_owned_seconds=None,
                    preparation_plus_owned_3600s_target_met=None,
                    owned_run_seconds=host['owned_total_seconds'],
                    historical_plan_preparation_added=False,
                    interpretation='Original plan/input/binary reused. Only source preflight is measured; complete new external preparation is unknown. Old 1.597s preparation is not added.')

    C.receipt_gate = receipt_gate
    C.preparation = preparation


def build(new_run):
    C = load_base()
    C.need(Path(new_run).resolve() == HERE/'run-r1', 'only the authorized same-directory run-r1 is accepted')
    install_rerun_provenance(C)
    # Includes original 1173 timeline/1138 kernel/29 API work, physical completion,
    # dirty I/C/E/F, passive writer, async/ticket/HBF closure and saved NCU checks.
    result = C.build(new_run)
    result['schema'] = 'GDDR_P2_RERUN_COMPLETE_MEASURED_COMPARISON_V1'
    result['status'] = 'PASS_CLOSED_SAVED_GDDR_P2_RERUN_COMPARISON'
    result['new']['host'].update(hard_budget_seconds=7200, execution_deadline_seconds=7140,
                               cleanup_reserve_seconds=60)
    result['host_budget_override'] = C.read(MANIFEST)
    result['compatibility'] = dict(original_comparator=C.pin(BASE),
        original_model_result_hardware_validators_reused=True,
        changed_validation_scope=['exact authorized host budget/provenance', 'external preparation provenance'],
        original_receipts_modified=False, failed_prior_run_substituted=False,
        receipt_projection_only_for_original_max_seconds_assertion=True)
    result['limitations'][-1] = ('Owned wall time includes runner validation. Native preparation/execution/finalization '
        'are separate. Source preflight is a limited measured preparation item; complete external preparation is unknown. '
        'No historical preparation or build cost is silently reused; host speed comparisons are not made.')
    result['source_pins'] += [C.pin(__file__), C.pin(MANIFEST)] + C.read(MANIFEST)['controller_pins']
    # Recheck all original source pins, not just the report's directly used files.
    rec = C.read(Path(new_run)/'receipt.json')
    result['source_pins'] += rec['pins']
    unique = {}
    for q in result['source_pins']:
        C.need(q['path'] not in unique or unique[q['path']] == q, 'conflicting source pin')
        unique[q['path']] = q
    result['source_pins'] = list(unique.values())
    for q in result['source_pins']:
        C.checked(q)
    return result


def render(d):
    h = d['new']['host']
    lines = ['# 17.10 Gb/s 完整冷入口 Measured 重跑', '',
        f"全部 1138 kernels、29 APIs、6 阶段边界闭合。宿主总耗时 **{h['owned_total_minutes']:.2f} 分钟**；60.00 分钟目标 **{'达到' if h['owned_target_3600s_met'] else '未达到'}**。执行截止119.00分钟，含清理硬上限120.00分钟。", '',
        '复用原 binary、输入与17.10配置。与18.00基线相比仅数据率配置改变；本次重跑相对首次17.10仅调整宿主时限。初始化/Warmup未模拟，无末尾dirty flush。', '',
        '| 阶段 | 旧读 / 新读 GB | 旧写 / 新写 KB | NCU写均值 MB | 新写相对NCU |',
        '|---|---:|---:|---:|---:|']
    for r in d['comparison']:
        rd, wr = r['metrics']['dram_read_bytes'], r['metrics']['dram_write_bytes']
        err = wr['new_vs_hardware_mean_percent']
        err_text = '未知' if err is None else f'{float(err):+.2f}%'
        lines.append(f"| {r['phase']} | {float(rd['old'])/1e9:.2f} / {float(rd['new'])/1e9:.2f} | {float(wr['old'])/1e3:.2f} / {float(wr['new'])/1e3:.2f} | {float(wr['hardware_mean'])/1e6:.2f} | {err_text} |")
    lines += ['', 'NCU PF和Full为直接cold-prefix计数；D1/D2是3组独立prefix差分估计，不能称同次直接阶段计数。JSON保留经验区间；它们不是置信区间。Full接近不能掩盖Decode写回差异。', '',
              '| 阶段 | 旧 / 新模型时间 ms | 旧 / 新带宽 GB/s |', '|---|---:|---:|']
    for r in d['comparison']:
        t, bw = r['metrics']['modeled_duration_ns'], r['metrics']['effective_read_write_GB_s']
        lines.append(f"| {r['phase']} | {float(t['old'])/1e6:.2f} / {float(t['new'])/1e6:.2f} | {bw['old']:.2f} / {bw['new']:.2f} |")
    prep = d['new']['external_preparation_and_owned_total']
    lines += ['', f"原生准备 {h['native_preparation_seconds']:.2f}s，执行 {h['native_execution_seconds']:.2f}s，收尾 {h['native_finalization_seconds']:.2f}s；独立SOURCE预检 {prep['measured_source_preflight_seconds']:.2f}s仅是准备的一部分。完整外部准备耗时未测，不借用旧准备值，不作CPU加速结论。", '',
        '固定2175MHz模型周期与NCU ROI时间口径仍有差别；17.10不是完整GDDR校准，R4缓存策略未移植。所有逻辑工作、完整请求/响应、dirty I+C=E+F和32B写回、writer边际及原source pins均复核。', '',
        '[精确数据与来源](data.json) · [闭合receipt](receipt.json)', '']
    return '\n'.join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--new-run', type=Path, default=HERE/'run-r1')
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.output.exists():
        raise ValueError('fresh output directory required')
    try:
        result = build(args.new_run)
    except (ValueError, KeyError, FileNotFoundError) as error:
        print(json.dumps(dict(status='BLOCKED_NO_COMPLETE_COMPARISON', error=str(error), output_created=False)))
        return 2
    args.output.mkdir(parents=True, exist_ok=False)
    md = render(result)
    (args.output/'data.json').write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False)+'\n')
    (args.output/'report.md').write_text(md)
    # A simple readable artifact with genuine links; no browser-side data fetch or polling.
    (args.output/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>17.10 Gb/s CLOSED comparison</title><style>body{max-width:1200px;margin:30px auto;font:16px/1.6 system-ui}pre{white-space:pre-wrap}</style><p><a href="data.json">精确数据与来源</a> · <a href="report.md">报告</a> · <a href="receipt.json">闭合receipt</a></p><pre>'+html.escape(md)+'</pre>')
    C = load_base()
    artifacts = [C.pin(args.output/name) for name in ('data.json','report.md','index.html')]
    (args.output/'receipt.json').write_text(json.dumps(dict(status=result['status'], artifacts=artifacts,
        source_pins=result['source_pins']), ensure_ascii=False, indent=2)+'\n')
    print(json.dumps(dict(status=result['status'], output=str(args.output.resolve()))))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
