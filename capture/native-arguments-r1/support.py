"""Verify frozen capture dependencies before reusing their Python code."""
import hashlib
import importlib.util
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
TASK = Path('/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi')
R1_SHA = '48d8d0366b965b5df13325861135ad931b7d1fd53a35c55a36d1b87a90d12e00'
R2_SHA = '36098fdf38aa6437af9c555af8db430a0accfbc4a8b752d41c2956002eb452ff'
CAPTURE_SHA = 'fe559d9bca3a03446e801a37c0a544a09694a22cf9b028909c0af3df478d4884'


def need(value, message):
    if not value:
        raise RuntimeError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(8 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def identity(root, expected=None):
    root = Path(root)
    digest = sha(root / 'upload-manifest.json')
    need(expected is None or digest == expected, 'dependency manifest SHA changed')
    manifest = json.loads((root / 'upload-manifest.json').read_text())
    names = {r['path'] for r in manifest['files']}
    need(len(names) == len(manifest['files']), 'duplicate manifest entry')
    need({p.name for p in root.iterdir() if p.is_file()} == names | {'upload-manifest.json'}, 'unlisted source files')
    for row in manifest['files']:
        p = root / row['path']
        need(p.parent == root and p.is_file() and not p.is_symlink(), 'regular flat source file')
        need(p.stat().st_size == row['bytes'] and sha(p) == row['sha256'], 'source bytes changed: ' + str(p))
    return dict(manifest_sha256=digest, files=manifest['files'])


def load_r1():
    r1 = next((p for p in (HERE.parent / 'observer', HERE.parent / 'native-observer')
               if (p / 'run_observer.py').is_file()), None)
    capture = next((p for p in (HERE.parent / 'capture', HERE.parent / 'p1024d32')
                    if (p / 'run_capture.py').is_file()), None)
    need(r1 is not None and capture is not None, 'frozen siblings missing')
    identity(r1, R1_SHA)
    identity(capture, CAPTURE_SHA)
    spec = importlib.util.spec_from_file_location('frozen_observer_r1', r1 / 'run_observer.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
