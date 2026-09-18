"""CPU checks of exact patch scope and frozen dependency identities."""
import hashlib
import json
import unittest

from support import HERE, load_r1


class CapacityTests(unittest.TestCase):
    def test_only_two_source_literals_changed(self):
        raw = (HERE / 'observer.cu').read_bytes()
        self.assertEqual(raw.count(b'const uint64_t DEFAULT_CAP = 1ull << 30;'), 1)
        self.assertEqual(raw.count(b'metadata quota outside 2..1024 MiB envelope'), 1)
        restored = raw.replace(b'const uint64_t DEFAULT_CAP = 1ull << 30;', b'const uint64_t DEFAULT_CAP = 256ull << 20;')
        restored = restored.replace(b'metadata quota outside 2..1024 MiB envelope', b'metadata quota outside 2..256 MiB envelope')
        self.assertEqual(hashlib.sha256(restored).hexdigest(), '7ebb59827281aea8651921b476af5f54bea17772e5aeac72da13a1d27160f2f7')
        self.assertEqual(hashlib.sha256((HERE / 'build.py').read_bytes()).hexdigest(), 'f4c290b9ab34741a30c69131cb42810861217583d6f7c4e3dcdfb98076048b54')
        manifest = json.loads((HERE / 'manifest.json').read_text())
        self.assertEqual(manifest['max_metadata_bytes'], 1 << 30)
        self.assertEqual(manifest['build_inputs']['observer.cu']['sha256'], hashlib.sha256(raw).hexdigest())

    def test_exact_frozen_validator_and_workload(self):
        r1 = load_r1()
        self.assertEqual(r1.base.verify_package()['manifest_sha256'], 'fe559d9bca3a03446e801a37c0a544a09694a22cf9b028909c0af3df478d4884')
        self.assertEqual(len(r1.workload.contract()['phases']), 33)
        self.assertTrue(callable(r1.validate_native))


if __name__ == '__main__':
    unittest.main()
