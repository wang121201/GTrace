#!/usr/bin/env python3
"""Remote-local closure/aggregation of shared-cache r4 replay; never launches a workload.

Raw requests are not opened. Hardware and frozen predictions stay on the host.
stdout contains aggregates only. Per-case derived counts are written locally to
the requested fresh output directory; no addresses, input paths or requests.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
from statistics import median
import time

MODELS = ('LRU_u128_s4_h0_c1000', 'CLOCK_u128_s16_h2_c1062')
CAPACITY = {32768: (98304, 102400), 65536: (65536, 67584), 102400: (28672, 28672)}


def require(ok, code):
    if not ok:
        raise ValueError(code)


def number(value):
    require(isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value) and value >= 0, 'invalid_nonnegative_number')
    return value


def integer(value):
    value = number(value)
    require(value == int(value), 'expected_integer_count')
    return int(value)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def validate_replay_receipt(receipt, prediction_sha256):
    require(receipt.get('schema') == 'GTSIM_ADA_R4_SERIAL_RECEIPT_V1', 'unexpected_replay_receipt_schema')
    require(receipt.get('status') == 'PASS_PROCESS_ONLY', 'replay_process_not_successfully_closed')
    require(receipt.get('source_unchanged') is True and receipt.get('input_unchanged') is True,
            'replay_source_or_input_changed')
    require(type(receipt.get('case_count')) is int and receipt['case_count'] == 384,
            'replay_receipt_requires_384_cases')
    require(type(receipt.get('returncode')) is int and receipt['returncode'] == 0,
            'replay_process_returncode_failed')
    require(receipt.get('GPU_executed') is False, 'replay_receipt_must_be_CPU_only')
    require(isinstance(prediction_sha256, str) and len(prediction_sha256) == 64 and
            all(c in '0123456789abcdef' for c in prediction_sha256) and
            receipt.get('prediction_sha256') == prediction_sha256,
            'replay_prediction_sha256_mismatch')
    return dict(status=receipt['status'], case_count=384, returncode=0,
                source_unchanged=True, input_unchanged=True, GPU_executed=False,
                prediction_sha256=prediction_sha256)


def aggregate(rows):
    reference = sum(r['NCU_median_L2_read_sectors'] for r in rows)
    predicted = sum(r['L2_read_sectors'] for r in rows)
    absolute = sum(abs(r['L2_read_sectors'] - r['NCU_median_L2_read_sectors']) for r in rows)
    return dict(conditions=len(rows), predicted_L2_read_sectors=predicted,
                NCU_median_L2_read_sectors=reference,
                absolute_error_sum_sectors=absolute,
                WAPE_percent=100 * absolute / reference if reference else None,
                signed_aggregate_error_sectors=predicted-reference,
                signed_aggregate_error_percent=100*(predicted-reference)/reference if reference else None,
                per_condition_within_2percent_or_2sectors=sum(
                    abs(r['L2_read_sectors']-r['NCU_median_L2_read_sectors']) <=
                    max(2, .02*r['NCU_median_L2_read_sectors']) for r in rows),
                frozen_prediction_mismatches=sum(not r['frozen_prediction_equal'] for r in rows),
                replay_CPU_minutes=sum(r['CPU_seconds'] for r in rows)/60,
                replay_wall_minutes=sum(r['wall_seconds'] for r in rows)/60)


def evaluate(replays, hardware, frozen):
    require(set(hardware) == {str(i) for i in range(384)}, 'hardware_requires_all_384_conditions')
    index = {}
    for row in replays:
        require(row.get('schema') == 'ADA_R4_SHARED_SERIAL_CASE_V1' and
                row.get('status') == 'PASS', 'noncompleted_replay_row')
        cid = integer(row['case_id'])
        require(cid < 384 and row['model_id'] in MODELS, 'unsupported_case_or_model')
        key = row['model_id'], cid
        require(key not in index, 'duplicate_replay_case_model')
        index[key] = row
    require(set(index) == {(m, i) for m in MODELS for i in range(384)}, 'incomplete_two_model_384_case_replay')
    derived = []
    for cid in range(384):
        hw = hardware[str(cid)]
        case = hw['case']
        require(integer(case['id']) == cid, 'hardware_case_identity_mismatch')
        repeats = hw['repeats']
        require(len(repeats) == 5, 'five_NCU_repeats_required')
        shared = integer(hw['median']['shared_bytes'])
        require(shared in CAPACITY and all(integer(r['shared_bytes']) == shared for r in repeats),
                'observed_shared_carveout_not_constant')
        reference = number(hw['median']['l2_reads'])
        require(median(number(r['l2_reads']) for r in repeats) == reference, 'NCU_median_repeat_mismatch')
        cg = bool(case['cg'])
        if cid < 372:
            require(cg == (360 <= cid), 'frozen_cg_partition_mismatch')
        category = 'fresh_ca' if cid < 360 else ('cg_controls' if cid < 372 else 'cross_time_anchors')
        for model_index, model in enumerate(MODELS):
            row = index[model, cid]
            r4 = model_index == 1
            expected_capacity = CAPACITY[shared][model_index]
            sets = 16 if r4 else 4
            require(integer(row['observed_shared_bytes']) == shared and
                    integer(row['nominal_l1_bytes']) == CAPACITY[shared][0], 'case_shared_or_nominal_mismatch')
            require(integer(row['capacity_bytes']) == expected_capacity and
                    integer(row['sets']) == sets and integer(row['ways']) == expected_capacity//128//sets and
                    row['policy'] == ('CLOCK' if r4 else 'LRU') and
                    integer(row['allocation_unit']) == 128 and integer(row['sector_bytes']) == 32 and
                    integer(row['hash']) == (2 if r4 else 0) and integer(row['scale']) == (1062 if r4 else 1000),
                    'frozen_policy_or_geometry_mismatch')
            hits, misses, requests = (integer(row[n]) for n in ('hits', 'misses', 'requests'))
            require(hits+misses == requests == integer(row['expected_requests']) and
                    misses == integer(row['L2_read_sectors']), 'replay_count_closure_failed')
            require(bool(row['cg']) == cg, 'case_cg_mismatch')
            require(requests == integer(case['scalar_loads']), 'request_count_differs_from_hardware_case')
            if cg:
                require(hits == 0 and misses == requests, 'cg_bypass_not_exact')
            pred = frozen[model][str(cid)]
            require(integer(pred['hits'])+integer(pred['misses']) == requests, 'frozen_source_count_mismatch')
            match = (hits == integer(pred['hits']) and misses == integer(pred['misses']) and
                     expected_capacity == integer(pred['capacity_bytes']))
            delta = misses-reference
            derived.append(dict(model_id=model, case_id=cid, category=category,
                observed_shared_bytes=shared, stride_y=case['y'], order_z=case['z'],
                read_group_stable=bool(hw['read_group_stable']), requests=requests,
                hits=hits, L2_read_sectors=misses, NCU_median_L2_read_sectors=reference,
                NCU_repeat_count=5, frozen_hits=integer(pred['hits']),
                frozen_L2_read_sectors=integer(pred['misses']), frozen_prediction_equal=match,
                capacity_bytes=expected_capacity, signed_error_sectors=delta,
                absolute_error_sectors=abs(delta),
                signed_error_percent=100*delta/reference if reference else None,
                CPU_seconds=number(row['CPU_seconds']), wall_seconds=number(row['wall_seconds'])))
    models = {}
    for model in MODELS:
        own = [r for r in derived if r['model_id'] == model]
        fresh = [r for r in own if r['category'] == 'fresh_ca']
        grouped = {}
        for key in ('observed_shared_bytes', 'stride_y', 'order_z', 'read_group_stable'):
            groups = defaultdict(list)
            for row in fresh:
                groups[str(row[key])].append(row)
            grouped[key] = {label: aggregate(items) for label, items in sorted(groups.items())}
        cg = [r for r in own if r['category'] == 'cg_controls']
        models[model] = dict(fresh_ca=aggregate(fresh), fresh_groups=grouped,
            cg_controls={**aggregate(cg), 'exact_replay_bypass_pass': all(r['L2_read_sectors'] == r['requests'] for r in cg),
                         'exact_NCU_bypass_pass': all(r['NCU_median_L2_read_sectors'] == r['requests'] for r in cg)},
            cross_time_anchors=aggregate([r for r in own if r['category'] == 'cross_time_anchors']),
            all_cases=aggregate(own))
    mismatches = sum(not row['frozen_prediction_equal'] for row in derived)
    return dict(schema='ADA_R4_SHARED_SERIAL_EVALUATION_V1',
                status='PASS_REFERENCE_EQUIVALENCE' if not mismatches else 'FAIL_REFERENCE_EQUIVALENCE',
                condition_count=384, model_case_count=768, fresh_conditions=360, cg_conditions=12,
                anchor_conditions=12, NCU_repeats_per_condition=5,
                frozen_prediction_mismatches=mismatches, models=models,
                scope='Shared PerSmL1Cache synchronous scalar-read L1 filter; L2 read sectors only. No DRAM-byte/timing/write/LLM qualification.',
                timing_scope='Replay CPU/wall includes cold cache construction, excludes input IO; analysis time separate.',
                no_cases_dropped=True, parameter_selection_performed=False), derived


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--replay-jsonl', type=Path, required=True)
    ap.add_argument('--replay-receipt', type=Path, required=True)
    ap.add_argument('--hardware', type=Path, required=True)
    ap.add_argument('--frozen', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    cpu, wall = time.process_time(), time.monotonic()
    try:
        require(not args.output.exists(), 'output_directory_already_exists')
        inputs = {'replay': args.replay_jsonl, 'replay_receipt': args.replay_receipt,
                  'hardware': args.hardware, 'frozen': args.frozen}
        pins = {k: digest(p) for k, p in inputs.items()}
        verified_receipt = validate_replay_receipt(json.loads(args.replay_receipt.read_text()), pins['replay'])
        with args.replay_jsonl.open() as stream:
            rows = [json.loads(line) for line in stream if line.strip()]
        summary, derived = evaluate(rows, json.loads(args.hardware.read_text()), json.loads(args.frozen.read_text()))
        require(pins == {k: digest(p) for k, p in inputs.items()}, 'input_changed_during_analysis')
        summary['input_sha256'] = pins
        summary['replay_receipt_verified'] = verified_receipt
        summary['analysis_CPU_minutes'] = (time.process_time()-cpu)/60
        summary['analysis_wall_minutes'] = (time.monotonic()-wall)/60
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output/'case-derived.jsonl').write_text(''.join(json.dumps(r, sort_keys=True)+'\n' for r in derived))
        (args.output/'summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True)+'\n')
        print(json.dumps(summary, sort_keys=True))
        return 0 if summary['status'] == 'PASS_REFERENCE_EQUIVALENCE' else 1
    except (ValueError, KeyError, OSError, TypeError, OverflowError):
        # Do not leak a request path, address or raw input in exceptions.
        print(json.dumps({'status': 'FAIL_ANALYSIS_CLOSURE', 'error': 'invalid_or_incomplete_remote_evidence'}))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
