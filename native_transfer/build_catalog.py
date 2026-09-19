#!/usr/bin/env python3
"""Export a small pinned decoder catalog from existing sealed typed plans."""
import argparse
import hashlib
import json
from pathlib import Path

FAMILIES = [('GEMV', 'gemv', 'r2'), ('PlainNorm', 'norm', 'r1'),
            ('FusedNorm', 'fusednorm', 'r1'), ('SiLU', 'silu', 'r1'),
            ('Rotary', 'rotary', 'r1')]
RESOURCE_FIELDS = ('static_shared_bytes', 'dynamic_shared_bytes', 'registers', 'local_bytes_per_thread')


def pin(path):
    raw = path.read_bytes()
    return dict(path=str(path.resolve()), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def build(root):
    families, evidence = [], {}
    for family, slug, rev in FAMILIES:
        path = root / ('canonical-' + slug + '-model-' + rev) / 'plan.json'
        data = json.loads(path.read_text())
        evidence[str(path)] = pin(path)
        model = path.with_name('model.py')
        evidence[str(model)] = pin(model)
        sample = data['calls'][0]
        fields = []
        for item in sample.get('typed_arguments', []):
            fields.append(dict(index=item['index'], name=item['name'],
                               bytes=item.get('size_bytes', item.get('bytes'))))
        pointer_names = {'GEMV': ['weight', 'input', 'output'],
                         'PlainNorm': ['input', 'weight', 'output'],
                         'FusedNorm': ['input', 'residual', 'weight'],
                         'SiLU': ['out', 'input'],
                         'Rotary': ['q', 'k', 'q_rope', 'k_rope', 'cos_sin_cache', 'pos_ids']}[family]
        entries, seen = [], set()
        for call in data['calls']:
            entry = dict(code_sha256=call['code_sha256'],
                         parameter_layout_sha256=call['parameter_layout_sha256'],
                         grid=call['grid'], block=call['block'],
                         resources={k: call['native_resources'][k] for k in RESOURCE_FIELDS})
            if family == 'GEMV':
                name = call['template_key']
                ref = data['templates'][name]
                template = json.loads(Path(ref['path']).read_text())
                entry.update(template_id=name, template_phase=call['phase'],
                             argument_sizes=[152], argument_words=template['argument_words'],
                             K=template['K'], N=template['N'],
                             pointer_alignment_mod128=template['pointer_alignment_mod128'])
            else:
                phase = call['phase']
                if family == 'PlainNorm' and phase != 'Prefill':
                    phase = 'Decode1'
                ref = data['templates'][phase] if 'templates' in data else data['template']
                entry.update(template_id=family + ':' + phase, template_phase=phase,
                             argument_sizes=[f['bytes'] for f in fields],
                             scalars={k: v for k, v in call['arguments'].items() if k not in pointer_names})
            actual = pin(Path(ref['path']))
            if actual != ref:
                raise ValueError('sealed template pin changed: ' + ref['path'])
            evidence[ref['path']] = actual
            entry['template_pin'] = ref
            key = json.dumps(entry, sort_keys=True)
            if key not in seen:
                entries.append(entry)
                seen.add(key)
        families.append(dict(family=family, fields=fields, pointer_names=pointer_names,
                             templates=entries))
    return dict(schema='NATIVE_HOST_ARGUMENT_DECODER_CATALOG_V1',
                qualification='SEALED_SOURCE_PARAMETER_SHAPES_ONLY_NOT_TARGET_TRANSFER',
                families=families, source_pins=list(evidence.values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = (json.dumps(build(args.source_root), indent=2, sort_keys=True) + '\n').encode()
    args.output.write_bytes(raw)
    print(json.dumps(dict(path=str(args.output), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())))


if __name__ == '__main__':
    main()
