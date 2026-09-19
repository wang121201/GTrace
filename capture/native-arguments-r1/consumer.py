"""Independent host-argument consumer; never admits a dynamic TileGen model.

validate_metadata is adapted from the pinned frozen native-observer validator.
The frozen package is verified by support.load_r1 before source-contract reuse.
"""
import collections
import hashlib
import json
import re
from pathlib import Path
from support import load_r1, need, sha as sha_file
U64 = (1 << 64) - 1
JOURNALS = {'launch-journal.jsonl', 'scope-journal.jsonl', 'functions.jsonl',
            'static-instructions.jsonl', 'lifecycle.jsonl', 'allocation-journal.jsonl'}


def integer(value, low=0, high=U64):
    need(type(value) is int and low <= value <= high, 'exact bounded integer')
    return value


def parse_json(raw):
    def pairs(items):
        out = {}
        for key, value in items:
            need(key not in out, 'duplicate JSON key: ' + key)
            out[key] = value
        return out
    def invalid(value):
        raise ValueError('nonfinite JSON value: ' + value)
    return json.loads(raw, object_pairs_hook=pairs, parse_constant=invalid)


def read(path, cap=64 << 20):
    path = Path(path)
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= cap, 'bounded regular JSON: ' + str(path))
    return parse_json(path.read_bytes())


def lines(path, row_cap=1 << 20):
    path = Path(path)
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= 1 << 30, 'bounded regular journal')
    with path.open('rb') as handle:
        for raw in handle:
            need(raw.endswith(b'\n') and 1 < len(raw) <= row_cap, 'bounded newline-closed journal row')
            yield parse_json(raw)


def pin(path):
    path = Path(path)
    return dict(path=str(path.resolve()), bytes=path.stat().st_size, sha256=sha_file(path))


def artifact_pins(run, c):
    root = Path(run) / 'artifacts'
    inventory = read(root / 'files.sha256.json', 1 << 20)
    wanted = {'manifest.json', 'module_calls.json', 'tensor_roots.json'} | {p + '.json' for p in c['phases']}
    need(type(inventory) is list and len(inventory) == len(wanted) and {r['path'] for r in inventory} == wanted, 'exact native artifact inventory')
    total = 0
    for row in inventory:
        p = root / row['path']
        need(p.parent == root and p.is_file() and not p.is_symlink(), 'regular flat native artifact')
        total += integer(row['bytes'], 1, 1 << 30)
        need(p.stat().st_size == row['bytes'] and sha_file(p) == row['sha256'], 'native artifact SHA/bytes')
    need(total <= 1 << 30, 'native artifact aggregate bound')
    return {'inventory': pin(root / 'files.sha256.json'), 'files': inventory}


def enriched_calls(census, before, modules):
    """Canonical ordinals rebind opaque IDs while retaining their alias topology."""
    counters = collections.Counter()
    identities = {k: {} for k in ('function_id', 'call_id', 'context_id', 'stream_u64')}
    result = []
    aliases = 0
    for original in census['calls']:
        row = dict(original)
        b = before[row['native_launch_id']]
        need(type(row['stream_u64']) is int and row['stream_u64'] == 0 and integer(row['context_id'], 1) > 0, 'single default stream semantics')
        integer(b['forward_id'], 0, 63)
        row.update(forward_id=b['forward_id'], role=b['role'])
        mk = (row['epoch_id'], row['call_id'])
        row['module_kernel_ordinal'] = counters[mk]
        counters[mk] += 1
        for field, mapping in identities.items():
            value = integer(row[field], 1 if field in ('function_id', 'context_id') else 0)
            if value not in mapping:
                mapping[value] = len(mapping)
            row[field + '_binding'] = mapping[value]
        module = modules.get(row['call_id'])
        if module is None:
            need(row['module_scope'] == '<phase-global>' and row['call_id'] == 10000000 + row['forward_id'] and row['layer_id'] == -1, 'exact phase-global binding')
            dynamic = '<phase-global>'
            ancestry = []
        else:
            ancestors = []
            current = module
            while current is not None:
                ancestors.append({'module': current['module'], 'module_class': current['module_class']})
                current = modules.get(current['parent_call_id'])
            ancestry = list(reversed(ancestors))
            layer = row['layer_id']
            dynamic = re.sub(r'model\.layers\.\d+(?=\.|$)', 'model.layers.%d' % layer, row['module_scope'], count=1) if layer >= 0 else row['module_scope']
            aliases += dynamic != row['module_scope']
        row.update(dynamic_module=dynamic, module_ancestry=ancestry)
        result.append(row)
    return result, aliases


def validate_metadata(run, c, observer_parent=None, argument_plan=None):
    """Validate exact journals and derive new ordinals, with no old shape census."""
    frozen = load_r1()
    workload = frozen.workload
    run = Path(run)
    manifest = read(run / 'artifacts/manifest.json')
    need(manifest['status'] == 'COMPLETE' and manifest['input_contract'] == c and
         manifest['native_source_unchanged'] is True and manifest['coverage']['native_scope_abi_enabled'] is True and
         manifest['coverage']['kernel_launch_metadata'] is False, 'native host/observer source closure')
    workload.check_native_sources(manifest['native_source_files'])
    need(manifest['driver_sha256'] == sha_file(frozen.PACKAGE / 'sglang_driver.py'), 'frozen native metadata driver SHA')
    need(manifest['stage_files'] == [s + '.json' for s in c['phases']], 'complete native stages')
    process = manifest['process']
    need(set(process) == {'pid', 'start_ticks'}, 'exact process identity fields')
    integer(process['pid'], 1); integer(process['start_ticks'], 1)
    roots = list(((Path(observer_parent) if observer_parent is not None else run) / 'observer').glob('process-*'))
    need(len(roots) == 1 and roots[0].name == 'process-%d-%d' % (process['pid'], process['start_ticks']), 'single matching observer process')
    root = roots[0]
    finish = read(root / 'finish.json')
    need(finish['status'] == ('PASS_NATIVE_ARGUMENT_OBSERVER_CLOSED_NOT_TRACE' if argument_plan else 'PASS_METADATA_OBSERVER_CLOSED_NOT_TRACE') and
         finish['pid'] == process['pid'] and finish['start_ticks'] == process['start_ticks'], 'observer finish/process closure')
    expected_files = JOURNALS | ({'launch-arguments.jsonl'} if argument_plan else set())
    need({r['name'] for r in finish['files']} == expected_files and len(finish['files']) == len(expected_files), 'exact closed journal inventory')
    need(type(finish['metadata_bytes_before_finish']) is int and finish['metadata_bytes_before_finish'] == sum(r['bytes'] for r in finish['files']) <= (1 << 30) - (128 << 10), 'metadata byte budget/ledger')
    for row in finish['files']:
        p = root / row['name']
        need(not p.is_symlink() and p.stat().st_size == row['bytes'] and sha_file(p) == row['sha256'], 'journal SHA/length: ' + row['name'])
    for field in ('launch_error_count', 'unsupported_dispatch_count', 'graph_node_callback_count',
                  'unknown_launch_attribute_count', 'open_context_count', 'active_epoch', 'internal_inspection_dispatch_count'):
        need(finish[field] == 0, 'observer error/capability: ' + field)
    need(not finish['errors'] and finish['epoch_begin_count'] == finish['epoch_end_count'] == len(c['phases']), 'epoch total/error closure')
    need(integer(finish['launch_before_count'], 1, 1000000) == integer(finish['launch_return_count'], 1, 1000000), 'finish launch closure')
    functions = {}
    for row in lines(root / 'functions.jsonl'):
        fid = integer(row['function_id'], 1)
        need(fid not in functions, 'duplicate function ID')
        functions[fid] = row
    need(len(functions) == finish['function_count'] > 0, 'actual function census')
    static_hashes, static_counts = {}, collections.Counter()
    with (root / 'static-instructions.jsonl').open('rb') as f:
        for raw in f:
            need(raw.endswith(b'\n') and len(raw) <= 1 << 20, 'static row boundary')
            row = parse_json(raw)
            fid = row['function_id']
            need(fid in functions and row['schema'] == 'sg_nvbit_static_instruction_v1', 'static function identity')
            # Hash original bytes, preserving the observer's exact field order
            # and its byte-wise JSON escapes; no decode/re-encode substitution.
            canonical = raw.rstrip(b'\n').split(b'"instruction":', 1)[1][:-1] + b'\n'
            static_hashes.setdefault(fid, hashlib.sha256()).update(canonical)
            static_counts[fid] += 1
    for fid, f in functions.items():
        need(static_counts[fid] == f['instruction_count'] and fid in static_hashes and
             static_hashes[fid].hexdigest() == f['code_sha256'], 'static instruction census/SHA')
    epochs = {}
    for row in lines(root / 'scope-journal.jsonl'):
        need(row['pid'] == process['pid'] and row['start_ticks'] == process['start_ticks'], 'scope process identity')
        if row['type'] in ('epoch_begin', 'epoch_end'):
            e = epochs.setdefault(row['epoch_id'], {})
            need(row['type'] not in e, 'duplicate epoch boundary')
            e[row['type']] = row['event_ordinal']
    need(set(epochs) == set(range(1, len(c['phases']) + 1)), 'exact contract epoch IDs')
    previous_end = -1
    for epoch in range(1, len(c['phases']) + 1):
        e = epochs[epoch]
        need(set(e) == {'epoch_begin', 'epoch_end'} and previous_end < e['epoch_begin'] < e['epoch_end'], 'ordered nonoverlapping complete epochs')
        previous_end = e['epoch_end']
    before, after = {}, {}
    journal_event = -1
    for row in lines(root / 'launch-journal.jsonl'):
        need(row['pid'] == process['pid'] and row['start_ticks'] == process['start_ticks'], 'launch process identity')
        integer(row['launch_id'], 0, 1000000); integer(row['event_ordinal'], 0, U64)
        need(row['event_ordinal'] > journal_event, 'physical launch journal event order'); journal_event = row['event_ordinal']
        need(row['edge'] in ('before', 'return'), 'launch edge')
        dest = before if row['edge'] == 'before' else after
        need(row['launch_id'] not in dest, 'duplicate launch edge')
        dest[row['launch_id']] = row
    need(set(before) == set(after) == set(range(finish['launch_before_count'])), 'complete launch ordinal/pair census')
    ignored = {'edge', 'event_ordinal', 'monotonic_ns', 'cuda_status'}
    module_rows = read(run / 'artifacts/module_calls.json')
    need(len(module_rows) <= 1000000, 'bounded module census')
    modules = {m['call_id']: m for m in module_rows}
    need(len(modules) == len(module_rows), 'duplicate module call ID')
    for m in module_rows:
        integer(m['call_id'], 0, U64)
        parent = m['parent_call_id']
        need(parent is None or type(parent) is int and parent in modules and parent < m['call_id'] and modules[parent]['phase'] == m['phase'], 'same-phase acyclic module parent')
    phase_counts = collections.Counter()
    calls = []
    previous_event = -1
    for lid in sorted(before):
        b, a = before[lid], after[lid]
        need(previous_event < b['event_ordinal'] < a['event_ordinal'] and integer(a['cuda_status']) == 0, 'launch order/return status')
        previous_event = b['event_ordinal']
        need({k: v for k, v in b.items() if k not in ignored} == {k: v for k, v in a.items() if k not in ignored}, 'launch before/return identity')
        epoch = b['epoch_id']
        enclosed = [e for e, bounds in epochs.items() if bounds['epoch_begin'] < b['event_ordinal'] < a['event_ordinal'] < bounds['epoch_end']]
        need(enclosed == ([epoch] if epoch else []), 'native launch missing/wrong epoch mark')
        if not epoch:
            need(not b.get('parameter_values_captured') and not b.get('native_argument_record'), 'unmeasured launch argument capture')
            continue
        need(epoch in epochs, 'launch outside known epochs')
        e, phase = epochs[epoch], c['phases'][epoch - 1]
        need(e['epoch_begin'] < b['event_ordinal'] < a['event_ordinal'] < e['epoch_end'], 'launch epoch enclosure')
        need(b['phase'] == phase and b['forward_id'] == epoch - 1 and b['role'] == 'measurement' and b['scope_bound'], 'native phase/scope attribution')
        need(b['metadata_supported'] and b['function_id'] in functions, 'native function unsupported')
        f = functions[b['function_id']]
        need(b['code_sha256'] == f['code_sha256'] and b['argument_sizes'] == f['argument_sizes'], 'launch/static function ABI identity')
        need(b['parameter_layout_sha256'] == hashlib.sha256(json.dumps(b['argument_sizes'], separators=(',', ':')).encode()).hexdigest(), 'ABI size layout SHA')
        if b['module_scope'] == '<phase-global>':
            need(b['call_id'] == 10000000 + epoch - 1 and b['layer_id'] == -1, 'phase-global owner')
        else:
            module = modules.get(b['call_id'])
            need(module and module['module'] == b['module_scope'] and module['phase'] == phase, 'same-process module call owner')
            owner, layer, visited = module, -1, set()
            while owner is not None:
                need(owner['call_id'] not in visited, 'cyclic module ancestry')
                visited.add(owner['call_id'])
                if owner['module_class'].endswith('.LlamaDecoderLayer'):
                    layer = int(owner['module'].split('.layers.')[1].split('.')[0])
                    break
                owner = modules.get(owner.get('parent_call_id'))
            need(b['layer_id'] == layer, 'actual decoder ancestry')
        need(all(type(n) is int and n > 0 for k in ('grid', 'block') for n in b[k]) and
             len(b['grid']) == len(b['block']) == 3, 'actual launch geometry')
        ordinal = phase_counts[phase]
        phase_counts[phase] += 1
        fields = ('cuda_api', 'function_id', 'function_name', 'code_sha256', 'code_sha256_kind',
                  'context_id', 'stream_u64', 'module_scope', 'call_id', 'layer_id', 'grid', 'block',
                  'static_shared_bytes', 'dynamic_shared_bytes', 'registers', 'local_bytes_per_thread',
                  'launch_attributes', 'argument_sizes', 'parameter_layout_sha256')
        calls.append(dict(source_launch_key='epoch-%d-launch-%d' % (epoch, ordinal), epoch_id=epoch,
                          epoch_launch_ordinal=ordinal, native_launch_id=lid, phase=phase,
                          **{k: b[k] for k in fields}))
    need(set(phase_counts) == set(c['phases']) and all(phase_counts[p] > 0 for p in c['phases']), 'all phases have observed native launches')
    census = dict(schema='SGLANG_PD_NATIVE_METADATA_CENSUS_V1', status='PASS_NATIVE_METADATA_CENSUS_ONLY',
                process=process, input_contract_sha256=c['sha256'], phase_counts=dict(phase_counts),
                measured_launches=len(calls), total_launches=len(before), inspected_functions=len(functions),
                unique_decoded_code_hashes=len({f['code_sha256'] for f in functions.values()}), calls=calls,
                observer_finish_sha256=sha_file(root / 'finish.json'), journals=finish['files'],
                qualification=dict(decoded_static_SASS_hashes_verified=True, argument_size_layout_only=True,
                    raw_argument_values_captured=False, typed_pointer_binding=False, dynamic_memory_addresses=False,
                    dynamic_program_execution=False, cubin_hash=False, complete_allocator_coverage=False,
                    native_model_admitted=False, exhaustive_driver_callback_coverage_proven=False,
                    same_process_CUPTI_crosscheck=False))

    return census, root, finish, before, after, modules

RECORD_FIELDS = {'schema','sequence','native_launch_binding','source_launch_key','epoch_id',
    'epoch_launch_ordinal','forward_id','phase','cuda_api','layer','module_scope','module_call_id',
    'module_kernel_ordinal','context_id','function_id','stream_u64','code_sha256','code_sha256_kind',
    'parameter_layout_sha256','grid','block','dynamic_shared_bytes','static_shared_bytes',
    'registers','local_bytes_per_thread','launch_attributes','argument_transport',
    'capture_before_original_launch','device_memory_dereferenced','arguments'}


def validate_arguments(run, c, observer_parent=None, plan_path=None):
    from make_argument_plan import REFERENCE_ONLY, validate_plan
    plan_path = Path(plan_path) if plan_path else Path(__file__).parent / 'argument-plan.json'
    plan = validate_plan(read(plan_path));plan_sha = sha_file(plan_path)
    need(plan['input_contract_sha256'] == c['sha256'] and plan['phases'] == c['phases'], 'plan/current input contract')
    evidence = artifact_pins(run, c)
    census,root,finish,before,after,modules = validate_metadata(run,c,observer_parent,plan)
    limits=plan['limits']
    for field, key in [('argument_sideband_entries','launches'),('argument_sideband_returns','launches'),
                       ('argument_sideband_argument_count','arguments'),('argument_sideband_raw_bytes','raw_bytes')]:
        need(type(finish[field]) is int and finish[field] == limits[key], 'exact closed argument producer counter: '+field)
    need(finish['argument_sideband_closed'] is True and finish['argument_sideband_plan_sha256'] == plan_sha, 'argument plan/producer closure')
    for field in ['dynamic_instrumentation','memory_addresses_captured','actual_sm_placement_captured','device_synchronization_inserted','gpu_memory_allocated_by_observer','template_or_gtsim_admission']:
        need(finish[field] is False, 'host-only scope: '+field)
    need(finish['kernel_argument_values_captured'] is True, 'actual host values required')
    for name in ['lifecycle.jsonl','allocation-journal.jsonl']:
        for _ in lines(root/name):pass
    actual,aliases = enriched_calls(census,before,modules)
    need(len(actual)==limits['launches'] and aliases==plan['shared_module_alias_launches'], 'argument/native census and aliases')
    need(census['phase_counts']==plan['phase_counts'], 'planned phase population')
    for got, expected in zip(actual, plan['launches']):
        need(set(got)==set(expected) and {k:v for k,v in got.items() if k not in REFERENCE_ONLY} ==
             {k:v for k,v in expected.items() if k not in REFERENCE_ONLY}, 'fresh launch/code/ABI/scope/resources/topology differs from plan')
    argpath=root/'launch-arguments.jsonl'
    need(argpath.stat().st_size <= limits['max_file_bytes'], 'argument serialized file cap')
    process=census['process'];argument_count=total_bytes=sequence=maximum_row=0
    with argpath.open('rb') as handle:
        for raw in handle:
            need(raw.endswith(b'\n') and 1<len(raw)<=limits['max_row_bytes'], 'argument serialized newline/row cap')
            maximum_row=max(maximum_row,len(raw))
            need(sequence<len(actual), 'extra argument row')
            row=parse_json(raw);call=actual[sequence];p=plan['launches'][sequence]
            need(set(row)==RECORD_FIELDS and row['schema']=='SG_NATIVE_ARGUMENT_VECTOR_V1', 'exact argument schema/fields')
            need(integer(row['sequence'])==sequence and row['source_launch_key']==call['source_launch_key'], 'argument sequence/source key')
            need(row['native_launch_binding']==dict(process=process,native_launch_id=call['native_launch_id'],source_launch_key=call['source_launch_key']), 'actual current-process native binding')
            native=before[call['native_launch_id']];returned=after[call['native_launch_id']]
            ref=dict(file='launch-arguments.jsonl',sequence=sequence,payload_sha256=hashlib.sha256(raw[:-1]).hexdigest())
            need(native.get('native_argument_record')==returned.get('native_argument_record')==ref and
                 native.get('parameter_values_captured') is True and returned.get('parameter_values_captured') is True, 'native before/return argument reference SHA')
            for field in ['phase','cuda_api','module_scope','code_sha256','code_sha256_kind','parameter_layout_sha256','grid','block','dynamic_shared_bytes','static_shared_bytes','registers','local_bytes_per_thread','launch_attributes']:
                need(row[field]==native[field]==p[field], 'argument/native/planned '+field)
            for field,other in [('epoch_id','epoch_id'),('epoch_launch_ordinal','epoch_launch_ordinal'),('forward_id','forward_id'),('module_kernel_ordinal','module_kernel_ordinal'),('module_call_id','call_id'),('context_id','context_id'),('function_id','function_id'),('stream_u64','stream_u64'),('layer','layer_id')]:
                integer(row[field],-1 if field=='layer' else 0)
                need(row[field]==call[other], 'argument/native '+field)
            need(row['argument_transport']=='kernelParams' and row['capture_before_original_launch'] is True and row['device_memory_dereferenced'] is False, 'host-only separate parameter buffers')
            args=row['arguments'];need(type(args) is list and len(args)==len(p['argument_sizes']), 'argument vector length')
            for index,(argument,size) in enumerate(zip(args,p['argument_sizes'])):
                need(set(argument)=={'index','size_bytes','parameter_buffer_offset','raw_bytes_hex','sha256'}, 'argument field set')
                need(integer(argument['index'])==index and integer(argument['size_bytes'],1)==size and argument['parameter_buffer_offset'] is None, 'argument index/width/unknown packed offset')
                value=argument['raw_bytes_hex'];need(type(value) is str and len(value)==2*size and all(c in '0123456789abcdef' for c in value), 'argument raw hex encoding/size')
                data=bytes.fromhex(value);need(hashlib.sha256(data).hexdigest()==argument['sha256'], 'actual argument bytes SHA')
                argument_count+=1;total_bytes+=len(data)
            call['argument_record']=ref;sequence+=1
    need(sequence==limits['launches'] and argument_count==limits['arguments'] and total_bytes==limits['raw_bytes'], 'independent raw argument closure')
    census.update(schema='SG_NATIVE_ARGUMENT_CAPTURE_CENSUS_V1',status='PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY',
        calls=actual,argument_count=argument_count,argument_raw_bytes=total_bytes,argument_plan_sha256=plan_sha,
        argument_plan_bytes=plan_path.stat().st_size,argument_serialized_bytes=argpath.stat().st_size,
        maximum_argument_row_bytes=maximum_row,observer_directory=str(root.resolve()),journals=finish['files'],
        shared_module_alias_launches=aliases,artifacts=evidence,
        qualification=dict(decoded_static_SASS_hashes_verified=True,argument_size_layout_only=False,
            raw_argument_values_captured=True,device_memory_dereferenced=False,capture_before_original_launch=True,
            typed_objects_or_relocation_qualified=False,typed_pointer_binding=False,dynamic_memory_addresses=False,
            dynamic_program_execution=False,actual_SM_placement=False,cubin_hash=False,complete_allocator_coverage=False,
            native_model_admitted=False,exhaustive_driver_callback_coverage_proven=False,same_process_CUPTI_crosscheck=False))
    return census
