"""Export only aggregate status/counts/traffic/timing from task-owned runs.

Raw trace addresses, native names, records and NCU reports stay on XMU.
Use stdout JSON as the only allowed cross-environment result artifact.
"""
import argparse
import hashlib
import json
from pathlib import Path


def load(path):
    return json.loads(path.read_text()) if path.is_file() else None


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, required=True)
    a = ap.parse_args()
    root = a.root.resolve()
    assert root.parent == Path('/home/xmu/nvidiagds/codex-runs')
    assert root.name.startswith('gtsim-ada-r4-llm-20260922-')
    result = dict(schema='LLM_R2_R4_SAFE_AGGREGATES_V1', root=str(root), cases=[])
    for case in ('qwen', 'llama'):
        for profile in ('r2', 'r4'):
            name = case+'-'+profile
            out, job = root/name, root/(name+'-job')
            row = dict(case=case, profile=profile, status='NOT_STARTED', completed_phases=[])
            state = load(out/'status.json')
            if state:
                for k in ('status', 'producer_CPU_minutes', 'child_CPU_minutes', 'wall_minutes', 'executed_counts', 'source_stream'):
                    if k in state:
                        row[k] = state[k]
                # Error text can contain an address or kernel name; only export the class.
                row['error_type'] = state.get('error_type')
                row['state_sha256'] = sha(out/'status.json')
            for suffix in ('start', 'finish'):
                j = load(job/('job-'+suffix+'.json'))
                if j:
                    row['job_'+suffix] = {k:j[k] for k in ('status', 'cpu', 'started_utc', 'finished_utc', 'CPU_minutes', 'wall_minutes') if k in j}
                    if 'process' in j:
                        row['cleanup_verified'] = j['process']['cleanup']['owned_descendants_empty']
            progress = out/'progress.jsonl'
            if progress.is_file():
                for line in reversed(progress.read_text().splitlines()):
                    try:
                        p = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    row['progress'] = {k:p[k] for k in ('event', 'phase', 'source_event_ordinal', 'counts', 'wall_seconds', 'producer_CPU_seconds') if k in p}
                    break
            snapshots = out/'cache-snapshots.jsonl'
            phases, shared = {}, {}
            if snapshots.is_file():
                for line in snapshots.open():
                    try:
                        s = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if s['type'] == 'snapshot':
                        phases[s['label']] = s
                    if s['type'] == 'kernel_L1_configuration':
                        c = s.get('L1', {})
                        if c:
                            key = str(c['shared_carveout_bytes'])
                            shared[key] = shared.get(key, 0)+1
                for label, after in phases.items():
                    if not label.endswith('/end') or label[:-4]+'/begin' not in phases:
                        continue
                    before = phases[label[:-4]+'/begin']
                    b, e = before['cumulative'], after['cumulative']
                    d = dict(phase=label[:-4], dirty_start_bytes=b['dirty_tail_bytes'], dirty_end_bytes=e['dirty_tail_bytes'],
                             cache_CPU_minutes=after['CPU_minutes_since_run_start']-before['CPU_minutes_since_run_start'])
                    keys = ['DRAM_read_bytes','DRAM_write_bytes','source_read_effect_bytes','source_write_effect_bytes',
                            'age_writeback_bytes','capacity_eviction_writeback_bytes','L2_forwarded_access_sequence',
                            'L1_read_hits','L1_read_misses','L1_pre_reads','L1_pre_writes',
                            'L1_bypass','L1_evictions','L1_read_sector_requests','L1_read_sector_hits',
                            'L1_read_sector_misses','L1_forwarded_read_sector_requests','L1_forwarded_write_sector_requests']
                    for k in keys:
                        if k in b and k in e:
                            d[k] = e[k]-b[k]
                    row['completed_phases'].append(d)
            summary = load(out/'cache-summary.json')
            if summary:
                row['summary_sha256'] = sha(out/'cache-summary.json')
                row['configuration'] = summary['configuration']
                row['cache_CPU_minutes'] = summary['CPU_minutes']
                # Snapshot has scalar aggregate counters and opaque hashes, no memory addresses.
                row['final_counters'] = {k:v for k,v in summary['snapshot'].items()
                                         if isinstance(v, (int,float)) and not any(x in k.lower() for x in ('address','covered_min','covered_max'))}
            row['shared_kernel_counts'] = shared
            result['cases'].append(row)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
