"""Test age admission without importing or replaying remote native inputs."""
import argparse
import ast
import contextlib
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


path = Path(__file__).resolve().parents[1] / 'executor-r1/whole_stream.py'
tree = ast.parse(path.read_text())
names = {'need', 'parse_dirty_age_accesses', 'dirty_age_expectation',
         'validate_cache_configuration'}
definitions = [node for node in tree.body
               if isinstance(node, ast.FunctionDef) and node.name in names]
# Exercise the real CLI declarations and expectation resolution, stopping before
# filesystem/native-source admission. No duplicate parser contract in this test.
main = next(node for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == 'main')
prefix = []
for node in main.body:
    prefix.append(node)
    if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == 'age_expectation'
            for target in node.targets):
        break
prefix.append(ast.Return(value=ast.Name(id='age_expectation', ctx=ast.Load())))
main.body = prefix
main.name = 'parse_cli'
module = ast.fix_missing_locations(ast.Module(body=definitions + [main], type_ignores=[]))
namespace = dict(argparse=argparse, Path=Path)
exec(compile(module, str(path), 'exec'), namespace)


class AgeAdmission(unittest.TestCase):
    def cli(self, *extra):
        argv = ['whole_stream.py', '--graph', 'g', '--registry', 'r',
                '--runtime', 'rt', '--output', 'out', *extra]
        with patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()):
            return namespace['parse_cli']()

    def validate(self, actual, expected, ef=288):
        namespace['validate_cache_configuration'](
            {'L2': {'EF_hit_numerator': ef, 'dirty_age_accesses': actual}}, expected)

    def test_legacy_default_still_pins_64m(self):
        result = self.cli()
        self.assertEqual(result, dict(expected_dirty_age_accesses=64000000,
                                     expected_dirty_age_source='legacy_default_64000000'))
        self.validate(64000000, result['expected_dirty_age_accesses'])
        with self.assertRaisesRegex(ValueError, 'runner dirty age'):
            self.validate(0, result['expected_dirty_age_accesses'])

    def test_explicit_zero_and_uint64_max_are_admitted(self):
        for value in (0, 64000000, (1 << 64) - 1):
            with self.subTest(value=value):
                result = self.cli('--expected-dirty-age-accesses', str(value))
                self.assertEqual(result['expected_dirty_age_accesses'], value)
                self.assertEqual(result['expected_dirty_age_source'],
                                 'explicit_cli_expected_dirty_age_accesses')
                self.validate(value, value)

    def test_cli_rejects_negative_overflow_or_noninteger(self):
        for value in ('-1', str(1 << 64), '1.5', '1e6', 'nan', '+1', '', ' 1', '１２'):
            with self.subTest(value=value), self.assertRaises(SystemExit) as exc:
                self.cli('--expected-dirty-age-accesses=' + value)
            self.assertEqual(exc.exception.code, 2)

    def test_runner_mismatch_and_wrong_types_fail(self):
        for actual, expected in ((64000000, 0), (0, 64000000),
                                 ('0', 0), (False, 0), (0.0, 0)):
            with self.subTest(actual=actual, expected=expected), self.assertRaisesRegex(ValueError, 'runner dirty age'):
                self.validate(actual, expected)
        for expected in (-1, 1 << 64, False, '0'):
            with self.subTest(expected=expected), self.assertRaisesRegex(ValueError, 'expected dirty age'):
                self.validate(0, expected)

    def test_ef_guard_is_not_relaxed(self):
        for expected in (0, 64000000):
            with self.subTest(expected=expected), self.assertRaisesRegex(ValueError, 'frozen EF h288'):
                self.validate(expected, expected, ef=287)


if __name__ == '__main__':
    unittest.main()
