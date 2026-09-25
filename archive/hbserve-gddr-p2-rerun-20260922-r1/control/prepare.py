"""Prepare isolated current-input comparisons; no execution and no trace dump."""
from pathlib import Path
from collections import Counter
import hashlib
import json

D = Path(__file__).resolve().parent
R = D.parents[1]
H = R / 'work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-execution-r1'


def pin(path):
    path = Path(path).resolve()
    data = path.read_bytes()
    return dict(path=str(path), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def write_new(path, value):
    with path.open('x') as f:
        json.dump(value, f, indent=2)
        f.write('\n')


def select(source, registry, first, last):
    rows = source['timeline']
    start = next(i for i, e in enumerate(rows)
                 if e['kind'] == 'native_kernel' and e['native_launch_id'] == first)
    stop = next(i for i, e in enumerate(rows)
                if e['kind'] == 'native_kernel' and e['native_launch_id'] == last)
    chosen = rows[start:stop + 1]
    work = dict(CTAs=0, nodes=0, logical_read_bytes=0, logical_write_bytes=0)
    api = dict(logical_read_bytes=0, logical_write_bytes=0)
    for e in chosen:
        if e['kind'] == 'native_kernel':
            c = registry['entries'][str(e['native_launch_id'])]
            w = c['variants']['min']['expected'] if 'variants' in c else c['expected']
            for k in work:
                work[k] += w[k]
        elif e['kind'] == 'memory_api_submission':
            for effect in e['device_effect_ranges']:
                api['logical_' + effect['operation'].lower() + '_bytes'] += effect['bytes']
    result = json.loads(json.dumps(source))
    result.update(timeline=chosen, counts=dict(Counter(e['kind'] for e in chosen)),
                  kernel_work=work, API_work=api, full_history=False,
                  complete_prefix_from_process_start=False,
                  last_submission_event=chosen[-1]['submission_event'],
                  scope='Cold independent contiguous diagnostic window; NOT full-history accuracy',
                  initial_cache_state='EMPTY_EXPLICIT_DIAGNOSTIC',
                  omitted_prefix_is_not_claimed_equivalent=True,
                  matched_NCU_ROI_available=False)
    result.pop('preparation_script', None)
    return result


def main():
    source = json.loads((H / 'full-history-min-r1.json').read_text())
    for name in ('dispatch', 'history_source', 'common_device_input', 'common_map_source'):
        assert pin(source[name]['path']) == source[name], name
    registry = json.loads(Path(source['dispatch']['path']).read_text())
    out = D / 'plans-r1'
    out.mkdir(exist_ok=False)
    # Intervals chosen from workload roles before collecting candidate results.
    windows = [('prefill-layer0', 1303, 1314),
               ('decode1-entry-layer0', 1696, 1715),
               ('decode2-layer0-heldout', 2070, 2080)]
    receipts = []
    for name, first, last in windows:
        plan = select(source, registry, first, last)
        for profile in ('legacy', 'tuner-v1', 'J-candidate'):
            p = json.loads(json.dumps(plan))
            p['ada_alignment_profile'] = profile
            if profile != 'legacy':
                p['profile']['clock'] = dict(period_ps_numerator=40000,
                                             period_ps_denominator=87,
                                             source='Pinned Accel-Sim core clock 2175 MHz; not this NCU ROI clock measurement')
            destination = out / (name + '-' + profile + '.json')
            write_new(destination, p)
            receipts.append(dict(name=name, profile=profile, plan=pin(destination),
                                 counts=p['counts'], work=p['kernel_work']))
    # Full process-history plan preserves every event. It is prepared, NOT launched.
    full = json.loads(json.dumps(source))
    full['ada_alignment_profile'] = 'tuner-v1'
    full['profile']['clock'] = dict(period_ps_numerator=40000, period_ps_denominator=87,
                                    source='Pinned Accel-Sim core clock 2175 MHz; not this NCU ROI clock measurement')
    full['launch_admission'] = dict(ready=False, user_target_seconds=3600,
        reasons=['Full-history one-hour budget not demonstrated',
                 'New structure adapter requires actual HBFSIM regression',
                 'L1 policy, L2 sector/write allocation, DMA history and controller remain qualified separately'])
    write_new(out / 'full-history-tuner-v1-not-admitted.json', full)
    write_new(out / 'manifest.json', dict(status='PREPARED_NO_SIMULATION_LAUNCHED',
        source=pin(H / 'full-history-min-r1.json'), preparation=pin(__file__),
        windows=receipts, full_plan=pin(out / 'full-history-tuner-v1-not-admitted.json'),
        source_work_unchanged=True, full_trace_saved=False))
    print(json.dumps(dict(status='PREPARED_NO_SIMULATION_LAUNCHED', windows=len(receipts),
                          full_counts=full['counts'], full_work=full['kernel_work'])))


if __name__ == '__main__':
    main()
