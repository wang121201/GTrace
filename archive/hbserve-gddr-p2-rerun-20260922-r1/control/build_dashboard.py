"""Render only validated closed data; never execute or modify the model/report."""
from pathlib import Path
from decimal import Decimal as D
from html import escape as esc
import hashlib,json
R=Path(__file__).resolve().parents[3]
O=R/'outputs/ada-gddr-p2-rerun-r1/result-r1'
def pin(p):
 b=p.read_bytes();return dict(path=str(p),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
def need(v,msg):
 if not v:raise ValueError(msg)
def n(v):return D(str(v))
def rd(v):return f'{n(v)/D(10**9):.2f} GB'
def wr(v):return f'{n(v)/D(10**6):.2f} MB' if abs(n(v))>=10**6 else f'{n(v)/D(1000):.2f} KB'
def pct(v):return '—' if v is None else f'{n(v):+.2f}%'
def table(headers,rows):
 return '<div class="scroll"><table><thead><tr>'+''.join('<th>'+esc(h)+'</th>'for h in headers)+'</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+esc(str(v))+'</td>'for v in row)+'</tr>'for row in rows)+'</tbody></table></div>'
before=[pin(O/f)for f in ('data.json','report.md','index.html','receipt.json')]
receipt=json.loads((O/'receipt.json').read_text());d=json.loads((O/'data.json').read_text())
need(d['status']==receipt['status']=='PASS_CLOSED_SAVED_GDDR_P2_RERUN_COMPARISON','closed comparison required')
need(before[0]in receipt['artifacts'],'validated data hash')
need(d['scope']['complete_measured'] and d['scope']['kernels']==1138 and d['scope']['APIs']==29 and d['scope']['layers']==32,'complete scope')
h=d['new']['host'];need(h['owned_target_3600s_met']is False,'explicit target outcome')
need(d['new']['external_preparation_and_owned_total']['full_external_preparation_seconds']is None,'external preparation remains unknown')
by={x['phase']:x for x in d['comparison']};order=('Prefill','Decode1','Decode2','Full');need(set(by)==set(order),'four scopes')
for side in ('old','new'):
 for metric in ('dram_read_bytes','dram_write_bytes','cycles'):
  need(sum(x[metric]for x in d[side]['phases'])==d[side]['full'][metric],'phase/full closure '+side+' '+metric)
rows=[];times=[];intervals=[]
for phase in order:
 m=by[phase]['metrics'];r,w,t,b=(m[k]for k in ('dram_read_bytes','dram_write_bytes','modeled_duration_ns','effective_read_write_GB_s'))
 rows.append([phase]+[rd(r[k])for k in ('old','new','hardware_mean')]+[wr(w[k])for k in ('old','new','hardware_mean')]+[pct(w['new_vs_hardware_mean_percent'])])
 times.append([phase]+[f'{n(t[k])/10**6:.2f}'for k in ('old','new','hardware_mean')]+[f'{n(b[k]):.2f}'for k in ('old','new','hardware_descriptive_estimate')])
 intervals.append([phase,'直接冷前缀实测'if phase in ('Prefill','Full')else'独立冷前缀差分',wr(w['hardware_observed_interval'][0])+' – '+wr(w['hardware_observed_interval'][1])])
body='<header><span class="badge">CLOSED PASS · 完整 Measured 区间完成</span><h1>17.10 Gb/s GDDR 配置：完整重跑结果</h1><p>Llama3-8B · B1 / P32 / D2 · 32 layers</p></header>'
cards=[('完整工作','1138 / 1138 kernels'),('内存 API','29 / 29'),('实际宿主总耗时',f"{h['owned_total_minutes']:.2f} 分钟"),('一小时目标','未达到')]
body+='<section class="cards">'+''.join('<div><small>'+esc(k)+'</small><strong>'+esc(v)+'</strong></div>'for k,v in cards)+'</section>'
body+='<p>复用同一 binary、输入与 17.10 Gb/s 配置。此次重跑仅延长宿主执行时限；相对旧 18.00 Gb/s 基线，只改变 GDDR 数据率。所有 1173 个时间线节点、完整工作与最终 dirty / HBFSIM / writer 总账已闭合。</p>'
body+='<aside><strong>完整运行成功，一小时目标未达到。</strong> Full 写流量接近硬件均值，仍不能代表 Decode 写回准确：D1 / D2 新模型分别为 '+wr(by['Decode1']['metrics']['dram_write_bytes']['new'])+' / '+wr(by['Decode2']['metrics']['dram_write_bytes']['new'])+'，比硬件差分均值低 '+f"{abs(n(by['Decode1']['metrics']['dram_write_bytes']['new_vs_hardware_mean_percent'])):.2f}% / {abs(n(by['Decode2']['metrics']['dram_write_bytes']['new_vs_hardware_mean_percent'])):.2f}%"+'。</aside>'
body+='<h2>DRAM 读写流量</h2><p>旧模型 = 18.00 Gb/s；新模型 = 17.10 Gb/s。NCU 为同一 GPU0 的三组冷前缀统计，单位采用十进制 GB / MB / KB。</p>'
body+=table(['阶段','旧模型读','新模型读','NCU读均值','旧模型写','新模型写','NCU写均值','新写 vs NCU'],rows)
body+='<h2>模型阶段时间与有效带宽</h2><p>模型时间 = 阶段操作周期 × 40000/87 ps（2175 MHz）；带宽 = 读写字节 / 该时间，不使用 HBFSIM 最后完成时间。NCU 是 profiled ROI 时间，Decode 为前缀差分时间；这些均不等于未插桩的自然推理延迟。</p>'
body+=table(['阶段','旧时间 ms','新时间 ms','NCU时间 ms','旧带宽 GB/s','新带宽 GB/s','NCU带宽 GB/s'],times)
body+='<h2>硬件比较口径</h2><p>Prefill / Full 直接测量各自冷前缀；Decode1 / Decode2 是三组独立运行之间的累计差分估计，<strong>不是同次运行的直接阶段计数</strong>。下列区间为观测范围或保守差分范围，不是置信区间。没有硬件逐 layer 读写数据。</p>'+table(['阶段','来源','NCU写流量区间'],intervals)
body+='<h2>宿主成本与适用范围</h2><ul><li>宿主总耗时 '+f"{h['owned_total_seconds']:.2f} 秒（{h['owned_total_minutes']:.2f} 分钟）"+'，含控制器验证与收尾。</li><li>原生准备 '+f"{h['native_preparation_seconds']:.2f} 秒、执行 {h['native_execution_seconds']:.2f} 秒、收尾 {h['native_finalization_seconds']:.2f} 秒"+'；这些是总耗时内部项目，不能再次相加到总耗时。</li><li>外部完整准备耗时未测，记为未知；SOURCE 预检 0.70 秒仅是一项准备成本。本页不作 CPU 加速结论。</li><li>模拟范围是空缓存冷入口 Measured Prefill→D1→D2，未模拟进程初始化或 Warmup，末尾不强制 dirty flush。</li><li>17.10 是观测速率候选，不是完整 GDDR 时序、控制器或地址映射校准。R4 CLOCK / hash2 / carveout / dirty-age / lazy-sector 策略仍未移植。</li></ul>'
body+='<nav><a href="data.json">精确数据与来源</a><a href="report.md">原始文字报告</a><a href="receipt.json">完整分析收据</a><a href="dashboard-receipt.json">本页生成收据</a></nav><footer>只读已验证结果生成；未运行新模拟、编译或 GPU 采集。<br>数据 SHA-256：<code>'+before[0]['sha256']+'</code></footer>'
style='body{font:15px/1.65 system-ui,sans-serif;color:#243246;background:#f5f7fa;max-width:1440px;margin:auto;padding:30px}header{margin-bottom:20px}h1{font-size:29px;line-height:1.3}h2{font-size:21px;margin-top:30px}.badge{color:#166534;font-weight:700}.cards{display:flex;gap:12px;flex-wrap:wrap}.cards>div{background:#fff;border:1px solid #d9e3ed;border-radius:9px;padding:18px;flex:1;min-width:180px}.cards small,.cards strong{display:block}.cards strong{font-size:22px}.scroll{overflow-x:auto;background:#fff;border:1px solid #d9e3ed;border-radius:8px;margin:16px 0}table{width:100%;border-collapse:collapse;white-space:nowrap;font-variant-numeric:tabular-nums}th,td{padding:12px;border-bottom:1px solid #e6ebf1;text-align:right}th{background:#edf2f8}th:first-child,td:first-child{text-align:left}aside{background:#fff5e5;border-left:4px solid #ad670b;padding:16px 20px;margin:24px 0}nav{display:flex;gap:20px;flex-wrap:wrap;margin:30px 0}a{color:#175aa7}footer{color:#607086;font-size:13px}code{word-break:break-all}@media(max-width:700px){body{padding:16px}.cards>div{min-width:145px}h1{font-size:25px}}'
html='<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>17.10 Gb/s 完整重跑结果</title><style>'+style+'</style></head><body>'+body+'</body></html>\n'
need(not(O/'dashboard.html').exists()and not(O/'dashboard-receipt.json').exists(),'new dashboard only')
(O/'dashboard.html').write_text(html)
need(before==[pin(O/f)for f in ('data.json','report.md','index.html','receipt.json')],'original artifacts remain byteexact')
summary=dict(schema='CLOSED_GDDR_P2_DASHBOARD_RENDER_V1',status='PASS_RENDERED_FROM_VALIDATED_COMPLETE_DATA',source_data=before[0],source_comparison_receipt=before[3],source_generator=pin(Path(__file__).resolve()),html=pin(O/'dashboard.html'),original_four_artifacts_unchanged=before,scope=d['scope'],owned_elapsed_minutes=h['owned_total_minutes'],one_hour_target_met=False,external_complete_preparation_seconds=None,no_new_simulation=True,no_CPU_speed_claim=True)
(O/'dashboard-receipt.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(dict(status=summary['status'],html=summary['html'],receipt=pin(O/'dashboard-receipt.json'))))
