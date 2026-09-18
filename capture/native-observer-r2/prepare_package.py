#!/usr/bin/env python3
"""CPU source checks and upload manifest. No compiler, SSH or GPU execution."""
import ast
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
for p in HERE.glob('*.py'):
    ast.parse(p.read_text(), filename=str(p))
env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
steps = []
for directory, name in [(HERE, 'test_capacity'), (HERE.parent / 'native-observer', 'test_observer')]:
    r = subprocess.run([sys.executable, '-B', '-m', 'unittest', '-v', name], cwd=directory,
                       env=env, capture_output=True, text=True, timeout=30)
    steps.append(dict(test=name, returncode=r.returncode, stdout=r.stdout, stderr=r.stderr))
receipt = dict(status='PASS_CPU_ONLY_CAPACITY_REVISION' if all(x['returncode'] == 0 for x in steps) else 'FAIL',
               GPU_executed=False, NVCC_executed=False, remote_executed=False, steps=steps)
(HERE / 'cpu-preflight.json').write_text(json.dumps(receipt, indent=2) + '\n')
if receipt['status'] == 'FAIL':
    print(json.dumps(receipt, indent=2))
    raise SystemExit(1)
files = [dict(path=p.name, bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
         for p in sorted(HERE.iterdir()) if p.is_file() and p.name != 'upload-manifest.json']
(HERE / 'upload-manifest.json').write_text(json.dumps(dict(schema='SGLANG_PD_OBSERVER_R2_UPLOAD_V1',
    remote_directory='/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi/observer-r2', files=files), indent=2) + '\n')
print(json.dumps(dict(status=receipt['status'], files=len(files),
    manifest_sha256=hashlib.sha256((HERE / 'upload-manifest.json').read_bytes()).hexdigest())))
