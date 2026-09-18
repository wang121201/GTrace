"""Synthetic CPU fixtures for 33-epoch closure; never loads CUDA/NVBit."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import run_observer as r


def json_write(path, value):
    path.write_text(json.dumps(value) + '\n')


def fixture(root):
    c = r.workload.contract()
    artifacts = root / 'artifacts'
    artifacts.mkdir()
    process = dict(pid=123, start_ticks=456)
    native_sources = r.read(r.PACKAGE / 'native-source-reference.json')['files']
    json_write(artifacts / 'manifest.json', dict(status='COMPLETE', input_contract=c, native_source_unchanged=True,
        native_source_files=native_sources, process=process, stage_files=[s + '.json' for s in c['phases']],
        coverage=dict(native_scope_abi_enabled=True, kernel_launch_metadata=False)))
    json_write(artifacts / 'module_calls.json', [])
    out = root / 'observer' / 'process-123-456'
    out.mkdir(parents=True)
    instruction = '{"index":0,"offset":0,"opcode":"EXIT","sass":"EXIT;"}'
    code = hashlib.sha256((instruction + '\n').encode()).hexdigest()
    (out / 'static-instructions.jsonl').write_text('{"schema":"sg_nvbit_static_instruction_v1","function_id":1,"instruction":' + instruction + '}\n')
    json_write(out / 'functions.jsonl', dict(function_id=1, instruction_count=1, code_sha256=code, argument_sizes=[8]))
    scopes, launches = [], []
    for i, phase in enumerate(c['phases']):
        epoch = i + 1
        scopes += [dict(type='epoch_begin', epoch_id=epoch, event_ordinal=i * 10, **process),
                   dict(type='epoch_end', epoch_id=epoch, event_ordinal=i * 10 + 5, **process)]
        b = dict(edge='before', launch_id=i, event_ordinal=i * 10 + 1, monotonic_ns=i * 100,
                 epoch_id=epoch, phase=phase, forward_id=i, role='measurement', scope_bound=True,
                 metadata_supported=True, function_id=1, code_sha256=code, argument_sizes=[8],
                 parameter_layout_sha256=hashlib.sha256(b'[8]').hexdigest(), module_scope='<phase-global>',
                 call_id=10000000 + i, layer_id=-1, grid=[1, 1, 1], block=[32, 1, 1],
                 cuda_api='cuLaunchKernel', function_name='fixture', code_sha256_kind='sha256_nvbit_decoded_instruction_rows_v1',
                 context_id=1, stream_u64=0, static_shared_bytes=0, dynamic_shared_bytes=0,
                 registers=1, local_bytes_per_thread=0, launch_attributes=[], **process)
        a = dict(b, edge='return', event_ordinal=i * 10 + 2, monotonic_ns=i * 100 + 1, cuda_status=0)
        launches += [b, a]
    for name, rows in [('scope-journal.jsonl', scopes), ('launch-journal.jsonl', launches)]:
        (out / name).write_text(''.join(json.dumps(x) + '\n' for x in rows))
    for name in ('lifecycle.jsonl', 'allocation-journal.jsonl'):
        (out / name).write_text('')
    finish = dict(status='PASS_METADATA_OBSERVER_CLOSED_NOT_TRACE', function_count=1,
                  epoch_begin_count=33, epoch_end_count=33, launch_before_count=33, launch_return_count=33,
                  errors=[], **process)
    finish.update({name: 0 for name in ('launch_error_count', 'unsupported_dispatch_count', 'graph_node_callback_count',
        'unknown_launch_attribute_count', 'open_context_count', 'active_epoch', 'internal_inspection_dispatch_count')})
    json_write(out / 'finish.json', finish)
    repin(out)
    return c, out


def repin(out):
    finish = r.read(out / 'finish.json')
    finish['files'] = [dict(name=n, bytes=(out / n).stat().st_size, sha256=r.base.sha(out / n)) for n in sorted(r.JOURNALS)]
    json_write(out / 'finish.json', finish)


class ObserverTests(unittest.TestCase):
    def test_all_33_and_exact_new_keys(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            c, _ = fixture(root)
            result = r.validate_native(root, c)
            self.assertEqual(result['measured_launches'], 33)
            self.assertEqual(result['calls'][-1]['source_launch_key'], 'epoch-33-launch-0')
            self.assertFalse(result['qualification']['raw_argument_values_captured'])
            self.assertFalse(result['qualification']['native_model_admitted'])

    def test_hash_epoch_static_and_pair_rejections(self):
        for change in ('filehash', 'epoch', 'static', 'pair', 'abi', 'process'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as d:
                root = Path(d)
                c, out = fixture(root)
                if change in ('filehash', 'static'):
                    p = out / 'static-instructions.jsonl'
                    p.write_text(p.read_text().replace('EXIT;', 'NOP;'))
                elif change == 'epoch':
                    p = out / 'scope-journal.jsonl'
                    rows = list(r.lines(p))[:-1]
                    p.write_text(''.join(json.dumps(x) + '\n' for x in rows))
                else:
                    p = out / 'launch-journal.jsonl'
                    rows = list(r.lines(p))
                    if change == 'pair':
                        rows[-1]['cuda_status'] = 1
                    elif change == 'abi':
                        rows[-2]['argument_sizes'] = rows[-1]['argument_sizes'] = [4]
                    else:
                        rows[-2]['start_ticks'] = 999
                    p.write_text(''.join(json.dumps(x) + '\n' for x in rows))
                if change != 'filehash':
                    repin(out)
                with self.assertRaises((RuntimeError, KeyError)):
                    r.validate_native(root, c)


if __name__ == '__main__':
    unittest.main()
