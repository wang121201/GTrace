#!/usr/bin/env python3
"""Decode captured host parameters; never read device memory or admit a model."""
import argparse
import collections
import hashlib
import json
import re
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_CATALOG_SHA256 = '233b5ea04146b6c3835467d0813553aa553174299771c4ab8ae9a9a84fc9fdb2'
RESOURCE_FIELDS = ('static_shared_bytes', 'dynamic_shared_bytes', 'registers', 'local_bytes_per_thread')
JOURNALS = {'launch-journal.jsonl', 'scope-journal.jsonl', 'functions.jsonl',
            'static-instructions.jsonl', 'lifecycle.jsonl', 'allocation-journal.jsonl',
            'launch-arguments.jsonl'}
U64 = (1 << 64) - 1


class InvalidCapture(ValueError):
    """Malformed or inconsistent evidence: no PASS receipt may be published."""


def need(ok, message):
    if not ok:
        raise InvalidCapture(message)


def natural(value, cap=U64):
    need(type(value) is int and 0 <= value <= cap, 'unsigned integer out of range')
    return value


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def strict_json(raw):
    def pairs(items):
        out = {}
        for key, value in items:
            need(key not in out, 'duplicate JSON key: ' + key)
            out[key] = value
        return out
    return json.loads(raw, object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(InvalidCapture('nonfinite JSON')))


def bounded_read(path, cap):
    path = Path(path)
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= cap,
         'bounded regular input required: ' + str(path))
    with path.open('rb') as handle:
        raw = handle.read(cap + 1)
    need(len(raw) <= cap, 'input grew beyond bound')
    return raw


def file_pin(path, raw=None):
    raw = bounded_read(path, 256 << 20) if raw is None else raw
    return dict(path=str(Path(path).resolve()), bytes=len(raw), sha256=digest(raw))


def verify_file(path, expected, cap=1 << 30):
    path = Path(path)
    size = natural(expected['bytes'], cap)
    need(path.is_file() and not path.is_symlink() and path.stat().st_size == size,
         'evidence file length/type: ' + str(path))
    h = hashlib.sha256()
    actual = 0
    with path.open('rb') as handle:
        while True:
            block = handle.read(1 << 20)
            if not block:
                break
            actual += len(block)
            need(actual <= size, 'evidence file grew')
            h.update(block)
    need(actual == size and h.hexdigest() == expected['sha256'], 'evidence file SHA: ' + str(path))
    return dict(path=str(path.resolve()), bytes=size, sha256=h.hexdigest())


def validate_payload(record, allow_legacy=False):
    accepted = {'SG_NATIVE_ARGUMENT_VECTOR_V1'}
    if allow_legacy:
        accepted.add('SG_NATIVE_ALL1138_ARGUMENT_VECTOR_V1')
    need(record.get('schema') in accepted, 'argument record schema')
    need(record.get('argument_transport') == 'kernelParams' and
         record.get('capture_before_original_launch') is True and
         record.get('device_memory_dereferenced') is False, 'host-only pre-launch argument contract')
    process = record['native_launch_binding']['process']
    need(set(process) == {'pid', 'start_ticks'} and all(natural(x) > 0 for x in process.values()),
         'actual process identity')
    epoch, ordinal = natural(record['epoch_id'], 64), natural(record['epoch_launch_ordinal'], 100000)
    need(epoch > 0 and record['source_launch_key'] == 'epoch-%d-launch-%d' % (epoch, ordinal),
         'argument source key chronology')
    need(record['native_launch_binding']['source_launch_key'] == record['source_launch_key'] and
         natural(record['native_launch_binding']['native_launch_id']) > 0, 'argument native launch identity')
    for field in ('context_id', 'function_id', 'stream_u64'):
        natural(record[field])
    need(re.fullmatch(r'Prefill|Decode[1-9][0-9]*', record['phase']) is not None, 'actual phase label')
    need(all(re.fullmatch(r'[0-9a-f]{64}', record[k]) for k in
             ('code_sha256', 'parameter_layout_sha256')), 'code/ABI SHA syntax')
    args = record['arguments']
    need(type(args) is list and 0 < len(args) <= 256, 'argument count bound')
    values, sizes = [], []
    for index, item in enumerate(args):
        size = natural(item['size_bytes'], 65536)
        need(type(item['index']) is int and item['index'] == index and size > 0 and
             item['parameter_buffer_offset'] is None, 'separate argument ABI boundaries')
        text = item['raw_bytes_hex']
        need(type(text) is str and len(text) == 2 * size and
             re.fullmatch(r'[0-9a-f]*', text) is not None, 'bounded raw argument hex')
        raw = bytes.fromhex(text)
        need(digest(raw) == item['sha256'], 'raw argument SHA mismatch')
        values.append(raw)
        sizes.append(size)
    need(sum(sizes) <= 1 << 20, 'per-launch raw byte bound')
    # Producer hashes the exact compact argument-size vector.
    need(digest(json.dumps(sizes, separators=(',', ':')).encode()) == record['parameter_layout_sha256'],
         'argument vector does not match ABI SHA')
    return values, sizes


def join_launch(record, launch, process, allow_legacy=False):
    values, sizes = validate_payload(record, allow_legacy)
    need(canonical(record['native_launch_binding']['process']) == canonical(process),
         'cross-process argument substitution')
    for field in ('source_launch_key', 'phase', 'code_sha256', 'parameter_layout_sha256',
                  'context_id', 'function_id', 'stream_u64'):
        need(canonical(record[field]) == canonical(launch[field]), 'argument/native census mismatch: ' + field)
    if 'native_launch_id' in launch:
        need(record['native_launch_binding']['native_launch_id'] == launch['native_launch_id'],
             'argument/native launch ID mismatch')
    need(sizes == launch['argument_sizes'], 'argument/native ABI size mismatch')
    for field in ('grid', 'block'):
        need(type(launch[field]) is list and len(launch[field]) == 3 and
             all(natural(v, (1 << 32) - 1) > 0 for v in launch[field]), 'native geometry shape')
        if not allow_legacy or field in record:
            need(record[field] == launch[field], 'argument/native geometry mismatch: ' + field)
    for field in RESOURCE_FIELDS:
        natural(launch[field])
        if not allow_legacy or field in record:
            need(record[field] == launch[field], 'argument/native resource mismatch: ' + field)
    # Consumer naming has aliases for scope IDs; source labels are never rewritten.
    aliases = {'call_id': 'module_call_id', 'layer_id': 'layer'}
    for field in ('epoch_id', 'epoch_launch_ordinal', 'forward_id', 'module_scope',
                  'module_kernel_ordinal', 'cuda_api', 'launch_attributes', 'code_sha256_kind',
                  'call_id', 'layer_id'):
        raw_field = aliases.get(field, field)
        if field in launch:
            need(raw_field in record and launch[field] == record[raw_field],
                 'argument/native scope mismatch: ' + field)
    return values, sizes


class Decoder:
    def __init__(self, catalog_path=None, catalog_sha256=None):
        path = Path(catalog_path) if catalog_path else HERE / 'template_catalog.json'
        want = catalog_sha256 or (DEFAULT_CATALOG_SHA256 if not catalog_path else None)
        need(isinstance(want, str) and re.fullmatch(r'[0-9a-f]{64}', want) is not None,
             'explicit catalog SHA required')
        raw = bounded_read(path, 1 << 20)
        need(digest(raw) == want, 'decoder catalog SHA mismatch')
        self.catalog = strict_json(raw)
        need(self.catalog['schema'] == 'NATIVE_HOST_ARGUMENT_DECODER_CATALOG_V1', 'catalog schema')
        self.catalog_pin = file_pin(path, raw)
        self.by_code = {}
        for family in self.catalog['families']:
            for template in family['templates']:
                self.by_code.setdefault(template['code_sha256'], []).append((family, template))

    def decode(self, record, launch, process, allow_legacy=False):
        values, sizes = join_launch(record, launch, process, allow_legacy)
        output = {k: record[k] for k in ('source_launch_key', 'phase', 'code_sha256',
                  'parameter_layout_sha256', 'context_id', 'function_id', 'stream_u64')}
        output.update(native_launch_binding=record['native_launch_binding'],
                      grid=launch['grid'], block=launch['block'], argument_sizes=sizes,
                      canonical_argument_content_sha256=digest(canonical(record)),
                      status='UNSUPPORTED_TYPED_BINDING', native_model_admitted=False,
                      dynamic_memory_validated=False, register_control_transfer_validated=False,
                      object_root_binding_validated=False)
        output['native_scope'] = {k: launch[k] for k in
            ('epoch_id', 'epoch_launch_ordinal', 'forward_id', 'module_scope', 'call_id', 'layer_id',
             'module_kernel_ordinal', 'dynamic_module', 'module_ancestry', 'cuda_api', 'launch_attributes',
             'code_sha256_kind', 'role') if k in launch}

        def unsupported(reason):
            output['reason'] = reason
            output['template_candidates'] = []
            return output

        candidates = self.by_code.get(record['code_sha256'], [])
        if not candidates:
            return unsupported('UNSUPPORTED_CODE_SHA256')
        output['family'] = candidates[0][0]['family']
        candidates = [(f, t) for f, t in candidates if
                      t['parameter_layout_sha256'] == record['parameter_layout_sha256'] and
                      t['argument_sizes'] == sizes]
        if not candidates:
            return unsupported('UNSUPPORTED_ABI_FOR_CODE')
        candidates = [(f, t) for f, t in candidates if t['grid'] == launch['grid'] and
                      t['block'] == launch['block'] and
                      all(t['resources'][k] == launch[k] for k in RESOURCE_FIELDS)]
        if not candidates:
            return unsupported('UNSUPPORTED_STATIC_GEOMETRY_OR_RESOURCES')
        family = candidates[0][0]
        name = family['family']
        typed, fields = {}, []
        if name == 'GEMV':
            words = list(struct.unpack('<38I', values[0]))
            typed = {role: words[index] + (words[index + 1] << 32)
                     for role, index in [('weight', 0), ('input', 4), ('output', 8)]}
            if words[8:10] != words[12:14]:
                return unsupported('GEMV_EPILOGUE_OUTPUT_ALIAS_MISMATCH')
            pointer_words = {0, 1, 4, 5, 8, 9, 12, 13}
            candidates = [(f, t) for f, t in candidates if all(
                words[i] == t['argument_words'][i] for i in range(38) if i not in pointer_words)]
            if not candidates:
                return unsupported('GEMV_120_NONPOINTER_BYTES_HAVE_NO_SEALED_TEMPLATE')
            candidates = [(f, t) for f, t in candidates if all(
                typed[role] > 0 and typed[role] % 128 == t['pointer_alignment_mod128'][role]
                for role in family['pointer_names'])]
            if not candidates:
                return unsupported('GEMV_POINTER_ALIGNMENT_DOMAIN')
            k, n = candidates[0][1]['K'], candidates[0][1]['N']
            if not (words[35] > 0 and k % (launch['block'][0] * words[35]) == 0 and
                    n == launch['grid'][0] * launch['block'][1]):
                return unsupported('GEMV_NO_TAIL_GEOMETRY_MISMATCH')
            output.update(argument_words=words, template_dimensions=dict(K=k, N=n),
                          nonpointer_bytes_compared=120, output_epilogue_alias=True,
                          captured_aliases={'epilogue_input': dict(argument_index=0, byte_offset=48,
                              bytes=8, value_u=typed['output'], alias_of='output')})
            for role, offset in [('weight', 0), ('input', 16), ('output', 32)]:
                fields.append(dict(name=role, argument_index=0, byte_offset=offset, bytes=8,
                                   type='captured_pointer_u64', value_u=typed[role]))
            extents = dict(weight=2*k*n, input=2*k, output=2*n)
        else:
            for spec, raw, arg in zip(family['fields'], values, record['arguments']):
                value = int.from_bytes(raw, 'little')
                typed[spec['name']] = value
                fields.append(dict(name=spec['name'], argument_index=spec['index'], bytes=spec['bytes'],
                                   type='captured_pointer_u64' if spec['name'] in family['pointer_names']
                                   else 'float32_raw_bits' if spec['name'].endswith('_bits') else 'unsigned_integer',
                                   value_u=value, raw_bytes_sha256=arg['sha256']))
            candidates = [(f, t) for f, t in candidates if all(typed[k] == v for k, v in t['scalars'].items())]
            if not candidates:
                return unsupported('SCALAR_VALUES_HAVE_NO_SEALED_TEMPLATE')
            for role in family['pointer_names']:
                alignment = 8 if role == 'pos_ids' else 16
                if not typed[role] or typed[role] % alignment:
                    return unsupported('POINTER_ALIGNMENT_DOMAIN:' + role)
            rows = launch['grid'][0]
            if name in ('PlainNorm', 'FusedNorm'):
                extents = {role: 8192*(1 if role == 'weight' else rows) for role in family['pointer_names']}
            elif name == 'SiLU':
                extents = dict(input=57344*rows, out=28672*rows)
                if not (typed['input'] + extents['input'] <= typed['out'] or
                        typed['out'] + extents['out'] <= typed['input']):
                    return unsupported('SILU_RESTRICT_ALIAS_VIOLATION')
            else:
                if typed['q'] != typed['q_rope'] or typed['k'] != typed['k_rope']:
                    return unsupported('ROTARY_INPLACE_ALIAS_MISMATCH')
                extents = {'pos_ids': 8*typed['nnz']}
                for role in ('q', 'k', 'q_rope', 'k_rope'):
                    heads = typed['num_qo_heads'] if role.startswith('q') else typed['num_kv_heads']
                    extents[role] = 2*((typed['nnz']-1)*typed[role+'_stride_n'] +
                                      (heads-1)*typed[role+'_stride_h'] + typed['rotary_dim'])
                output.update(position_values=None, position_values_observed=False,
                              extra_binding_requirements=['same-run actual positions contents',
                                                          'exact CTA class/mask/control transfer',
                                                          'cos_sin_cache allocation extent'])
        if any(typed[role] > U64 - extent for role, extent in extents.items()):
            return unsupported('POINTER_EXPECTED_EXTENT_OVERFLOW')
        output.update(status='DECODED_HOST_PARAMETERS_ONLY', reason=None, family=name,
                      typed_parameters=typed, typed_fields=fields,
                      pointers={role: typed[role] for role in family['pointer_names']},
                      expected_view_span_bytes=extents,
                      expected_view_span_is_observed_allocation=False,
                      template_candidates=[dict(template_id=t['template_id'], source_phase=t['template_phase'],
                                                template_pin=t['template_pin'], transfer_qualified=False)
                                           for _, t in candidates],
                      preserved_actual_phase=record['phase'])
        return output


def decode_census(census_path, observer_dir=None, decoder=None, artifacts_dir=None):
    decoder = decoder or Decoder()
    census_path = Path(census_path)
    raw = bounded_read(census_path, 128 << 20)
    census = strict_json(raw)
    need(census['schema'] == 'SG_NATIVE_ARGUMENT_CAPTURE_CENSUS_V1' and
         census['status'] == 'PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY', 'successful argument census required')
    controller_path = census_path.parent / 'controller.json'
    controller_raw = bounded_read(controller_path, 16 << 20)
    controller = strict_json(controller_raw)
    need(controller['status'] == 'PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY' and
         controller['argument_census_sha256'] == digest(raw) and
         canonical(controller['phase_counts']) == canonical(census['phase_counts']) and
         canonical(controller['measured_launches']) == canonical(census['measured_launches']),
         'post-run PASS controller must bind this exact argument census')
    process = census['process']
    calls = census['calls']
    need(type(calls) is list and 0 < len(calls) <= 100000 and
         len(calls) == natural(census['measured_launches'], 100000), 'complete bounded census calls')
    directory = Path(observer_dir) if observer_dir else Path(census['observer_directory'])
    need(directory.name == 'process-%d-%d' % (process['pid'], process['start_ticks']) and
         directory.is_dir() and not directory.is_symlink(), 'same-process observer directory')
    need(type(census['journals']) is list and len(census['journals']) == len(JOURNALS) and
         {p['name'] for p in census['journals']} == JOURNALS, 'complete seven-journal inventory')
    need(sum(natural(p['bytes'], 1 << 30) for p in census['journals']) <= 1 << 30,
         'aggregate observer evidence bound')
    evidence = [verify_file(directory / p['name'], p) for p in census['journals']]
    finish_path = directory / 'finish.json'
    finish_raw = bounded_read(finish_path, 1 << 20)
    need(digest(finish_raw) == census['observer_finish_sha256'], 'observer finish pin')
    finish = strict_json(finish_raw)
    need(finish['status'] == 'PASS_NATIVE_ARGUMENT_OBSERVER_CLOSED_NOT_TRACE' and
         finish['pid'] == process['pid'] and finish['start_ticks'] == process['start_ticks'] and
         finish['argument_sideband_closed'] is True and finish['files'] == census['journals'],
         'closed same-process argument observer required')
    evidence.append(file_pin(finish_path, finish_raw))
    artifact_ref = census['artifacts']
    local_default = census_path.parent / 'native/artifacts'
    artifact_root = (Path(artifacts_dir) if artifacts_dir else local_default if local_default.is_dir()
                     else Path(artifact_ref['inventory']['path']).parent)
    need(artifact_root.is_dir() and not artifact_root.is_symlink(), 'native artifact directory')
    inventory_path = artifact_root / 'files.sha256.json'
    evidence.append(verify_file(inventory_path, artifact_ref['inventory'], 1 << 20))
    inventory = strict_json(bounded_read(inventory_path, 1 << 20))
    wanted = {'manifest.json', 'module_calls.json', 'tensor_roots.json'} | {p + '.json' for p in census['phase_counts']}
    need(inventory == artifact_ref['files'] and len(inventory) == len(wanted) and
         {p['path'] for p in inventory} == wanted, 'complete same-run artifact inventory')
    need(sum(natural(p['bytes'], 1 << 30) for p in inventory) <= 1 << 30, 'artifact byte bound')
    evidence.extend(verify_file(artifact_root / p['path'], p) for p in inventory)
    manifest = strict_json(bounded_read(artifact_root / 'manifest.json', 16 << 20))
    need(manifest['status'] == 'COMPLETE' and canonical(manifest['process']) == canonical(process) and
         manifest['input_contract']['sha256'] == census['input_contract_sha256'],
         'native artifact current process/contract')
    journal = directory / 'launch-arguments.jsonl'
    pins = [p for p in census['journals'] if p['name'] == journal.name]
    need(len(pins) == 1, 'one argument journal pin required')
    payload = bounded_read(journal, 256 << 20)
    need(len(payload) == pins[0]['bytes'] and digest(payload) == pins[0]['sha256'], 'argument journal pin mismatch')
    need(payload.endswith(b'\n'), 'complete argument journal newline')
    lines = payload.splitlines()
    need(len(lines) == len(calls), 'exact census/journal count')
    out, last_id, seen, raw_bytes, argument_count = [], 0, set(), 0, 0
    phases, decoded_families, reasons = collections.Counter(), collections.Counter(), collections.Counter()
    for sequence, (line, launch) in enumerate(zip(lines, calls)):
        need(0 < len(line) <= 1 << 20, 'bounded argument row')
        record = strict_json(line)
        ref = launch['argument_record']
        need(canonical(ref) == canonical(dict(file='launch-arguments.jsonl', sequence=sequence, payload_sha256=digest(line))) and
             natural(record['sequence']) == sequence, 'exact argument record linkage')
        key = launch['source_launch_key']
        need(key not in seen, 'duplicate census source key')
        seen.add(key)
        native_id = natural(record['native_launch_binding']['native_launch_id'])
        need(native_id > last_id, 'strict native launch order')
        last_id = native_id
        result = decoder.decode(record, launch, process)
        # This is the on-disk producer payload SHA, not reserialized JSON.
        result['argument_payload_sha256'] = digest(line)
        result['argument_record'] = ref
        raw_bytes += sum(a['size_bytes'] for a in record['arguments'])
        argument_count += len(record['arguments'])
        need(raw_bytes <= 64 << 20, 'aggregate raw argument byte bound')
        phases[record['phase']] += 1
        if result['status'] == 'DECODED_HOST_PARAMETERS_ONLY':
            decoded_families[result['family']] += 1
        else:
            reasons[result['reason']] += 1
        out.append(result)
    need(dict(phases) == census['phase_counts'], 'phase census closure')
    need(raw_bytes == natural(census['argument_raw_bytes']) and
         argument_count == natural(census['argument_count']), 'consumer/raw argument ledger')
    need(bounded_read(census_path, 128 << 20) == raw and
         bounded_read(controller_path, 16 << 20) == controller_raw and
         digest(bounded_read(journal, 256 << 20)) == pins[0]['sha256'], 'input changed during decode')
    for pin in evidence:
        verify_file(Path(pin['path']), pin)
    unsupported = [dict(source_launch_key=x['source_launch_key'], phase=x['phase'],
                        family=x.get('family'), reason=x['reason']) for x in out
                   if x['status'] != 'DECODED_HOST_PARAMETERS_ONLY']
    return dict(schema='NATIVE_TYPED_PARAMETER_DECODE_V1', status='PASS_TYPED_PARAMETER_DECODE_ONLY',
                process=process, input_contract_sha256=census['input_contract_sha256'],
                input_pins=dict(controller=file_pin(controller_path, controller_raw),
                                census=file_pin(census_path, raw), argument_journal=file_pin(journal, payload),
                                catalog=decoder.catalog_pin, decoder=file_pin(Path(__file__))),
                observer_and_artifact_evidence=evidence,
                consumer_validation_status=census['status'],
                measured_launches=len(calls), decoded_calls=sum(decoded_families.values()),
                unsupported_calls=len(unsupported), complete_decode_coverage=not unsupported,
                record_coverage_closed=len(out) == len(calls), raw_argument_bytes=raw_bytes,
                phase_counts=dict(phases), decoded_family_counts=dict(decoded_families),
                unsupported_reason_counts=dict(reasons), unsupported=unsupported, calls=out,
                qualification=dict(host_argument_bytes_only=True, device_memory_dereferenced=False,
                                   object_root_binding_validated=False, dynamic_memory_validated=False,
                                   register_control_transfer_validated=False, native_model_admitted=False,
                                   complete_allocator_coverage=False, hardware_timing_calibrated=False,
                                   unsupported_calls_not_silently_omitted=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--census', type=Path, required=True)
    parser.add_argument('--observer-dir', type=Path)
    parser.add_argument('--artifacts-dir', type=Path)
    parser.add_argument('--catalog', type=Path)
    parser.add_argument('--catalog-sha256')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = decode_census(args.census, args.observer_dir, Decoder(args.catalog, args.catalog_sha256), args.artifacts_dir)
    inputs = list(result['input_pins'].values()) + result['observer_and_artifact_evidence']
    need(args.output.resolve() not in {Path(p['path']) for p in inputs}, 'output must not replace input evidence')
    temporary = args.output.with_name(args.output.name + '.partial')
    need(not temporary.exists() and not args.output.is_symlink(), 'unsafe output path')
    with temporary.open('x') as handle:
        json.dump(result, handle, indent=2, ensure_ascii=False, allow_nan=False)
        handle.write('\n')
    temporary.replace(args.output)
    print(json.dumps({k: result[k] for k in ('status', 'measured_launches', 'decoded_calls',
                                           'unsupported_calls', 'complete_decode_coverage')}))


if __name__ == '__main__':
    main()
