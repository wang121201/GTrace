#!/usr/bin/env python3
"""Render an SVG engineering timeline directly from simulator stage events."""
import argparse
import html
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--runs', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
plans = []
for window in (1, 2):
    data = json.loads((a.runs/('decode-w'+str(window))/'result.json').read_text())
    us = data['clock']['period_ps_numerator']/data['clock']['period_ps_denominator']/1e6
    plans.append([(r['first_admission_cycle']*us, r['memory_ready_cycle']*us,
                   r['compute_start_cycle']*us, r['compute_finish_cycle']*us) for r in data['stages'][:4]])
end = max(r[3] for rows in plans for r in rows)*1.03
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1080" height="600" viewBox="0 0 1080 600">',
         '<rect width="1080" height="600" fill="white"/>',
         '<style>text{font-family:Arial,sans-serif;fill:#172b4d} .label{font-size:15px}</style>']
def text(x, y, value, size=15):
    parts.append(f'<text x="{x}" y="{y}" font-size="{size}">{html.escape(value)}</text>')
def rect(x, y, width, color):
    parts.append(f'<rect x="{x:.3f}" y="{y}" width="{width:.3f}" height="23" rx="2" fill="{color}"/>')
text(28, 36, 'Unchanged direct trace + explicit stage overlap', 25)
text(28, 65, 'B1 · 48 CTAs/group · uncalibrated timing approximation', 17)
for panel, rows in enumerate(plans):
    top = 120+panel*207
    text(28, top, 'Window 1 — serial stages' if panel == 0 else 'Window 2 — next-group prefetch', 19)
    for tick in range(6):
        x = 175+tick*170
        parts.append(f'<path d="M{x} {top+15}V{top+158}" stroke="#e5e7eb"/>')
        if panel == 1:
            text(x-7, top+181, f'{end*tick/5:.1f}')
    for i, (start, ready, comp, finish) in enumerate(rows):
        y = top+23+i*35
        text(30, y+17, 'CTA group '+str(i))
        rect(175+850*start/end, y, 850*(ready-start)/end, '#E69F00')
        rect(175+850*comp/end, y, 850*(finish-comp)/end, '#0072B2')
text(320, 538, 'Modeled time (microseconds); first four groups of selected GEMV')
rect(28, 555, 18, '#E69F00')
text(56, 573, 'First admission → all group memory complete (not bus busy)')
rect(727, 555, 18, '#0072B2')
text(757, 573, 'Estimated compute')
parts.append('</svg>')
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text('\n'.join(parts)+'\n')
