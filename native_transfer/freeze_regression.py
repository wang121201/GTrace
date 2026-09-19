#!/usr/bin/env python3
"""Freeze small real captured argument examples and independent old typed answers."""
import argparse
import json
from pathlib import Path

from build_catalog import FAMILIES, RESOURCE_FIELDS, pin


def freeze(root):
    journal = root / 'captured-allargs-norm-r1/observer/process-1925216-857625100/launch-arguments.jsonl'
    records = {}
    for line in journal.read_bytes().splitlines():
        row = json.loads(line)
        records[row['source_launch_key']] = (row, line)
    examples, pins = [], [pin(journal)]
    for family, slug, rev in FAMILIES:
        path = root / ('canonical-' + slug + '-model-' + rev) / 'plan.json'
        plan = json.loads(path.read_text())
        pins.append(pin(path))
        seen = set()
        for call in plan['calls']:
            key = call.get('template_key', call['phase'])
            if key in seen:
                continue
            seen.add(key)
            record, line = records[call['source_launch_key']]
            import hashlib
            if hashlib.sha256(line).hexdigest() != call['argument_line_sha256']:
                raise ValueError('independent plan/raw argument line mismatch')
            launch = {k: call[k] for k in ('source_launch_key', 'phase', 'code_sha256',
                       'parameter_layout_sha256', 'grid', 'block', 'context_id', 'function_id', 'stream_u64')}
            launch.update({k: call['native_resources'][k] for k in RESOURCE_FIELDS})
            launch['argument_sizes'] = [a['size_bytes'] for a in record['arguments']]
            launch['native_launch_id'] = record['native_launch_binding']['native_launch_id']
            if family == 'GEMV':
                expected = {role: call['objects'][role]['pointer'] for role in ('weight', 'input', 'output')}
            else:
                expected = call['arguments']
            examples.append(dict(family=family, raw_record=record, launch=launch,
                                 process=call['process'], expected_parameters=expected,
                                 expected_word_vector=call.get('argument_words'),
                                 plan_pin=pin(path), argument_line_sha256=call['argument_line_sha256']))
    return dict(schema='REAL_CAPTURED_TYPED_PARAMETER_REGRESSION_V1',
                provenance='Raw captured host bytes compared with independently existing typed Model plans',
                source_pins=pins, examples=examples)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    result = freeze(a.source_root)
    a.output.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    print(json.dumps(dict(examples=len(result['examples']), **pin(a.output))))


if __name__ == '__main__':
    main()
