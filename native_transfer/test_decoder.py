#!/usr/bin/env python3
"""CPU-only real-capture regression and evidence-rejection tests."""
import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from decode import (Decoder, InvalidCapture, RESOURCE_FIELDS, JOURNALS, canonical, decode_census,
                    digest, strict_json, file_pin)

HERE = Path(__file__).resolve().parent
FIXTURE_SHA256 = '867e05bd340335d33bd937d611e543ae5b2fd95fe01bf6a831b731fc7414d0cf'


class DecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        raw = (HERE / 'regression_inputs.json').read_bytes()
        if digest(raw) != FIXTURE_SHA256:
            raise ValueError('regression fixture SHA mismatch')
        cls.fixture = strict_json(raw)
        cls.decoder = Decoder()

    def example(self, family='GEMV', phase=None):
        return copy.deepcopy(next(e for e in self.fixture['examples'] if e['family'] == family and
                                  (phase is None or e['raw_record']['phase'] == phase)))

    def decode(self, example):
        return self.decoder.decode(example['raw_record'], example['launch'], example['process'], True)

    def rewrite(self, example, arg_index, raw):
        row = example['raw_record']['arguments'][arg_index]
        row['raw_bytes_hex'], row['sha256'] = raw.hex(), digest(raw)

    def test_real_captured_independent_typed_oracle(self):
        self.assertEqual(len(self.fixture['examples']), 23)
        for example in self.fixture['examples']:
            with self.subTest(family=example['family'], key=example['launch']['source_launch_key']):
                out = self.decode(example)
                self.assertEqual(out['status'], 'DECODED_HOST_PARAMETERS_ONLY')
                self.assertEqual(out['typed_parameters'], example['expected_parameters'])
                self.assertEqual(out['family'], example['family'])
                self.assertFalse(out['native_model_admitted'])
                self.assertTrue(out['template_candidates'])
                if example['expected_word_vector']:
                    self.assertEqual(out['argument_words'], example['expected_word_vector'])

    def test_norm_same_abi_different_code_different_roles(self):
        plain, fused = self.example('PlainNorm'), self.example('FusedNorm')
        self.assertEqual(plain['launch']['parameter_layout_sha256'], fused['launch']['parameter_layout_sha256'])
        self.assertNotEqual(plain['launch']['code_sha256'], fused['launch']['code_sha256'])
        self.assertIn('output', self.decode(plain)['typed_parameters'])
        self.assertIn('residual', self.decode(fused)['typed_parameters'])

    def test_bad_sha_transport_boundaries_and_process_rejected(self):
        for mutation in ('sha', 'transport', 'index', 'offset', 'process', 'duplicate_key'):
            e = self.example()
            r = e['raw_record']
            if mutation == 'sha': r['arguments'][0]['sha256'] = '0'*64
            if mutation == 'transport': r['argument_transport'] = 'extra'
            if mutation == 'index': r['arguments'][0]['index'] = 1
            if mutation == 'offset': r['arguments'][0]['parameter_buffer_offset'] = 0
            if mutation == 'process': r['native_launch_binding']['process']['pid'] += 1
            with self.subTest(mutation=mutation), self.assertRaises(InvalidCapture):
                if mutation == 'duplicate_key': strict_json('{"a":1,"a":2}')
                else: self.decode(e)

    def test_native_identity_and_geometry_join_rejected(self):
        for field in ('native_launch_id', 'context_id', 'function_id', 'stream_u64', 'phase'):
            e = self.example()
            e['launch'][field] = 'Decode32' if field == 'phase' else e['launch'][field] + 1
            with self.subTest(field=field), self.assertRaises(InvalidCapture):
                self.decode(e)

    def test_gemv_all_120_nonpointer_bytes_are_checked(self):
        for word in set(range(38)) - {0, 1, 4, 5, 8, 9, 12, 13}:
            e = self.example()
            raw = bytearray.fromhex(e['raw_record']['arguments'][0]['raw_bytes_hex'])
            raw[word*4] ^= 1
            self.rewrite(e, 0, bytes(raw))
            with self.subTest(word=word):
                self.assertEqual(self.decode(e)['reason'], 'GEMV_120_NONPOINTER_BYTES_HAVE_NO_SEALED_TEMPLATE')

    def test_gemv_alias_and_alignment(self):
        e = self.example()
        raw = bytearray.fromhex(e['raw_record']['arguments'][0]['raw_bytes_hex'])
        raw[48] ^= 16
        self.rewrite(e, 0, raw)
        self.assertEqual(self.decode(e)['reason'], 'GEMV_EPILOGUE_OUTPUT_ALIAS_MISMATCH')
        e = self.example()
        raw = bytearray.fromhex(e['raw_record']['arguments'][0]['raw_bytes_hex'])
        raw[0] ^= 1
        self.rewrite(e, 0, raw)
        self.assertEqual(self.decode(e)['reason'], 'GEMV_POINTER_ALIGNMENT_DOMAIN')

    def test_unknown_code_and_changed_geometry_are_explicit(self):
        e = self.example()
        for row in (e['raw_record'], e['launch']): row['code_sha256'] = '0'*64
        self.assertEqual(self.decode(e)['reason'], 'UNSUPPORTED_CODE_SHA256')
        e = self.example('SiLU')
        e['launch']['grid'] = [1024, 1, 1]
        self.assertEqual(self.decode(e)['reason'], 'UNSUPPORTED_STATIC_GEOMETRY_OR_RESOURCES')

    def test_scalar_and_alias_rejections(self):
        for family, index in [('PlainNorm', 3), ('FusedNorm', 7), ('SiLU', 2), ('Rotary', 6)]:
            e = self.example(family)
            raw = bytearray.fromhex(e['raw_record']['arguments'][index]['raw_bytes_hex'])
            raw[0] ^= 1
            self.rewrite(e, index, raw)
            with self.subTest(family=family):
                self.assertEqual(self.decode(e)['reason'], 'SCALAR_VALUES_HAVE_NO_SEALED_TEMPLATE')
        e = self.example('SiLU')
        self.rewrite(e, 0, bytes.fromhex(e['raw_record']['arguments'][1]['raw_bytes_hex']))
        self.assertEqual(self.decode(e)['reason'], 'SILU_RESTRICT_ALIAS_VIOLATION')

    def test_rotary_new_phase_is_preserved_and_positions_unknown(self):
        e = self.example('Rotary', 'Decode2')
        e['raw_record']['phase'] = e['launch']['phase'] = 'Decode32'
        out = self.decode(e)
        self.assertEqual(out['status'], 'DECODED_HOST_PARAMETERS_ONLY')
        self.assertEqual(out['phase'], 'Decode32')
        self.assertEqual(out['preserved_actual_phase'], 'Decode32')
        self.assertIsNone(out['position_values'])
        self.assertFalse(out['position_values_observed'])
        self.assertTrue(all(not x['transfer_qualified'] for x in out['template_candidates']))

    def write_census(self, root):
        e = self.example('SiLU', 'Decode1')
        r, launch = e['raw_record'], e['launch']
        r.update(schema='SG_NATIVE_ARGUMENT_VECTOR_V1', sequence=0)
        for field in ('grid', 'block') + RESOURCE_FIELDS: r[field] = launch[field]
        directory = root / ('process-%d-%d' % (e['process']['pid'], e['process']['start_ticks']))
        directory.mkdir()
        line = canonical(r)
        payload = line + b'\n'
        (directory / 'launch-arguments.jsonl').write_bytes(payload)
        launch['argument_record'] = dict(file='launch-arguments.jsonl', sequence=0, payload_sha256=digest(line))
        journals = []
        for filename in sorted(JOURNALS):
            p = directory / filename
            if not p.exists(): p.write_bytes(b'')
            journals.append(dict(name=filename, bytes=p.stat().st_size, sha256=digest(p.read_bytes())))
        finish = dict(status='PASS_NATIVE_ARGUMENT_OBSERVER_CLOSED_NOT_TRACE', **e['process'],
                      argument_sideband_closed=True, files=journals)
        (directory / 'finish.json').write_bytes(canonical(finish))
        artifacts = root / 'native/artifacts'
        artifacts.mkdir(parents=True)
        manifest = dict(status='COMPLETE', process=e['process'], input_contract={'sha256': '1'*64})
        artifact_inventory = []
        for filename in ('manifest.json', 'module_calls.json', 'tensor_roots.json', 'Decode1.json'):
            p = artifacts / filename
            p.write_bytes(canonical(manifest if filename == 'manifest.json' else {}))
            artifact_inventory.append(dict(path=filename, bytes=p.stat().st_size, sha256=digest(p.read_bytes())))
        inventory_path = artifacts / 'files.sha256.json'
        inventory_path.write_bytes(canonical(artifact_inventory))
        c = dict(schema='SG_NATIVE_ARGUMENT_CAPTURE_CENSUS_V1', status='PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY', process=e['process'],
                 calls=[launch], measured_launches=1, observer_directory=str(directory),
                 input_contract_sha256='1'*64, phase_counts={'Decode1': 1},
                 observer_finish_sha256=digest(canonical(finish)), journals=journals,
                 artifacts=dict(inventory=file_pin(inventory_path), files=artifact_inventory),
                 argument_count=3, argument_raw_bytes=20)
        path = root / 'census.json'
        path.write_bytes(canonical(c))
        controller = dict(status='PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY',
                          argument_census_sha256=digest(canonical(c)),
                          phase_counts=c['phase_counts'], measured_launches=c['measured_launches'])
        (root / 'controller.json').write_bytes(canonical(controller))
        return path, directory, c

    def test_end_to_end_census_and_tamper(self):
        with tempfile.TemporaryDirectory(dir=HERE) as name:
            path, directory, census = self.write_census(Path(name))
            out = decode_census(path)
            self.assertEqual(out['status'], 'PASS_TYPED_PARAMETER_DECODE_ONLY')
            self.assertEqual(out['decoded_calls'], 1)
            self.assertTrue(out['record_coverage_closed'])
            self.assertFalse(out['qualification']['native_model_admitted'])
            census['calls'][0]['argument_record']['payload_sha256'] = '0'*64
            path.write_bytes(canonical(census))
            with self.assertRaises(InvalidCapture): decode_census(path)

    def test_bad_catalog_pin(self):
        with self.assertRaises(InvalidCapture): Decoder(HERE / 'template_catalog.json', '0'*64)

    def test_valid_raw_bytes_but_wrong_abi_rejected(self):
        e = self.example('SiLU')
        item = e['raw_record']['arguments'][2]
        self.rewrite(e, 2, bytes.fromhex(item['raw_bytes_hex']) + b'\x00'*4)
        item['size_bytes'] = 8
        sizes = [8, 8, 8]
        e['launch']['argument_sizes'] = sizes
        layout = digest(json.dumps(sizes, separators=(',', ':')).encode())
        e['launch']['parameter_layout_sha256'] = e['raw_record']['parameter_layout_sha256'] = layout
        self.assertEqual(self.decode(e)['reason'], 'UNSUPPORTED_ABI_FOR_CODE')

    def test_cli_publishes_only_final_complete_json(self):
        with tempfile.TemporaryDirectory(dir=HERE) as name:
            path, directory, census = self.write_census(Path(name))
            output = Path(name) / 'decoded.json'
            command = [sys.executable, '-B', str(HERE / 'decode.py'), '--census', str(path),
                       '--observer-dir', str(directory), '--output', str(output)]
            proc = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            result = json.loads(output.read_text())
            self.assertEqual(result['status'], 'PASS_TYPED_PARAMETER_DECODE_ONLY')
            self.assertFalse(output.with_name(output.name + '.partial').exists())
            output.unlink()
            census['status'] = 'FAIL'
            path.write_bytes(canonical(census))
            proc = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            self.assertFalse(output.exists())

    def test_unselected_journal_and_artifact_tamper_rejected(self):
        for kind in ('journal', 'artifact'):
            with tempfile.TemporaryDirectory(dir=HERE) as name:
                path, directory, census = self.write_census(Path(name))
                target = directory / 'static-instructions.jsonl' if kind == 'journal' else Path(name) / 'native/artifacts/tensor_roots.json'
                target.write_bytes(b'changed')
                with self.subTest(kind=kind), self.assertRaises(InvalidCapture): decode_census(path)

    def test_missing_failed_or_wrong_controller_rejected(self):
        for mode in ('missing', 'failed', 'wrong_sha'):
            with tempfile.TemporaryDirectory(dir=HERE) as name:
                path, directory, census = self.write_census(Path(name))
                controller_path = Path(name) / 'controller.json'
                controller = json.loads(controller_path.read_text())
                if mode == 'missing': controller_path.unlink()
                else:
                    if mode == 'failed': controller['status'] = 'FAIL'
                    else: controller['argument_census_sha256'] = '0'*64
                    controller_path.write_bytes(canonical(controller))
                with self.subTest(mode=mode), self.assertRaises(InvalidCapture): decode_census(path)


if __name__ == '__main__':
    unittest.main(verbosity=2)
