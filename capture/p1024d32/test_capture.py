"""CPU-only contract, boundary, and counter-format checks. No torch imports."""
import csv
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import workload
from native_ncu_driver import Boundary
from sglang_driver import Observer
from run_capture import parse_ncu_csv, owned_snapshot


class ContractTest(unittest.TestCase):
    def test_default_and_legacy_contract(self):
        c = workload.contract()
        self.assertEqual((len(c['prompt_ids']), len(c['decode_input_ids']), len(c['phases'])), (1024, 32, 33))
        self.assertEqual(c['prompt_ids'][0:2], [1000, 1001])
        self.assertEqual(c['prompt_ids'][-1], 2023)
        self.assertEqual(c['decode_input_ids'][:4], [944, 291, 944, 291])
        self.assertFalse(c['output_feedback'])
        self.assertEqual(workload.expected_controls(c, 32), dict(input_ids=[291], positions=[1055],
                         seq_lens_sum=1056, out_cache_loc=[1056]))
        old = workload.contract(32, 2, 256)
        self.assertEqual(old['decode_input_ids'], [944, 291])
        self.assertEqual(workload.expected_controls(old, 2)['positions'], [33])
        self.assertNotEqual(old['sha256'], c['sha256'])

    def test_no_shared_mutable_contract(self):
        a = workload.contract()
        a['prompt_ids'][0] = 0
        self.assertEqual(workload.contract()['prompt_ids'][0], 1000)

    def test_shape_rejections(self):
        for args in [(0, 32, 1280), (1025, 32, 1280), (1024, 33, 1280), (1024, 32, 1056), (32, 2, 4097)]:
            with self.assertRaises(ValueError):
                workload.contract(*args)

    def test_actual_control_gate(self):
        c = workload.contract()
        for i in (0, 1, 8, 16, 32):
            expected = workload.expected_controls(c, i)
            workload.validate_controls(c, i, expected)
            for name in expected:
                bad = dict(expected)
                bad[name] = None
                with self.assertRaises(ValueError):
                    workload.validate_controls(c, i, bad)


class BoundaryTest(unittest.TestCase):
    def test_native_scope_decode32(self):
        calls = []
        class Native:
            def sg_nvbit_observer_set_scope(self, *args):
                calls.append(args)
                return 1
        observer = Observer.__new__(Observer)
        observer.phases = workload.contract()['phases']
        observer.native = Native()
        observer.stage = 'Decode32'
        observer.active = []
        observer.update_native_scope()
        self.assertEqual(calls[0][:3], (10000032, 32, -1))
        self.assertEqual(calls[0][3], b'Decode32')

    def test_ranges_preserve_whole_workflow(self):
        phases = workload.contract()['phases']
        for scope in ('full', 'Prefill', 'Decode1', 'Decode8', 'Decode16', 'Decode32'):
            calls = []
            current = [None]
            b = Boundary(scope, phases, lambda: calls.append(('start', current[0])),
                         lambda: calls.append(('stop', current[0])))
            for phase in phases:
                current[0] = phase
                b.before(phase)
                b.after(phase)
            b.check()
            expected = [('start', 'Prefill'), ('stop', 'Decode32')] if scope == 'full' else [('start', scope), ('stop', scope)]
            self.assertEqual(calls, expected)

    def test_incomplete_and_unknown_range(self):
        with self.assertRaises(ValueError):
            Boundary('Decode32', workload.contract(32, 2, 256)['phases'], lambda: None, lambda: None)
        b = Boundary('full', workload.contract()['phases'], lambda: None, lambda: None)
        b.before('Prefill')
        with self.assertRaises(ValueError):
            b.check()
        with self.assertRaises(ValueError):
            b.before('Prefill')


class CounterTest(unittest.TestCase):
    def parse(self, rows):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'raw.csv'
            with p.open('w', newline='') as f:
                csv.writer(f, quoting=csv.QUOTE_ALL).writerows(rows)
            return parse_ncu_csv(p)

    def fixture(self):
        return [['ID', 'Process ID', 'Kernel Name', 'dram__bytes_read.sum', 'dram__bytes_write.sum', 'gpu__time_duration.sum'],
                ['', '', '', 'byte', 'byte', 'ns'], ['0', '123', 'range', '320', '64', '1000']]

    def test_exact_integer_bytes_and_same_window(self):
        r = self.parse(self.fixture())
        self.assertEqual(r['metrics']['dram__bytes_read.sum'], 320)
        self.assertEqual(r['metrics']['gpu__time_duration.sum'], '1000')
        self.assertTrue(r['bytes_and_time_same_app_range'])
        self.assertFalse(r['hardware_accuracy_accepted'])

    def test_reject_bad_ranges_units_and_values(self):
        cases = []
        for row, column, bad in [(1, 3, 'Kbyte'), (2, 2, 'kernel'), (2, 3, '0.5'),
                                 (2, 4, '-32'), (2, 5, '0'), (2, 5, 'NaN')]:
            value = self.fixture()
            value[row][column] = bad
            cases.append(value)
        cases.append(self.fixture() + [self.fixture()[2]])
        for rows in cases:
            with self.assertRaises((RuntimeError, ValueError)):
                self.parse(rows)


class OwnershipTest(unittest.TestCase):
    def test_descendants_and_reused_pid(self):
        def row(ppid, pgid, birth):
            return dict(ppid=ppid, pgid=pgid, startticks=birth, state='S', rss_bytes=1)
        root = row(1, 10, 100)
        seen = {10: root}
        rows = {10: root, 11: row(10, 10, 101), 12: row(11, 10, 102), 99: row(1, 99, 900)}
        with patch('run_capture.process_rows', return_value=rows):
            live, _ = owned_snapshot(10, 100, seen)
        self.assertEqual(set(live), {10, 11, 12})
        reused = {10: row(1, 10, 999), 99: rows[99]}
        with patch('run_capture.process_rows', return_value=reused):
            live, _ = owned_snapshot(10, 100, seen)
        self.assertEqual(live, {})


if __name__ == '__main__':
    unittest.main()
