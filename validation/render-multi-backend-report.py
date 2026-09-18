#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Render audited, already-completed multi-backend replay results; no simulation.

Only Python's standard library is required. All plotted values come from receipts.
"""
from __future__ import annotations

import argparse
import csv
import decimal
import hashlib
import html
import io
import json
import math
import statistics
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO.parents[2] / 'outputs/tilegen-stage-backends-20260918/index.html'
NAMES = ('gddr6', 'hbm', 'hbf')
LABELS = {'gddr6': 'GDDR6', 'hbm': 'HBM', 'hbf': 'HBF'}
COLORS = {'gddr6': '#147b83', 'hbm': '#5665b6', 'hbf': '#b16b3b'}


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def load(path):
    return json.loads(Path(path).read_text())


def e(value):
    return html.escape(str(value), quote=True)


def f(value, places=2):
    return f'{value:,.{places}f}'


def payload(value):
    """Keep large payloads in GB and small payloads in MB."""
    return f(value/1e9) + ' GB' if abs(value) >= 1e9 else f(value/1e6) + ' MB'


def payload_pair(mean, sd):
    divisor, unit = (1e9, 'GB') if abs(mean) >= 1e9 else (1e6, 'MB')
    return f(mean/divisor) + ' ± ' + f(sd/divisor) + ' ' + unit


def link(path, title):
    return f'<a href="{e(Path(path).resolve().as_uri())}">{e(title)}</a>'


def table(headings, rows, cls=''):
    return ('<div class="table-scroll"><table class="' + cls + '"><thead><tr>'
            + ''.join(f'<th>{x}</th>' for x in headings) + '</tr></thead><tbody>'
            + ''.join('<tr>' + ''.join(f'<td>{x}</td>' for x in row) + '</tr>' for row in rows)
            + '</tbody></table></div>')


def verify(root):
    runs = {}
    for name in NAMES:
        base = root / name
        r, receipt = load(base / 'result.json'), load(base / 'run-receipt.json')
        assert receipt['status'] == 'PASS' and receipt['returncode'] == 0
        assert r['status'] == 'PASS_CLOSED_STAGE_OVERLAP_REPLAY'
        assert sha(base / 'result.json') == receipt['result_sha256'], name + ': result hash'
        assert sha(base / 'replay-input.json') == receipt['input_manifest_sha256']
        cfg = base / ('native-memory.cfg' if name == 'gddr6' else 'target-memory.cfg')
        assert sha(cfg) == r['target_backend']['config_sha256'], name + ': target config hash'
        assert r['accepted'] == r['completed'] == r['requests'] == r['trace']['records']
        assert r['request_payload_fnv1a64'] == r['native_request_payload_fnv1a64'] == r['trace']['request_payload_fnv1a64']
        assert all(r[x] for x in ['byte_ledger_closed', 'request_ledger_closed', 'stage_ledger_closed', 'kernel_barriers_enforced'])
        assert r['final_live_requests'] == r['final_active_stages'] == 0
        assert r['makespan_cycles'] == r['stage_makespan_cycles'] + r['persistence_tail_cycles']
        assert math.isclose(r['aggregate_bandwidth_GBps'], (r['read_bytes'] + r['write_bytes']) / r['makespan_ns'])
        assert sum(c['read_bytes'] for c in r['calls']) == r['read_bytes']
        assert sum(c['write_bytes'] for c in r['calls']) == r['write_bytes']
        assert sum(c['makespan_cycles'] for c in r['calls']) == r['stage_makespan_cycles']
        assert sha(base / 'source-result.json') == load(base / 'source-run-receipt.json')['result_sha256']
        runs[name] = (r, receipt, base)
    first = runs[NAMES[0]][0]
    for name in NAMES[1:]:
        r = runs[name][0]
        for key in ['read_bytes', 'write_bytes', 'read_requests', 'write_requests', 'request_payload_fnv1a64',
                    'clock', 'selected_CTAs', 'selected_source_count', 'batch_size', 'phase_profile_sha256', 'window_stages']:
            assert r[key] == first[key], (name, key)
        assert r['trace']['file_sha256'] == first['trace']['file_sha256']
        assert r['trace']['context_sha256'] == first['trace']['context_sha256']
    # This report is deliberately scoped to the frozen three-kernel fragment.
    assert first['selected_source_count'] == 3 and first['selected_CTAs'] == 1025 and first['batch_size'] == 1
    assert [c['source_launch_key'] for c in first['calls']] == ['epoch-3-launch-28', 'epoch-3-launch-29', 'epoch-3-launch-30']
    assert [c['phase'] for c in first['calls']] == ['Decode2'] * 3
    assert first['window_stages'] == 2
    assert runs['hbf'][0]['physical']['final_quiescent'] is True
    return runs


def svg_start(title, desc, width=1000, height=370):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" role="img" aria-label="{e(title)}" viewBox="0 0 {width} {height}">',
            f'<title>{e(title)}</title><desc>{e(desc)}</desc>',
            '<style>text{font-family:Arial,"PingFang SC",sans-serif;fill:#23344b;font-size:14px}.small{font-size:12px;fill:#52627a}.axis{stroke:#dce4ec;stroke-width:1}</style>',
            f'<rect width="{width}" height="{height}" fill="#fff" rx="14"/>']


def log_comparison(runs):
    title = '同一 trace：模型总时间与逻辑带宽'
    s = svg_start(title, '横轴为对数刻度，点标注精确数值；总时间包含 HBF EOF 持久化尾部。', height=330)
    panels = [(25, '总时间 ms（含 EOF 尾部）', -2, 2, lambda r: r['makespan_ns'] / 1e6, 'ms'),
              (520, '逻辑 R+W / 总时间 · GB/s', 0, 4, lambda r: r['aggregate_bandwidth_GBps'], 'GB/s')]
    for left, subtitle, low, high, value, unit in panels:
        s.append(f'<text x="{left}" y="30" font-weight="700">{subtitle}</text>')
        x0, x1 = left + 76, left + 414
        for power in range(low, high + 1):
            x = x0 + (power - low) / (high - low) * (x1 - x0)
            s.append(f'<line class="axis" x1="{x}" y1="56" x2="{x}" y2="237"/><text class="small" x="{x}" y="264" text-anchor="middle">{10**power:g}</text>')
        for i, name in enumerate(NAMES):
            y, v = 91 + i * 66, value(runs[name][0])
            x = x0 + (math.log10(v) - low) / (high - low) * (x1 - x0)
            s.append(f'<text x="{left}" y="{y+5}">{LABELS[name]}</text><circle cx="{x}" cy="{y}" r="6" fill="{COLORS[name]}"/>')
            label_x = max(x0, min(x, x1 - 25))
            s.append(f'<text x="{label_x}" y="{y-15}" text-anchor="middle" font-weight="700">{f(v, 3)} {unit}</text>')
    s.append('<text class="small" x="25" y="307">对数刻度；HBF 结果包含页读放大和当前控制器策略，不代表接口峰值或实际 LLM 延迟。</text></svg>')
    return ''.join(s)


def payload_chart(runs):
    s = svg_start('相同逻辑输入与不同物理 payload', '左右分别读、写；各面板线性刻度。HBF 的物理单位为 NAND 页 payload。', height=400)
    for left, key, title in [(25, 'read', '读取 · MB'), (520, 'write', '写入 · MB')]:
        maximum = max(runs[n][0][f'physical_payload_{key}_bytes'] / 1e6 for n in NAMES) * 1.10
        x0, width = left + 83, 295
        s.append(f'<text x="{left}" y="30" font-weight="700">{title}</text>')
        for frac in (0, .5, 1):
            x = x0 + frac * width
            s.append(f'<line class="axis" x1="{x}" y1="54" x2="{x}" y2="296"/><text class="small" x="{x}" y="316" text-anchor="middle">{f(maximum*frac, 2)}</text>')
        for i, name in enumerate(NAMES):
            r, y = runs[name][0], 76 + 77 * i
            s.append(f'<text x="{left}" y="{y+22}">{LABELS[name]}</text>')
            for j, (field, color) in enumerate([(f'{key}_bytes', '#b8c6d5'), (f'physical_payload_{key}_bytes', COLORS[name])]):
                value, yy = r[field] / 1e6, y + j * 25
                w = value / maximum * width
                s.append(f'<rect x="{x0}" y="{yy}" width="{max(.8,w)}" height="15" rx="3" fill="{color}"/><text x="{x0+w+6}" y="{yy+12}" class="small">{f(value,2)}</text>')
    s.append('<rect x="25" y="349" width="15" height="12" fill="#b8c6d5"/><text class="small" x="48" y="360">上条：共同逻辑输入</text><text class="small" x="265" y="360">下条：各后端物理 payload；读、写面板尺度不同。</text>')
    s.append('<text class="small" x="25" y="384">HBF：Read 31.98×；Write 1.57×。源 32B 写由页级 write buffer 合并，非每条都产生一次 4KB program。</text></svg>')
    return ''.join(s)


def kernel_chart(runs):
    s = svg_start('三个 kernel 片段的逻辑带宽', '每个 kernel 的源 R+W 字节除以含 stage compute 的调用区间，HBF EOF 尾部未分摊。', height=335)
    low, high, x0, width = -1, 4, 235, 660
    for p in range(low, high + 1):
        x = x0 + (p-low)/(high-low)*width
        s.append(f'<line class="axis" x1="{x}" y1="40" x2="{x}" y2="260"/><text class="small" x="{x}" y="287" text-anchor="middle">{10**p:g}</text>')
    for i, family in enumerate(['GEMV · gate/up', 'SiLU', 'GEMV · down']):
        y = 70 + i * 76
        s.append(f'<text x="25" y="{y+9}" font-weight="700">{family}</text>')
        for j, name in enumerate(NAMES):
            value = runs[name][0]['calls'][i]['aggregate_bandwidth_GBps']
            x = x0 + (math.log10(value)-low)/(high-low)*width
            yy = y + (j-1)*18
            s.append(f'<circle cx="{x}" cy="{yy}" r="4" fill="{COLORS[name]}"/><text x="{x+8}" y="{yy+4}" class="small">{LABELS[name]} {f(value,2)}</text>')
    s.append('<text class="small" x="25" y="318">GB/s，对数横轴；每个 GEMV 仅前 512 CTA。色彩表示后端，未声称按 weights / activation / KV 归属。</text></svg>')
    return ''.join(s)


def verify_discovery(root):
    """Validate native discovery controls without treating profiler timing as NCU."""
    controller = load(root / 'controller.json')
    finishes = list((root / 'validate/host').glob('process-*/finish.json'))
    assert len(finishes) == 1, 'discovery requires one frozen natural timing receipt'
    finish_path = finishes[0]
    finish = load(finish_path)
    artifacts = root / 'metadata/artifacts'
    manifest = load(artifacts / 'manifest.json')
    assert controller['status'] == 'PASS_CAPTURE_CLOSURE_NOT_MODEL_QUALIFICATION'
    assert finish['status'] == 'PASS_NATIVE_WORKFLOW_AND_ROI' and manifest['status'] == 'COMPLETE'
    assert sha(artifacts / 'manifest.json') == controller['metadata_manifest_sha256']
    contract = controller['input_contract']
    assert contract == finish['input_contract'] == manifest['input_contract']
    raw_contract = {k: v for k, v in contract.items() if k != 'sha256'}
    assert hashlib.sha256(json.dumps(raw_contract, sort_keys=True, separators=(',', ':')).encode()).hexdigest() == contract['sha256']
    assert (contract['batch_size'], contract['prefill_length'], contract['decode_steps'], contract['layers']) == (1, 1024, 32, 32)
    phases = ['Prefill'] + [f'Decode{i}' for i in range(1, 33)]
    assert contract['phases'] == finish['natural_phases'] == controller['metadata_census']['phase_markers'] == phases
    assert manifest['stage_files'] == [x + '.json' for x in phases]
    assert set(finish['natural_cuda_event_ms']) == set(phases)
    assert not finish['event_time_is_NCU_duration'] and not finish['profiler_api_invoked']
    assert not finish['predictions_fed_back'] and not controller['full_dynamic_trace_collected']
    assert finish['source_unchanged'] and manifest['native_source_unchanged']
    assert finish['source_identity'] == manifest['native_source_files']
    package = REPO / 'capture/p1024d32'
    assert sha(package / 'upload-manifest.json') == controller['package']['manifest_sha256']
    for row in controller['package']['files']:
        path = package / row['path']
        assert path.stat().st_size == row['bytes'] and sha(path) == row['sha256'], row['path']
    reference = load(package / 'native-source-reference.json')
    expected_sources = {r['relative']: r['sha256'] for r in reference['files']}
    assert {r['relative']: r['sha256'] for r in finish['source_identity']} == expected_sources
    assert sha(package / 'native_ncu_driver.py') == finish['driver_sha256']
    assert sha(package / 'sglang_driver.py') == finish['reference_driver_sha256'] == manifest['driver_sha256']
    files = load(artifacts / 'files.sha256.json')
    for row in files:
        path = artifacts / row['path']
        assert path.stat().st_size == row['bytes'] and sha(path) == row['sha256'], row['path']
    assert len(finish['control_values']) == len(phases)
    rows = []
    for index, phase in enumerate(phases):
        control = finish['control_values'][index]
        metadata = load(artifacts / (phase + '.json'))
        actual = metadata['actual_forward_batch']
        expected_ids = list(range(1000, 2024)) if index == 0 else [(944, 291)[(index - 1) % 2]]
        positions = list(range(1024)) if index == 0 else [1023 + index]
        slots = list(range(1, 1025)) if index == 0 else [1024 + index]
        assert control['phase'] == metadata['phase'] == phase
        assert metadata['forward_index'] == index and metadata['batch_size'] == 1
        assert control['input_ids'] == actual['input_ids'] == metadata['fixed_input_ids'] == expected_ids
        assert control['positions'] == actual['positions'] == positions
        assert control['out_cache_loc'] == actual['out_cache_loc'] == slots
        assert control['seq_lens_sum'] == sum(actual['seq_lens']) == 1024 + index
        assert metadata['kv_token_slots'] == list(range(1, 1025 + index))
        assert not metadata['predictions_fed_back'] and not metadata['cuda_graph_used']
        elapsed = finish['natural_cuda_event_ms'][phase]
        assert math.isfinite(elapsed) and elapsed > 0
        rows.append({'phase': phase, 'index': index, 'input_count': len(expected_ids), 'context': 1024 + index,
                     'event_ms': elapsed, 'metadata_path': artifacts / (phase + '.json')})
    kernels = load(artifacts / 'kernel_launches.json')
    assert kernels['kernel_count'] == controller['metadata_census']['recorded_kernels']
    assert not kernels['memory_instruction_trace']
    modules, roots = load(artifacts / 'module_calls.json'), load(artifacts / 'tensor_roots.json')
    assert set(x['phase'] for x in modules) == set(phases)
    process_rows = [load(root / name / 'process.json') for name in ['validate', 'metadata']]
    assert all(x['returncode'] == 0 and x['owned_processes_drained'] and x['gpu_quiescent'] for x in process_rows)
    return {'root': root, 'controller': controller, 'finish': finish, 'finish_path': finish_path,
            'manifest': manifest, 'rows': rows, 'files': files, 'module_count': len(modules),
            'root_count': len(roots), 'kernel_count': kernels['kernel_count'], 'event_count': len(kernels['events']),
            'source_count': len(expected_sources), 'processes': process_rows}


def discovery_chart(capture):
    rows = capture['rows']
    s = svg_start('P1024D32：33 阶段自然 CUDA event 时间', '单次非 NCU event 计时，包含阶段内 host launch 间隙；左面板 Prefill，右面板 32 Decode，共同零点但量程不同。', height=425)
    s.append('<text x="25" y="29" font-weight="700">Prefill（0–220 ms）</text><text x="270" y="29" font-weight="700">Decode1–32（0–60 ms，单次观测）</text>')
    for x0, width, limit, ticks in [(75, 115, 220, [0, 100, 200]), (290, 675, 60, [0, 20, 40, 60])]:
        for tick in ticks:
            y = 325 - tick / limit * 260
            s.append(f'<line class="axis" x1="{x0}" y1="{y}" x2="{x0+width}" y2="{y}"/><text class="small" x="{x0-8}" y="{y+4}" text-anchor="end">{tick}</text>')
    h = rows[0]['event_ms'] / 220 * 260
    s.append(f'<rect x="105" y="{325-h}" width="55" height="{h}" rx="4" fill="#147b83"/><text x="132.5" y="{315-h}" text-anchor="middle" font-weight="700">{f(rows[0]["event_ms"])} ms</text><text x="132.5" y="351" text-anchor="middle">Prefill</text>')
    stride = 675 / 32
    for i, row in enumerate(rows[1:]):
        x, h = 290 + i * stride, row['event_ms'] / 60 * 260
        s.append(f'<rect x="{x+3}" y="{325-h}" width="{stride-6}" height="{h}" rx="2" fill="#5665b6"><title>{e(row["phase"])}: {row["event_ms"]:.9f} ms</title></rect>')
        if i % 4 == 0 or i == 31:
            s.append(f'<text class="small" x="{x+stride/2}" y="350" text-anchor="middle">D{i+1}</text>')
    decode = [r['event_ms'] for r in rows[1:]]
    s.append(f'<text class="small" x="270" y="382">Decode 范围 {f(min(decode))}–{f(max(decode))} ms；所有阶段精确来源见下表链接。</text>')
    s.append('<text class="small" x="25" y="408">两个面板量程不同；不是 NCU kernel duration，也没有 DRAM 字节分子，不能据此计算 NCU 带宽。</text></svg>')
    return ''.join(s)


def read_ncu(root, capture):
    """Only a closed three-group campaign is eligible for published statistics."""
    controller_path = root / 'controller.json'
    if not controller_path.exists():
        return {'ready': False, 'root': root, 'status': 'PENDING_FORMAL_THREE_GROUP_RECEIPT'}
    controller = load(controller_path)
    if controller.get('status') != 'PASS_CAPTURE_CLOSURE_NOT_MODEL_QUALIFICATION':
        return {'ready': False, 'root': root, 'status': controller.get('status', 'UNKNOWN')}
    assert controller['mode'] == 'ncu' and controller['groups'] == 3, 'a pilot is not the final three-group campaign'
    assert controller['input_contract'] == capture['controller']['input_contract']
    assert controller['package'] == capture['controller']['package']
    assert controller['GPU_UUID'] == capture['controller']['GPU_UUID']
    assert {x['path']: x['sha256'] for x in controller['model_identity']} == {x['path']: x['sha256'] for x in capture['controller']['model_identity']}
    scopes = ['full', 'Prefill', 'Decode1', 'Decode8', 'Decode16', 'Decode32']
    assert controller['scopes'] == scopes
    assert len(controller['samples']) == 3 * len(scopes)
    seen, samples, hosts_with_cuda_events = set(), [], 0
    keys = ('dram__bytes_read.sum', 'dram__bytes_write.sum', 'gpu__time_duration.sum')
    for sample in controller['samples']:
        group, roi = sample['group'], sample['roi']
        assert group in (1, 2, 3) and roi in scopes and (group, roi) not in seen
        seen.add((group, roi))
        directory = root / f'group-{group}-{roi}'
        report, imported = directory / 'capture.ncu-rep', directory / 'import/stdout.log'
        assert sha(report) == sample['report_sha256']
        assert sha(imported) == sample['csv_sha256']
        lines = imported.read_text().splitlines()
        begin = next(i for i, line in enumerate(lines) if line.startswith('"ID","Process ID"'))
        rows = list(csv.reader(io.StringIO('\n'.join(lines[begin:]))))
        assert len(rows) == 3
        header, units, values = rows
        assert len(header) == len(set(header)) == len(units) == len(values)
        row, unit = dict(zip(header, values)), dict(zip(header, units))
        assert row['Kernel Name'] == 'range'
        counters = sample['counters']
        assert counters['bytes_and_time_same_app_range'] is True
        assert int(row['Process ID']) == counters['process_id'] and row['ID'] == counters['action_id']
        metrics = []
        for key, expected_unit in zip(keys, ('byte', 'byte', 'ns')):
            assert unit[key] == expected_unit
            value = decimal.Decimal(row[key].replace(',', ''))
            assert value.is_finite() and value >= 0 and value == decimal.Decimal(str(counters['metrics'][key]))
            if expected_unit == 'byte':
                assert value == value.to_integral_value()
            else:
                assert value > 0
            metrics.append(float(value))
        hosts = list((directory / 'host').glob('process-*/finish.json'))
        assert len(hosts) == sample['host_replays'] and hosts
        pids = set()
        for host_path in hosts:
            host = load(host_path)
            hosts_with_cuda_events += bool(host.get('natural_cuda_event_ms'))
            assert host['status'] == 'PASS_NATIVE_WORKFLOW_AND_ROI'
            assert host['input_contract'] == controller['input_contract'] and host['roi'] == roi
            assert host['natural_phases'] == controller['input_contract']['phases']
            assert host['source_unchanged'] and host['source_identity'] == capture['finish']['source_identity']
            assert host['driver_sha256'] == capture['finish']['driver_sha256']
            pids.add(host['pid'])
        assert counters['process_id'] in pids
        for process_path in [directory / 'process.json', directory / 'import/process.json']:
            process = load(process_path)
            assert process['status'] == 'PASS_OWNED_PROCESS' and process['returncode'] == 0
            assert process['owned_processes_drained']
        read, write, ns = metrics
        samples.append({'group': group, 'roi': roi, 'read': read, 'write': write, 'ns': ns,
                        'bandwidth': (read + write) / ns, 'host_replays': sample['host_replays'],
                        'csv': imported, 'report': report, 'csv_sha256': sample['csv_sha256']})
    return {'ready': True, 'root': root, 'controller': controller, 'samples': samples, 'scopes': scopes,
            'status': controller['status'], 'hosts_with_cuda_events': hosts_with_cuda_events}


def ncu_section(ncu, capture):
    parts = ['<h3 id="ncu-formal-results">NCU：正式三组结果</h3>']
    if not ncu['ready']:
        parts.append('<p class="note">Pilot / 正式采集流程进行中；尚未收到完整 PASS 的三组 controller receipt。此处保持 pending，不把 pilot 的部分 scope 当作最终三组结果。只有正式完整 receipt 通过契约、source、每次 app-range CSV、host replay 和 report SHA 校验后才展示统计。</p>')
        return '\n'.join(parts)
    parts += ['<p>以下为同一冻结 P1024D32 输入的三组独立 app-range 采样，每个 scope n=3。R / W / duration 来自同一次 action；带宽逐次先算 (R+W)/duration。标准差为样本标准差（ddof=1）。Full 与各阶段来自独立运行，不能将独立阶段 NCU 结果相加替代 Full。</p>',
              '<p><strong>不可用 Full−Prefill 推导 Decode 总量或再除以 32。</strong>这些 scope 的运行和 profiling 测量窗口独立；Decode1 写量也不能默认等于后续稳态 Decode。若独立阶段写量之和与 Full 差额不同，当前数据只建立观测差异，不能在缺少 cache 状态 / 边界计数证据时宣称已经由 L2 脏行或写回机制解释。</p>']
    rows = []
    for scope in ncu['scopes']:
        selected = [x for x in ncu['samples'] if x['roi'] == scope]
        def ms(key, divisor=1):
            values = [x[key]/divisor for x in selected]
            return f(statistics.mean(values)) + ' ± ' + f(statistics.stdev(values))
        def payload_stats(key):
            values = [x[key] for x in selected]
            return payload_pair(statistics.mean(values), statistics.stdev(values))
        ratio = (sum(x['read'] for x in selected) + sum(x['write'] for x in selected)) / sum(x['ns'] for x in selected)
        rows.append([e(scope), payload_stats('read'), payload_stats('write'), ms('ns',1e6), ms('bandwidth'), f(ratio)])
    parts.append('<figure>' + ncu_payload_chart(ncu) + '<figcaption>图 5 · 正式 group 1 / 2 / 3，各 scope 三次 app-range 的 mean ± sample SD。Full/Prefill 和 Decode 分面，读、写单独尺度，避免较小 Decode 写量被淹没。</figcaption></figure>')
    parts.append(table(['Scope · n=3', 'Read · mean ± SD', 'Write · mean ± SD', 'NCU duration ms · mean ± SD', '逐次 GB/s · mean ± SD', 'ratio of means GB/s'], rows))
    parts.append('<p class="muted">ratio of means = (mean R + mean W) / mean duration；一般不等于 mean[(R+W)/duration]。这里的 NCU B/time 是该 app-range 计数口径的比值，不是无干扰 inference throughput，也不是持续物理 bus bandwidth。</p>')
    parts.append('<p class="muted">同一单元格的 mean 与 SD 使用相同单位，两位小数；显示 SD = 0.00 只表示小于显示精度，不代表样本完全相同，精确字节可查逐次 CSV。</p>')
    full_mean = statistics.mean(x['ns'] for x in ncu['samples'] if x['roi']=='full') / 1e6
    event_sum = sum(x['event_ms'] for x in capture['rows'])
    parts.append(f'<p class="note warn">Full NCU duration 均值为 {f(full_mean)} ms；此前独立非 NCU 运行的 33 段 CUDA event 和为 {f(event_sum)} ms，二者计时及窗口口径不同。差异的具体原因尚未通过专门实验分解，不能简单归因于“仅 NCU 开销”，也不能拿 NCU 时间或此 B/time 反向拟合模拟器。</p>')
    variable_scope=max(ncu['scopes'],key=lambda scope:ncu_stats(ncu,scope,'ns')[1]/ncu_stats(ncu,scope,'ns')[0])
    time_mean,time_sd=ncu_stats(ncu,variable_scope,'ns',1e6)
    bw_mean,bw_sd=ncu_stats(ncu,variable_scope,'bandwidth')
    parts.append(f'<p class="note warn">时间与带宽离散明显：{e(variable_scope)} 的 duration 为 {f(time_mean)} ± {f(time_sd)} ms，CV = {f(time_sd/time_mean*100)}%；逐次带宽为 {f(bw_mean)} ± {f(bw_sd)} GB/s。n=3 不足以解释波动来源；这些 app-range 时间不能作为无干扰推理时间的校准目标。Read/Write 计数与时间波动应分开审视。</p>')
    diagnostic_path = REPO/'validation/ncu-timing-diagnostic.json'
    if diagnostic_path.exists():
        diagnostic=load(diagnostic_path)
        assert diagnostic['status']=='COMPLETE_SAME_RUN_CUDA_EVENT_TIMINGS_NOT_CAPTURED'
        assert ncu['hosts_with_cuda_events']==0
        parts.append('<p>18 份正式 NCU host finish 的 natural_cuda_event_ms 均为 null，<strong>没有同 run CUDA event</strong> 可用于拆分这个时间差。profiling 运行中的主机边界区间也比独立 discovery 慢，表明差异不只是 CSV 格式或单位问题；但该 host 区间不是 GPU 时间，不能替代带宽分母。详见 '+link(diagnostic_path,'独立 NCU 计时诊断')+'；尚不能分解 profiling、运行状态和边界的各自贡献。</p>')
    parts.append('<figure>' + ncu_timing_chart(ncu, capture) + '<figcaption>图 6 · NCU 为正式三组 mean ± sample SD；灰色 CUDA event 仅一次，无误差条，Full 灰点为 33 段 event 和。图中计时口径独立，不能据其比值直接认定 profiling overhead 或推理加速。横轴均为对数刻度。</figcaption></figure>')
    parts.append('<details><summary>各组原始样本与 CSV（18 次 scope 采样）</summary>')
    parts.append(table(['组', 'Scope', 'Read', 'Write', 'duration ms', 'R+W / duration GB/s', 'host replays', '来源'], [
        [str(x['group']), e(x['roi']), payload(x['read']), payload(x['write']), f(x['ns']/1e6), f(x['bandwidth']), str(x['host_replays']), link(x['csv'],'原始 CSV')] for x in ncu['samples']]))
    parts.append('</details><p>' + link(ncu['root']/'controller.json','正式 NCU 三组 controller receipt') + ' · SHA256 <code>' + sha(ncu['root']/'controller.json') + '</code></p>')
    return '\n'.join(parts)


def ncu_stats(ncu, scope, key, divisor=1):
    values = [x[key]/divisor for x in ncu['samples'] if x['roi'] == scope]
    return statistics.mean(values), statistics.stdev(values)


def ncu_payload_chart(ncu):
    s = svg_start('NCU 三组 R/W：Full 与 Prefill、Decode 分别展示', '每条是三个同 scope app-range 样本的均值，误差线为样本标准差；四个面板独立线性量程。', height=620)
    panels = [(25, 45, 'Full / Prefill · Read GB', ['full','Prefill'], 'read',1e9),
              (520,45,'Full / Prefill · Write GB',['full','Prefill'],'write',1e9),
              (25,295,'Decode · Read GB',['Decode1','Decode8','Decode16','Decode32'],'read',1e9),
              (520,295,'Decode · Write MB',['Decode1','Decode8','Decode16','Decode32'],'write',1e6)]
    for left,top,title,scopes,key,divisor in panels:
        values = [ncu_stats(ncu,scope,key,divisor) for scope in scopes]
        upper = max(mean+sd for mean,sd in values) * 1.17
        x0,width = left+94,270
        s.append(f'<text x="{left}" y="{top}" font-weight="700">{title}</text>')
        bottom = top+55+len(scopes)*47
        for frac in [0,.5,1]:
            x=x0+frac*width
            s.append(f'<line class="axis" x1="{x}" y1="{top+20}" x2="{x}" y2="{bottom}"/><text class="small" x="{x}" y="{bottom+20}" text-anchor="middle">{f(upper*frac)}</text>')
        for i,(scope,(mean,sd)) in enumerate(zip(scopes,values)):
            y=top+43+i*47;length=mean/upper*width;error=sd/upper*width
            s.append(f'<text class="small" x="{left}" y="{y+5}">{e(scope)}</text><rect x="{x0}" y="{y-8}" width="{length}" height="16" rx="3" fill="#b16b3b"/>')
            x=x0+length
            s.append(f'<line x1="{x-error}" y1="{y}" x2="{x+error}" y2="{y}" stroke="#243449"/><line x1="{x-error}" y1="{y-5}" x2="{x-error}" y2="{y+5}" stroke="#243449"/><line x1="{x+error}" y1="{y-5}" x2="{x+error}" y2="{y+5}" stroke="#243449"/><text class="small" x="{x0}" y="{y+25}">{payload_pair(mean*divisor,sd*divisor)}</text>')
    s.append('<text class="small" x="25" y="603">四面板均从 0 开始；量程不同。数据是独立 scope 的 NCU counter，不能跨 scope 求和替代 Full。</text></svg>')
    return ''.join(s)


def ncu_timing_chart(ncu,capture):
    s=svg_start('NCU app-range 时间、自然 CUDA event 与 NCU B/time', 'NCU 时间与带宽为正式三组均值和样本标准差，CUDA event 来自独立一次运行，没有误差条。',height=445)
    s.append('<text x="25" y="28" font-weight="700">时间 ms · 对数刻度</text><text x="535" y="28" font-weight="700">NCU 同 run R+W / duration · GB/s</text>')
    phases={r['phase']:r['event_ms'] for r in capture['rows']};phases['full']=sum(phases.values())
    for left,low,high,width in [(110,0,4,355),(620,1,3,315)]:
        for power in range(low,high+1):
            x=left+(power-low)/(high-low)*width
            s.append(f'<line class="axis" x1="{x}" y1="50" x2="{x}" y2="358"/><text class="small" x="{x}" y="380" text-anchor="middle">{10**power:g}</text>')
    for i,scope in enumerate(ncu['scopes']):
        y=75+50*i
        s.append(f'<text class="small" x="25" y="{y+5}">{e(scope)}</text><text class="small" x="535" y="{y+5}">{e(scope)}</text>')
        for left,low,high,width,key,divisor in [(110,0,4,355,'ns',1e6),(620,1,3,315,'bandwidth',1)]:
            mean,sd=ncu_stats(ncu,scope,key,divisor)
            def xpos(v):return left+(math.log10(max(v,10**low))-low)/(high-low)*width
            x,xl,xh=xpos(mean),xpos(mean-sd),xpos(mean+sd)
            yy=y-6 if key=='ns' else y
            s.append(f'<line x1="{xl}" y1="{yy}" x2="{xh}" y2="{yy}" stroke="#b16b3b" stroke-width="2"/><line x1="{xl}" y1="{yy-4}" x2="{xl}" y2="{yy+4}" stroke="#b16b3b"/><line x1="{xh}" y1="{yy-4}" x2="{xh}" y2="{yy+4}" stroke="#b16b3b"/><circle cx="{x}" cy="{yy}" r="4" fill="#b16b3b"/><text class="small" x="{min(x+8,left+width-55)}" y="{yy-8}">{f(mean)}</text>')
            if key=='ns':
                ex=xpos(phases[scope]);ey=y+12
                s.append(f'<circle cx="{ex}" cy="{ey}" r="4" fill="#fff" stroke="#8798ad" stroke-width="2"/><text class="small" x="{min(ex+8,left+width-55)}" y="{ey+5}">{f(phases[scope])}</text>')
    s.append('<circle cx="30" cy="408" r="4" fill="#b16b3b"/><text class="small" x="43" y="413">NCU n=3，mean ± sample SD</text><circle cx="340" cy="408" r="4" fill="#fff" stroke="#8798ad" stroke-width="2"/><text class="small" x="353" y="413">CUDA event n=1；不同运行、不同时间口径</text></svg>')
    return ''.join(s)


def theoretical_read_section(capture,ncu):
    manifest=capture['manifest'];parameters=manifest['parameters'];config=manifest['hf_config']
    assert len(parameters)==195 and len({p['root'] for p in parameters})==len(parameters)
    assert all(p['dtype']=='torch.bfloat16' and p['element_size']==2 for p in parameters)
    embeddings=[p for p in parameters if p['label']=='parameter.model.embed_tokens.weight']
    assert len(embeddings)==1
    embedding=embeddings[0]
    assert embedding['shape']==[config['vocab_size'],config['hidden_size']]
    total_weights=sum(p['logical_nbytes'] for p in parameters)
    row_bytes=math.prod(embedding['shape'][1:])*embedding['element_size']
    weight_once=total_weights-embedding['logical_nbytes']+row_bytes
    kv_per_token=2*config['num_hidden_layers']*config['num_key_value_heads']*config['head_dim']*2
    assert kv_per_token==sum(math.prod(b['shape'][1:])*b['element_size'] for b in manifest['kv_pool']['buffers'])
    assert (total_weights,embedding['logical_nbytes'],row_bytes,weight_once,kv_per_token)==(16060522496,1050673152,8192,15009857536,131072)
    parts=['<h3 id="theoretical-read">独立理论读量：一次逻辑覆盖，非 TileGen 结果</h3>',
           '<p>由当前 manifest 的 195 个唯一 BF16 parameter roots 构造：全部参数 '+f(total_weights/1e9)+' GB，减去完整 embedding table '+f(embedding['logical_nbytes']/1e9)+' GB，再加单 token 的一行 8,192 B。非 embedding 权重各覆盖一次 + embedding 一行 = <strong>'+f(weight_once/1e9)+' GB</strong>。LM head、norm 参数包含在非 embedding 权重中。</p>',
           '<p>Decode d 的全上下文 KV 逻辑读量 = 2(K/V) × 32 layers × 8 KV heads × 128 head_dim × 2 B × (1024+d)。这是基于实际参数和 shape 的 one-pass logical footprint，未用 NCU 拟合；忽略 activation / merge / metadata 请求、cache 命中、sector 与重复读取，不是精确 DRAM 预测或其必然下界。</p>']
    rows=[]
    for step in [1,8,16,32]:
        scope=f'Decode{step}';kv=kv_per_token*(1024+step);theory=weight_once+kv
        selected=[x for x in ncu.get('samples',[]) if x['roi']==scope] if ncu['ready'] else []
        if selected:
            ncumean=statistics.mean(x['read'] for x in selected);ncustd=statistics.stdev(x['read'] for x in selected)
            ncucell=payload_pair(ncumean,ncustd)
            deltacell=f((ncumean-theory)/1e6)+' MB / '+f((ncumean-theory)/theory*100,4)+'%'
        else:ncucell='N/A · 正式三组待采';deltacell='N/A · 缺正式对照'
        rows.append([scope,f(weight_once/1e9),f(kv/1e6),f(theory/1e9),ncucell,deltacell,'N/A · 模型未准入'])
    parts.append(table(['阶段','一次权重 GB','一次 KV MB','理论 Read GB','NCU Read · mean ± SD','NCU−理论','TileGen'],rows))
    parts.append('<p class="note">即使理论与 NCU 读量接近，也只表明这一工作负载的读量与一次覆盖账相近；它不验证地址、mask、访问顺序或 native trace，更不验证 write、latency 和 HBF 性能。每 token 新增 KV payload 为 131,072 B，仅表示 KV 逻辑持久新增，绝不是整个 DRAM 写量；缓存还可能推迟真正写回。</p>')
    parts.append('<details><summary>可复算的精确字节分解</summary><pre>'+e(json.dumps({'all_parameter_bytes':total_weights,'embedding_table_bytes':embedding['logical_nbytes'],'one_embedding_row_bytes':row_bytes,'non_embedding_once_plus_row_bytes':weight_once,'KV_bytes_per_context_token_all_layers':kv_per_token,'decode_read_theory_bytes':{f'Decode{d}':weight_once+kv_per_token*(1024+d) for d in [1,8,16,32]}},indent=2))+'</pre></details>')
    return '\n'.join(parts)


def read_observer(root,capture):
    controller_path=root/'controller.json'
    if not controller_path.exists():return {'ready':False,'root':root}
    controller=load(controller_path)
    if controller.get('status')!='PASS_NATIVE_METADATA_CENSUS_ONLY':return {'ready':False,'root':root}
    census=load(root/'native-census.json')
    assert census['status']=='PASS_NATIVE_METADATA_CENSUS_ONLY'
    assert sha(root/'native-census.json')==controller['native_census_sha256']
    assert census['input_contract_sha256']==capture['controller']['input_contract']['sha256']
    assert controller['workload_package']==capture['controller']['package']
    assert controller['workload_manifest_sha256']==capture['controller']['package']['manifest_sha256']
    assert sha(root/'build/build.json')==controller['build']['receipt_sha256']
    binary=controller['build']['binary']
    assert (root/'build/observer.so').stat().st_size==binary['bytes'] and sha(root/'build/observer.so')==binary['sha256']
    pid, ticks=census['process']['pid'],census['process']['start_ticks']
    process_root=root/f'observer/process-{pid}-{ticks}'
    finish=load(process_root/'finish.json')
    assert sha(process_root/'finish.json')==census['observer_finish_sha256']
    assert finish['status']=='PASS_METADATA_OBSERVER_CLOSED_NOT_TRACE'
    assert finish['launch_before_count']==finish['launch_return_count']==census['total_launches']
    assert finish['launch_error_count']==finish['unsupported_dispatch_count']==finish['unknown_launch_attribute_count']==0
    assert finish['epoch_begin_count']==finish['epoch_end_count']==33 and finish['active_epoch']==0
    assert finish['function_count']==census['inspected_functions']
    for row in census['journals']:
        path=process_root/row['name']
        assert path.stat().st_size==row['bytes'] and sha(path)==row['sha256'],row['name']
    assert len(census['journals'])==6
    expected={'Prefill':408,**{f'Decode{i}':397 for i in range(1,33)}}
    assert dict(Counter(x['phase'] for x in census['calls']))==census['phase_counts']==controller['phase_counts']==expected
    assert len(census['calls'])==census['measured_launches']==controller['measured_launches']==sum(expected.values())
    assert len({x['source_launch_key'] for x in census['calls']})==census['measured_launches']
    qualification=census['qualification']
    assert qualification['decoded_static_SASS_hashes_verified'] and qualification['argument_size_layout_only']
    assert not any(qualification[k] for k in ['raw_argument_values_captured','typed_pointer_binding','dynamic_memory_addresses','dynamic_program_execution','native_model_admitted'])
    manifest=load(root/'native/artifacts/manifest.json')
    assert manifest['input_contract']==capture['controller']['input_contract']
    assert manifest['native_source_files']==capture['manifest']['native_source_files'] and manifest['native_source_unchanged']
    assert manifest['coverage']['native_scope_abi_enabled'] is True
    process=load(root/'native/process.json')
    assert process['returncode']==0 and process['owned_processes_drained'] and process['gpu_quiescent']
    audit_path=REPO/'validation/native-p1024d32-census-audit.json'
    audit=load(audit_path)
    assert audit['schema']=='INDEPENDENT_P1024D32_NATIVE_CENSUS_AUDIT_V1'
    assert audit['status']=='PASS_METADATA_CENSUS_AUDIT_ONLY' and not audit['qualification']['native_model_admitted']
    assert audit['source']['controller_sha256']==sha(controller_path)
    assert audit['source']['census_sha256']==controller['native_census_sha256']
    assert audit['source']['finish_sha256']==census['observer_finish_sha256']
    assert audit['source']['journals']==census['journals']
    assert audit['source']['manifest_sha256']==sha(root/'native/artifacts/manifest.json')
    assert audit['measured_launches']==census['measured_launches'] and audit['phase_counts']==expected
    assert audit['measured_kernel_code_hashes']==len({x['code_sha256'] for x in census['calls']})
    assert audit['measured_symbols']==len({x['function_name'] for x in census['calls']})
    assert sum(audit['comparison']['status_totals'].values())==census['measured_launches']
    assert audit['comparison']['candidate_is_not_argument_value_or_dynamic_address_equivalence']
    return {'ready':True,'root':root,'controller':controller,'census':census,'finish':finish,'process_root':process_root,'audit':audit}


def observer_section(observer):
    if not observer['ready']:
        return '<p>静态 observer 尚待闭合；不能把 partial code 当作完整静态 census 或模型准入。</p>'
    census,root=observer['census'],observer['root']
    parts=['<h3 id="native-static-census">Observer r2：静态 census 已闭合，动态模型尚未建立</h3>',
           '<p class="note"><strong>PASS_NATIVE_METADATA_CENSUS_ONLY。</strong>Prefill 408 个 measured launches；每个 Decode 397 个，32 次共 12,704 个；合计 13,112 个。33 个测量 epoch 的 launch before / return 和静态元数据已闭合。此运行未使用 torch.profiler 或 NCU，也未做动态指令 instrumentation。</p>']
    parts.append(table(['范围','计数 / 证据','含义'],[
        ['measured workload','408 + 32 × 397 = 13,112 launches','有 phase、grid/block、resource、decoded SASS hash、argument size/layout identity；不是动态 PC/memory 地址 trace。'],
        ['整个 process',f'{census["total_launches"]:,} launch before / return；{observer["finish"]["unbound_launch_count"]:,} 个在测量 epoch 外','包括 warmup / setup 等，不与 measured launches 混用。'],
        ['静态检查总范围',f'{census["inspected_functions"]:,} inspected functions；{census["unique_decoded_code_hashes"]:,} unique decoded code hashes','属于整个检查范围，不能当作 measured kernel 家族数；hash 是 decoded SASS rows，不是 cubin 文件 hash。'],
        ['ABI 已采部分','argument sizes 与 parameter-layout hash','未采 raw argument values，尚无 typed pointer bindings；ABI layout census 不等于完整参数绑定。'],
        ['仍缺','dynamic PC/masks/memory/shared/barrier witness，私有对象完整身份、新 templates/bindings/phase支持','完整 callback coverage 和同进程 CUPTI crosscheck 尚未证明，native_model_admitted=false。'],
    ],'text'))
    parts.append('<p>r1 因 metadata 配额退出的记录保留；r2 独立运行已成功，六 journals、census、observer binary / finish SHA 与 33 phase 数量通过校验。13,112 恰与先前 profiler 计数一致，不等于同进程独立交叉验证了所有 callback。</p>')
    parts.append('<p>'+link(root/'controller.json','observer r2 controller')+' · '+link(root/'native-census.json','完整 native census')+' · '+link(observer['process_root']/'finish.json','observer finish')+' · '+link(root/'build/build.json','build 身份')+'</p>')
    audit_path=REPO/'validation/native-p1024d32-census-audit.json'
    audit=observer['audit'];comparison=audit['comparison'];totals=comparison['status_totals']
    parts.append(f'<p>独立审计在 measured 范围确认 <strong>{audit["measured_kernel_code_hashes"]} 个 decoded code hashes / {audit["measured_symbols"]} 个符号</strong>；其中 {comparison["shared_measured_code_hashes"]} 个 code hash 在旧 measured 工作流出现，{comparison["new_measured_code_hashes"]} 个是新 code hash。下表只比较静态身份与 launch 配置，不表示原模板已经可用。</p>')
    parts.append(table(['与旧工作流比较','新 measured launches','资格边界'],[
        ['code + ABI sizes + launch 配置完全匹配候选',f(totals['exact_code_ABI_launch_configuration_candidate'],0),'10,679 个静态复用候选；未比较 raw values、tensor shape 内容、动态控制或地址，不能直接准入。'],
        ['相同 code + ABI sizes，launch 配置改变',f(totals['same_code_ABI_changed_launch_configuration'],0),'grid/block/resource/API 等静态配置至少一项改变，需对应新 shape / control witness。'],
        ['新 decoded code',f(totals['new_code'],0),'Prefill 128 个新 GEMM launches；Decode 合计 1,024 个 MergeStates launches；需完整新输入与验证。'],
    ],'text'))
    parts.append('<p>“launch 配置”包含 grid、block、static/dynamic shared、registers、local bytes、launch attributes、CUDA API；即使这些都相同，context 长度或指针所指对象改变仍可能改变动态访问。四个新 code 包括一个 CUTLASS GEMM、两种 Ampere GEMM 和一个 MergeStates；名称分类只作说明，不替代 code hash。'+link(audit_path,'独立六 journal / epoch / 旧工作流对比审计')+'</p>')
    return '\n'.join(parts)


def discovery_section(capture, ncu, observer):
    root, c, m, finish = capture['root'], capture['controller'], capture['manifest'], capture['finish']
    contract = c['input_contract']
    artifact = root / 'metadata/artifacts'
    ncu_status = 'NCU 正式三组 PASS，结果见本节末表。' if ncu['ready'] else 'NCU pilot / 正式采集进行中，尚无完整三组结果。'
    parts = ['<h2 id="capture-status">7. 新目标：原生 SGLang B1 / P1024 / D32</h2>',
             '<div class="note" id="capture-status-placeholder"><strong>Discovery 已完成：</strong>PASS_CAPTURE_CLOSURE_NOT_MODEL_QUALIFICATION。33 阶段 input / position / sequence / KV slot 控制值闭合，原生代码身份一致。' + ncu_status + (' 静态 launch / decoded SASS / ABI layout census 已闭合；' if observer['ready'] else ' 静态 census 待闭合；') + 'P1024D32 native TileGen 模型尚未准入，raw arguments / 动态 witness / bindings 仍待完成。</div>',
             '<p>这是一个独立于上方三 kernel 回放片段的新工作负载。模型是 Meta-Llama-3-8B-Instruct，32 层 BF16、FlashInfer、native eager，B1 / P1024 / D32，KV pool 1,280 token、page size 1。固定 prompt ID 为 1000–2023；Decode 输入交替 944 / 291，保留 native sampling 但输出不反馈。CUDA graph、torch compile、radix cache 和 overlap schedule 均关闭；warmup 一次。数值正确性仍为 NOT_ASSESSED。</p>',
             '<p><strong>本节时间来自一次非 NCU validate 运行的 CUDA event。</strong>阶段内包含 host launch gap，不能当作 kernel busy sum 或 NCU gpu__time_duration.sum。另一次带 module hook / torch.profiler 的 metadata 运行只提供结构诊断，其 instrumented_elapsed_seconds 和 profiler 时间不作为性能基线；两次运行的冻结输入和 source SHA 相同。</p>',
             '<figure>' + discovery_chart(capture) + '<figcaption>图 4 · 33 阶段 CUDA event 实测，n=1，无重复测量置信区间。这里没有计算访存带宽。</figcaption></figure>']
    na_ncu = '<span class="muted">N/A · NCU 待采</span>'
    na_model = '<span class="muted">N/A · 模型未准入</span>'
    na_error = '<span class="muted">N/A · 缺同源双方</span>'
    rows = []
    for row in capture['rows']:
        ncu_value = na_ncu
        if ncu['ready']:
            selected = [x for x in ncu['samples'] if x['roi'] == row['phase']]
            ncu_value = (payload(statistics.mean(x['read'] for x in selected)) + ' / ' + payload(statistics.mean(x['write'] for x in selected)) + ' · n=3') if selected else '<span class="muted">N/A · 未采此 scope</span>'
        rows.append([link(row['metadata_path'], row['phase']), str(row['input_count']), str(row['context']), f(row['event_ms']), ncu_value, na_model, na_error])
    parts.append(table(['阶段', '输入 tokens', 'context tokens', 'CUDA event ms', 'NCU mean R / W', 'TileGen R / W', 'R / W 误差'], rows))
    parts.append('<p class="muted">33 段 event 时间相加为 ' + f(sum(row['event_ms'] for row in capture['rows'])) + ' ms，仅为分段 event 和，不是额外测得的 full-range NCU 时间。尚无数据的列保持 N/A；不填入旧 P32D2 或三个 MLP 片段的数据。</p>')
    parts.append('<h3>Metadata 摘要及完整性范围</h3>')
    kv_bytes = sum(x['logical_nbytes'] for x in m['kv_pool']['buffers'])
    param_bytes = sum(x['logical_nbytes'] for x in m['parameters'])
    parts.append(table(['项目', '已记录内容', '边界'], [
        ['运行环境', e(m['gpu']['name']) + '，SM ' + '.'.join(map(str,m['gpu']['capability'])) + '；' + e(m['python'].split(' (')[0]) + '；CUDA ' + e(m['torch_cuda']), 'GPU UUID ' + e(c['GPU_UUID'])],
        ['软件', '; '.join(e(k) + ' ' + e(v) for k,v in m['packages'].items()), '16 项原生 SGLang Python 文件 SHA 在 validate / metadata / 冻结 reference 之间一致；不是 SASS 身份验证。'],
        ['模型结构', 'hidden 4096；FFN 14336；32 Q heads / 8 KV heads；head_dim 128；vocab 128256；32 layers BF16', '4 个权重 shard 内容 SHA 已由 controller 记录；本地报告未重新读取远端权重文件。'],
        ['参数 / buffers', f'{len(m["parameters"]):,} 个参数 descriptor，{f(param_bytes/1e9)} GB；{len(m["module_buffers"]):,} 个 module-buffer descriptor', '描述对象大小，不代表被读取的流量；module buffer 可共享同一 storage root，不能直接求和为物理分配。'],
        ['KV 对象', f'{len(m["kv_pool"]["buffers"]):,} 个 K/V buffers，共 {f(kv_bytes/1e6)} MB；每个 [1281, 8, 128] BF16；request page table [1, 8196] int32', 'pool capacity 1280 加 sentinel slot；实际 append slots 从 1 连续到 1056。'],
        ['module / root', f'{capture["module_count"]:,} 个 module-call records；{capture["root_count"]:,} 个 storage-root records；33 阶段 JSON', 'Python 弱引用 lifetime 观测，backend-private allocations 不完整。'],
        ['profiler census', f'{capture["event_count"]:,} 个 torch.profiler metadata events，其中 kernel_count={capture["kernel_count"]:,}', '包含观测辅助；per-kernel phase join 未闭合、完整 CUDA API census 未建立，不与 native1138 直接同口径比较。'],
        ['discovery 本身的边界', '这次 torch.profiler metadata 未采 native ABI / decoded SASS，instruction-memory trace=false；tilegraph_complete=false', '后续 observer r2 已补静态 census（见下文）；动态 witness 与完整绑定尚未完成，不能驱动合格的 P1024D32 native TileGen。'],
    ], 'text'))
    parts.append('<p>结构 discovery 已完成，不代表原生模型精度准入。后续静态 observer 的已采字段和缺口在下文单列；没有把 torch.profiler 的 kernel 名称或时间自动转换成 compute / memory 程序，也没有从对象总大小推测 NCU 字节。</p>')
    parts.append('<p>证据：' + link(root/'controller.json','controller 闭合 receipt')+' · '+link(capture['finish_path'],'自然 CUDA event / control receipt')+' · '+link(artifact/'manifest.json','完整 metadata manifest')+' · '+link(artifact/'files.sha256.json','37 个元数据文件 SHA')+' · '+link(artifact/'kernel_launches.json','torch.profiler metadata')+' · '+link(artifact/'module_calls.json','module calls')+' · '+link(artifact/'tensor_roots.json','tensor roots')+'</p>')
    identities = {'input_contract_sha256': contract['sha256'], 'controller_sha256': sha(root/'controller.json'),
                  'natural_receipt_sha256': sha(capture['finish_path']), 'metadata_manifest_sha256': c['metadata_manifest_sha256'],
                  'capture_package_manifest_sha256': c['package']['manifest_sha256'], 'validate_driver_sha256': finish['driver_sha256'],
                  'metadata_driver_sha256': m['driver_sha256'], 'capture_start_UTC': c['start'], 'capture_finish_UTC': c['finish'],
                  'source_files': [{k:r[k] for k in ['relative','sha256']} for r in finish['source_identity']],
                  'model_files_receipted': [{k:r[k] for k in ['path','bytes','sha256']} for r in c['model_identity']]}
    parts.append('<details><summary>冻结身份：契约、采集源码及模型文件 SHA</summary><pre>'+e(json.dumps(identities,ensure_ascii=False,indent=2))+'</pre></details>')
    parts.append(ncu_section(ncu, capture))
    parts.append(theoretical_read_section(capture,ncu))
    parts.append(observer_section(observer))
    parts.append('<h3>新 native 模型仍未准入</h3><p>新 Decode attention grid 为 [9,8,1]，旧源为 [1,8,1]；新 CUTLASS / Ampere GEMM 与 PersistentVariableLengthMergeStates 首先由 profiler symbol 文本比较发现。后续静态 census 提供 decoded code / ABI layout 身份，具体与旧工作流差异见独立审计；静态身份也不能证明动态访问可套入旧绑定。完整 8B P1024D32 simulation 仍需要新 shape 的 raw arguments、完整 native witness、独立 heldout、bindings 及 phase 支持。</p><p>容量也尚待证明：trace 单文件硬上限 64 GiB；HBF 多离散 range 的 sparse seed 上限为 1,048,576 页（4 GiB payload），新工作负载实际 trace 规模尚未测得。不能只调大命令参数就宣称整模可运行。</p><p>'+link(REPO/'docs/native-p1024d32-admission.md','原生 P1024D32 准入审计')+' · '+link(REPO/'validation/native-p1024d32-admission.json','53 项证据 SHA 与具体缺口')+'。这份早期审计的 metadata weight_content_hashes_complete=false 对应内层 manifest；外层 discovery controller 另行记录了四个权重 shard 的内容 SHA。静态 census 后续补齐的身份以上方独立审计为准，动态访存 / 计算模型资格仍未建立。</p>')
    return '\n'.join(parts)


def render(root, output, discovery_root, ncu_root, observer_root):
    runs = verify(root)
    capture = verify_discovery(discovery_root)
    ncu = read_ncu(ncu_root, capture)
    observer=read_observer(observer_root,capture)
    admission = load(REPO/'validation/native-p1024d32-admission.json')
    assert admission['new_workload_admitted'] is False, 'update report qualification before claiming model admission'
    g, hbf = runs['gddr6'][0], runs['hbf'][0]
    physical = hbf['physical']
    source = load(root / 'gddr6/source-result.json')
    source_receipt = load(root / 'gddr6/source-run-receipt.json')
    common = g['read_bytes'] + g['write_bytes']
    parts = ['<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">',
             '<title>同一 TileGen trace 的 GDDR6 / HBM / HBF stage replay</title>',
             '''<style>:root{color-scheme:light}*{box-sizing:border-box}body{margin:0;background:#f3f6fa;color:#243449;font:16px/1.75 -apple-system,BlinkMacSystemFont,"PingFang SC",Arial,sans-serif}main{max-width:1160px;margin:auto;padding:46px 28px 80px}h1{font-size:34px;line-height:1.35;margin:12px 0}h2{font-size:23px;margin:42px 0 14px}h3{font-size:18px}p{margin:12px 0}a{color:#116c8d}code{font:13px/1.5 ui-monospace,monospace;overflow-wrap:anywhere}.eyebrow{letter-spacing:.12em;font-size:12px;color:#627389}.lead{font-size:19px}.note{padding:18px 22px;background:#eaf3f4;border-left:4px solid #147b83;border-radius:7px}.warn{background:#fbf0e5;border-color:#b16b3b}.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:14px}.card{background:white;padding:20px;border-radius:12px;border-top:4px solid var(--accent)}.metric{font-size:30px;font-weight:700}.muted,figcaption{color:#596b80;font-size:14px}figure{margin:24px 0;padding:14px;background:#fff;border:1px solid #e0e8ef;border-radius:14px}svg{display:block;width:100%;height:auto}figcaption{padding:8px 12px}.table-scroll{overflow-x:auto;border:1px solid #dce4ec;background:white;border-radius:10px}table{border-collapse:collapse;width:100%;font-size:14px}th,td{padding:10px 12px;border-bottom:1px solid #e4ebf2;text-align:right;vertical-align:top}th{background:#edf2f7;white-space:nowrap}td:first-child,th:first-child{text-align:left}tr:last-child td{border-bottom:0}.text td{text-align:left}.path{font-size:12px}details{background:white;border:1px solid #dce4ec;padding:16px;border-radius:10px;margin:14px 0}summary{cursor:pointer;font-weight:600}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#f3f6fa;padding:12px;font-size:12px}.chips span{display:inline-block;background:#e5edf5;padding:3px 9px;margin:3px;border-radius:5px;font-size:12px}.foot{margin-top:40px;padding-top:16px;border-top:1px solid #d8e0e9}@media(max-width:700px){main{padding:25px 15px}.cards{grid-template-columns:1fr}h1{font-size:28px}}</style><main>''',
             '<div class="eyebrow">TILEGEN · FROZEN DIRECT TRACE · NATIVE MEMORY BACKENDS · 更新 2026-09-19</div>',
             '<h1>同一条地址流，接入 GDDR6、HBM 和真实 HBF 控制器</h1>',
             '<p class="lead">三个后端均完成闭环回放；差异来自原生内存服务与控制器行为。回放结果覆盖 B1 / Decode2 的三个 kernel 片段，不能外推为完整 LLM 推理性能。新增 <a href="#capture-status">P1024D32 discovery</a> 已完成，33 阶段自然 CUDA event 与 metadata 单独展示。</p>',
             '<div class="chips"><span>固定 post-cache trace</span><span>Read 128B / Write 32B</span><span>W2 stage window</span><span>未校准 compute 近似</span><span>硬件 timing 未验证</span></div>',
             '<p class="note warn"><strong>HBF 结果的关键限制：</strong>大多数 128B 读在当前前台数据路径分别触发 4KB NAND 页读，物理读流量约为逻辑读的 <strong>' + f(physical['read_payload_amplification']) + ' 倍</strong>。因此这里的 ' + f(hbf['aggregate_bandwidth_GBps']) + ' GB/s 是这一地址顺序、窗口和控制器策略下的逻辑吞吐，不能通过替换峰值带宽直接预测 LLM。</p>',
             table(['工作阶段','已完成 / 当前状态','本页数据范围'],[
                 ['1 · 多后端回放','完成：GDDR6 numeric overlay / HBM / HBF 账本闭合','旧冻结 Decode2 三个 MLP kernel 的 512/1/512 CTA 片段；非新 P1024D32 模拟。'],
                 ['2 · 真实 SGLang / NCU','Discovery 已完成；'+('NCU 正式三组完成。' if ncu['ready'] else 'NCU 正式三组进行中。'),'新 B1/P1024/D32、32 层 BF16；独立真实采集。'],
                 ['3 · 新 native TileGen','未准入；NCU 完成也不会自动改变这一状态。',('静态 ABI layout / decoded SASS census 已闭合；' if observer['ready'] else '静态 census 待闭合；')+'raw args、动态 witness、shape 模板、bindings、phase 与容量适配待完成；TileGen 对照保持 N/A。'],
             ],'text'),
             '<div class="cards">']
    for name in NAMES:
        r = runs[name][0]
        parts.append(f'<div class="card" style="--accent:{COLORS[name]}"><strong>{LABELS[name]}</strong><div class="metric">{f(r["aggregate_bandwidth_GBps"])} <small>GB/s</small></div><div>总时间 {f(r["makespan_ns"]/1000)} μs</div><div class="muted">逻辑 R+W / 总时间（含持久化尾部）</div></div>')
    parts += ['</div>', '<figure>' + log_comparison(runs) + '<figcaption>图 1 · 时间和逻辑带宽使用同一完整分母；点图采用对数刻度以保留数量级差异。</figcaption></figure>',
              '<h2>1. 先固定比较对象与时间口径</h2>',
              f'<p>共同输入为 <strong>{g["requests"]:,}</strong> 条请求：{g["read_requests"]:,} × 128B 读和 {g["write_requests"]:,} × 32B 写，合计 <strong>{f(common/1e6)} MB</strong>。初始 L1/L2 为 cold；三次调用连续经过相同 source cache，结尾不额外刷新仍驻留的 dirty line。后端接收已经过 cache 的服务请求，不再次过 GPU cache。</p>',
              '<p>阶段以最多 48 CTA 切分，共 23 个 stage，最多两个未完成 compute 的 stage 同时开放。每个 stage 的内存完成后才进入单一 compute lane；跨 kernel 完整 barrier，后端 row / bank / controller 状态连续保留。compute 由原生模板的流水线资源估计，省略完整 GPU DAG、寄存器依赖调度和 GPU stall；它是显式近似，不等同默认精确 cosimulation。</p>',
              '<p>数据范围为层 1 MLP 的 GEMV → SiLU → GEMV：分别执行 512 / 1 / 512 CTA；前后 GEMV 原始 grid 分别是 7,168 / 1,024 CTA。因此这是 <strong>三个 kernel 的选定片段</strong>，不是完整 Decode2、完整 8B 模型，也不是 P1024D32。</p>']
    parts.append(table(['后端', 'Read MB', 'Write MB', 'stage μs', 'EOF 尾部 μs', '总时间 μs', '逻辑 GB/s', 'native span μs'], [
        [LABELS[n], f(r['read_bytes']/1e6), f(r['write_bytes']/1e6), f(r['stage_makespan_ns']/1000), f(r['persistence_tail_ns']/1000), f(r['makespan_ns']/1000), f(r['aggregate_bandwidth_GBps']), f(r['memory_active_span_ns']/1000)] for n,(r,_,_) in runs.items()]))
    parts += ['<p class="muted">所有 GB/s 均为十进制 bytes / ns。总时间从 cycle 0 到 source 完成、最后 compute 以及 EOF 维护结束；native span 从 native first arrival 到 native finish，包含间隙，不是“总线忙碌时间”。HBF 的 EOF 尾部未摊入 kernel 或语义阶段。</p>',
              '<h2>2. 逻辑请求与物理 payload 分开记账</h2>', '<figure>' + payload_chart(runs) + '<figcaption>图 2 · 灰色输入保持不变；HBF 彩色读条包括原生页读、RMW 和必要的映射/维护读。MB 两位小数，精确字节见原始 JSON。</figcaption></figure>']
    parts.append(table(['后端', '物理 Read MB', '物理 Write MB', 'Read 放大', 'Write 放大', '物理 payload / 总时间 GB/s'], [
        [LABELS[n], f(r['physical_payload_read_bytes']/1e6), f(r['physical_payload_write_bytes']/1e6), f(r['physical_payload_read_bytes']/r['read_bytes'])+'×', f(r['physical_payload_write_bytes']/r['write_bytes'])+'×', f((r['physical_payload_read_bytes']+r['physical_payload_write_bytes'])/r['makespan_ns'])] for n,(r,_,_) in runs.items()]))
    parts += [f'<p>HBF 共发生 {physical["page_reads"]:,} 次页读。源逻辑读为 {g["read_bytes"]:,} B，NAND 读为 {physical["physical_read_bytes"]:,} B。原生已完成页缓存命中 {physical["read_buffer_hits"]:,} 次、未命中 {physical["read_buffer_misses"]:,} 次。</p>',
              '<p><strong>已有缓存，但未做同页飞行中数据读合并。</strong>当前 HbfController 前台读先检查 decoded-page cache，未命中即独立 schedule_read_page。HbfDevice 每个 page-buffer bank 保留最多两个已完成页，尚未完成的 fill 不会被当作 ready hit；映射页路径的 coalesced misses 属于另一层，不能解释为用户数据读合并。未来若加入同页合并或改变页缓存策略，需要新的实验，不能把当前结果静默按 32 除。</p>',
              f'<p>源写 {g["write_requests"]:,} 条、{g["write_bytes"]:,} B 进入 write buffer 后，形成 {physical["data_programs"]:,} 个 data page program（{physical["data_program_payload_bytes"]:,} B）和 {physical["mapping_program_payload_bytes"]//4096:,} 个 mapping page program（{physical["mapping_program_payload_bytes"]:,} B）；本次 GC relocation 为 {physical["gc_relocation_payload_bytes"]:,} B，erase {physical["block_erases"]:,} 次。32B 是 source 写回粒度，4KB 是 NAND payload 页粒度，两者不矛盾。write_buffer_merged_bytes 记录重叠字节合并，不能用其为 0 推断没有页内写合并。</p>',
              f'<p>控制器附带的 HBM 仅服务 FTL / buffer：Read {f(physical["host_hbm_read_bytes"]/1e6)} MB、Write {f(physical["host_hbm_write_bytes"]/1e6)} MB；不等同源 trace 被放到 HBM，也不应加进逻辑流量分子。</p>',
              '<h2>3. 按阶段和 kernel 看流量与带宽</h2>',
              '<p>当前 result 的 semantic_phases 仅给出 <strong>Decode2 阶段标签</strong>。不存在经过核验的 weights / activation / KV 的 post-cache R/W 分解，因此下图颜色只表示后端。GEMV 含权重与激活请求，不能把其全部流量标为 weights；本片段未选 attention / KV kernel，不能据此声称整模 KV 流量为零。</p>',
              '<figure>' + kernel_chart(runs) + '<figcaption>图 3 · 每调用 GB/s = 该调用 source R+W / 其 stage 调用区间。HBF 最终持久化尾部另列。</figcaption></figure>']
    rows = []
    for i in range(3):
        for n in NAMES:
            r, _, _ = runs[n]
            c = r['calls'][i]
            evidence = r['phase_profile']['calls'][i]['source_evidence']
            rows.append([f'{i} · {e(c["family"])} · {LABELS[n]}', str(evidence['selected_CTAs']), f(c['read_bytes']/1e6), f(c['write_bytes']/1e6), f(c['makespan_ns']/1000), f(c['aggregate_bandwidth_GBps']), f(c['compute_ns']/1000)])
    parts.append(table(['调用 / 后端', 'CTA', 'Read MB', 'Write MB', '调用时间 μs', '逻辑 GB/s', 'compute μs'], rows))
    parts.append(table(['阶段（选定范围）', 'Read MB', 'Write MB', 'stage μs', '阶段 GB/s'], [
        [f'Decode2 · {LABELS[n]}', f(r['semantic_phases'][0]['read_bytes']/1e6), f(r['semantic_phases'][0]['write_bytes']/1e6), f(r['semantic_phases'][0]['makespan_ns']/1000), f(r['semantic_phases'][0]['aggregate_bandwidth_GBps'])] for n,(r,_,_) in runs.items()]))
    parts += ['<p class="muted">写流量按触发 eviction 的调用 / CTA 归属，不是最后写入该数据的 tensor 或 store 归属。跨调用 barrier 使调用时间可加为 stage 总时间；相邻 stage residence 会重叠，stage residence 不能直接求和。</p>',
              '<h2>4. compute overlap、持久化与 CPU 成本</h2>']
    parts.append(table(['后端', 'compute busy μs', 'native outstanding μs', '交集代理 μs', 'EOF 持久化 μs', 'CPU 分钟', 'wall 分钟'], [
        [LABELS[n], f(r['compute_busy_ns']/1000), f(r['native_outstanding_ns']/1000), f(r['compute_native_outstanding_overlap_ns']/1000), f(r['persistence_tail_ns']/1000), f(rc['CPU_minutes']), f(rc['elapsed_minutes'])] for n,(r,rc,_) in runs.items()]))
    parts += ['<p>“交集代理”是 compute 活跃且至少一条 native source 请求尚未交付完成的时间并集；它不测量实际物理总线与 GPU 计算的同时忙碌。三个后端使用相同 compute estimate，但内存服务不同。HBF write completion 可先于 NAND 持久化，EOF drain 继续完成 dirty write buffer / mapping 维护并确认 quiescent。</p>',
              f'<p>CPU 为完整回放子进程 user + system，包含读 trace、校验、HBF 预扫描 / seed 构造、stage 调度和 drain；不含编译、direct trace 生成与 Python wrapper 的元数据准备。共同 direct trace 生成单独花费 CPU <strong>{f(source_receipt["CPU_minutes"])} 分钟</strong>、wall {f(source_receipt["elapsed_minutes"])} 分钟，生成后由三个后端复用。以上为单次测量，没有置信区间；不使用 result.host_seconds 代替完整 CPU 成本。</p>',
              '<h2>5. 配置、初态与适用边界</h2>']
    parts.append(table(['目标', '原生实现 / 配置', '约束'], [
        ['GDDR6', 'HbmDevice generic 命令核心 + GDDR6 数值 overlay；10 channels × 16 bit × 18 Gb/s，32B burst；配置 DQ envelope 360 GB/s。', '保留 row / bank / queue 等待；真正 GDDR6 JEDEC controller 未实现。refresh=false、replication=false；未做硬件 timing 校准。'],
        ['HBM', 'HbmDevice；1 stack，48 GiB，32 channels × 64 bit × 8 Gb/s，2 PC / channel，32B burst；配置 DQ envelope 2,048 GB/s。', '同一 GPU source cache 结果被固定复用，不模拟真实 GPU 改接 HBM 后可能发生的 cache / 调度变化；refresh=false、replication=false。'],
        ['HBF', 'HbfController + HbfDevice；1 stack、16 channels、每 channel 1 die、每 die 16 planes；4KB page，read 4μs / program 75μs / erase 2ms，ECC 0.5μs。配置 HBIO envelope 1,536 GB/s。', 'HBIO envelope 不是 sustained NAND bandwidth。full-resident mapping；write buffer 1,024 pages，flush threshold 512；auto GC=true，thermal=false。'],
    ], 'text'))
    parts += [f'<p>HBF 初态预置本 trace 触及的 {hbf["seed_pages"]:,} 个逻辑页，共 {hbf["seed_ranges"]} 段，mutable mapping；未计入 preload 时间。source 的 packed service byte address 原样解释为 HBF logical address。初始内容与分配生命周期未恢复，这不是数据值正确性验证。HBF 共 {physical["initial_mapping_pages"]:,} 个初始 mapping 页，逻辑容量 {f(physical["logical_capacity_bytes"]/1e9)} GB。</p>',
              '<p>GDDR6 / HBM 使用最多 4,096 个 source parent，以及每 channel 32 个 native burst credits；HBF 使用最多 512 个 logical source parent，不能将这两个 credit 单位当作相同并发能力。EOF 尾部只算真实设备维护，不创建伪 trace parent。</p>',
              '<p>r3 CLI 为目标身份加入接口几何准入：HBM 为 x64 / 2 PC / BL8，GDDR overlay 为 x16 / 1 PC / BL16；HBF 配置必须显式声明 hbf-standard，防止同一个 generic core 被错误标为另一后端。本页采用 r3 的完整实耗 receipt。</p>',
              '<p class="note">此次验证证明：三后端使用相同源 trace 和阶段输入，request / byte / completion 账本闭合，HBF 完成持久化 drain。它没有证明实际 HBF 产品、真实 GPU-HBF 架构或端到端 LLM 的性能精度。native_target_timing_qualified=false；hardware_timing_calibrated=false。历史 source receipt 未冻结原始 cfg 文件字节，本次回放另行快照和哈希当前 cfg；不能把该快照说成历史原件的密码学证明。</p>',
              '<h2>6. 原始证据与可复现入口</h2>']
    rows = []
    for n,(r,rc,base) in runs.items():
        cfg = base / ('native-memory.cfg' if n == 'gddr6' else 'target-memory.cfg')
        rows.append([LABELS[n], link(base/'result.json','result.json') + ' · ' + link(base/'run-receipt.json','CPU receipt'), link(cfg,cfg.name), '<code>'+e(r['target_backend']['config_sha256'])+'</code>', '<code>'+e(rc['result_sha256'])+'</code>'])
    parts.append(table(['后端', '结果 / 实耗', '配置', '配置 SHA256', '结果 SHA256'], rows, 'text'))
    pins = {'source_trace_sha256': g['trace']['file_sha256'], 'context_sha256': g['trace']['context_sha256'], 'phase_profile_sha256': g['phase_profile_sha256'], 'request_shape_fnv1a64': g['request_payload_fnv1a64'], 'replay_binary_sha256': runs['gddr6'][1]['binary_sha256'], 'upstream_commit': g['memory_model']['upstream_commit'], 'hbf_initial_ranges_sha256': hbf['hbf_initial_image']['range_sha256']}
    parts += ['<details><summary>共享身份与精确账本</summary><pre>'+e(json.dumps(pins,ensure_ascii=False,indent=2))+'</pre>',
              f'<p>Read {g["read_bytes"]:,} B；Write {g["write_bytes"]:,} B；records {g["requests"]:,}；trace file {g["trace"]["file_bytes"]:,} B。所有三组 result hash、input hash、config hash、source receipt hash 与 ledger 已在渲染前检查。</p></details>']
    upstream = REPO / 'source/work/hbfsim-latest/upstream/src'
    parts += ['<p>代码证据：'+link(REPO/'source/hbf_replay_backend.h','HBF adapter / EOF drain')+' · '+link(upstream/'host/hbf_controller.cpp','HbfController foreground read（4414–4439）、mapping merge（5990 / 6181）')+' · '+link(upstream/'physical/hbf/hbf_device.cpp','HbfDevice time-aware page cache（99–155）')+' · '+link(REPO/'source/stage_replay.h','stage 调度与时间口径')+'</p>',
              '<p>'+link(REPO/'validation/render-multi-backend-report.py','本报告 renderer')+' · '+link(REPO/'docs/multi-backend-stage.md','复现说明')+' · '+link(root/'gddr6/source-result.json','原 direct source result')+' · '+link(root/'gddr6/source-run-receipt.json','direct CPU receipt')+'</p>',
              discovery_section(capture, ncu, observer),
              '<p class="foot muted">静态报告由冻结 JSON 生成，图为内嵌 SVG；刷新报告不触发模拟、GPU 采样或参数拟合。单位：GB / MB 十进制，GiB 二进制；运行成本以分钟展示。</p></main></html>']
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(parts))
    return {'output': str(output), 'bytes': output.stat().st_size, 'verified_backends': list(runs), 'figures': 6 if ncu['ready'] else 4,
            'logical_bytes': common, 'hbf_read_amplification': physical['read_payload_amplification'],
            'verified_discovery_phases': len(capture['rows']), 'verified_discovery_metadata_files': len(capture['files']),
            'formal_NCU_ready': ncu['ready'], 'native_static_census_ready':observer['ready']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-root', type=Path, default=REPO/'build/multi-backend-validation-r3')
    parser.add_argument('--discovery-root', type=Path, default=REPO.parents[2]/'work/p1024d32-capture/discovery-r1')
    parser.add_argument('--ncu-root', type=Path, default=REPO.parents[2]/'work/p1024d32-capture/ncu-three-groups-r1')
    parser.add_argument('--observer-root', type=Path, default=REPO.parents[2]/'work/p1024d32-capture/observer-r2')
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    print(json.dumps(render(args.input_root.resolve(), args.output.resolve(), args.discovery_root.resolve(), args.ncu_root.resolve(), args.observer_root.resolve()), ensure_ascii=False))


if __name__ == '__main__':
    main()
