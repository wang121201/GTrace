#!/usr/bin/env python3
"""Validate every old five-family call against real captured bytes and typed plans."""
import argparse
import collections
import json
import resource
import time
from pathlib import Path

from build_catalog import FAMILIES
from decode import Decoder, RESOURCE_FIELDS, digest, file_pin, need, verify_file


def validate(root):
    start = time.monotonic()
    usage = resource.getrusage(resource.RUSAGE_SELF)
    decoder = Decoder()
    fixture = json.loads(Path(__file__).with_name('regression_inputs.json').read_text())
    pins = {Path(p['path']).relative_to(root): p for p in fixture['source_pins']}
    evidence = [verify_file(root / relative, pin) for relative, pin in pins.items()]
    journal = root / 'captured-allargs-norm-r1/observer/process-1925216-857625100/launch-arguments.jsonl'
    raw_rows = {}
    for line in journal.read_bytes().splitlines():
        row = json.loads(line)
        raw_rows[row['source_launch_key']] = (row, digest(line))
    counts = collections.Counter()
    for family, slug, rev in FAMILIES:
        plan = json.loads((root / ('canonical-' + slug + '-model-' + rev) / 'plan.json').read_text())
        for call in plan['calls']:
            row, sha = raw_rows[call['source_launch_key']]
            need(sha == call['argument_line_sha256'], 'raw/independent typed plan identity')
            launch = {k: call[k] for k in ('source_launch_key', 'phase', 'code_sha256',
                'parameter_layout_sha256', 'grid', 'block', 'context_id', 'function_id', 'stream_u64')}
            launch.update({k: call['native_resources'][k] for k in RESOURCE_FIELDS})
            launch.update(native_launch_id=row['native_launch_binding']['native_launch_id'],
                          argument_sizes=[a['size_bytes'] for a in row['arguments']])
            result = decoder.decode(row, launch, call['process'], allow_legacy=True)
            expected = ({role: call['objects'][role]['pointer'] for role in ('weight', 'input', 'output')}
                        if family == 'GEMV' else call['arguments'])
            need(result['status'] == 'DECODED_HOST_PARAMETERS_ONLY' and result['family'] == family and
                 result['typed_parameters'] == expected, 'independent full-corpus typed regression')
            if family == 'GEMV':
                need(result['argument_words'] == call['argument_words'], 'all GEMV captured words')
            need(result['phase'] == call['phase'] and not result['native_model_admitted'], 'scope preserved')
            counts[family] += 1
    need(sum(counts.values()) == 646, 'old five-family corpus count')
    for pin in evidence:
        verify_file(Path(pin['path']), pin)
    after = resource.getrusage(resource.RUSAGE_SELF)
    return dict(schema='NATIVE_TYPED_PARAMETER_LEGACY_REGRESSION_V1',
                status='PASS_646_REAL_CAPTURED_CALLS_TYPED_ONLY', cases=sum(counts.values()),
                family_counts=dict(counts), input_pins=evidence, catalog=decoder.catalog_pin,
                source_pins=[file_pin(Path(__file__)), file_pin(Path(__file__).with_name('decode.py'))],
                elapsed_seconds=time.monotonic()-start,
                CPU_user_system_seconds=(after.ru_utime-usage.ru_utime)+(after.ru_stime-usage.ru_stime),
                GPU_used=False, device_memory_dereferenced=False, native_model_admitted=False)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    result = validate(a.source_root)
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('status', 'cases', 'family_counts', 'CPU_user_system_seconds')}))


if __name__ == '__main__':
    main()
