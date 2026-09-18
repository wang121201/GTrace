#!/usr/bin/env python3
"""Local CPU tests and wrapper upload manifest; never runs GPU/SSH."""
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
r = subprocess.run([sys.executable, '-B', '-m', 'unittest', '-v', 'test_observer'], cwd=HERE,
                   env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'), capture_output=True, text=True, timeout=30)
receipt = dict(status='PASS_CPU_NATIVE_OBSERVER_CLOSURE' if r.returncode == 0 else 'FAIL', returncode=r.returncode,
               stdout=r.stdout, stderr=r.stderr, GPU_executed=False, remote_executed=False)
(HERE / 'cpu-preflight.json').write_text(json.dumps(receipt, indent=2) + '\n')
if r.returncode:
    print(json.dumps(receipt, indent=2))
    raise SystemExit(r.returncode)
files = [dict(path=p.name, bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
         for p in sorted(HERE.iterdir()) if p.is_file() and p.name != 'upload-manifest.json']
(HERE / 'upload-manifest.json').write_text(json.dumps(dict(schema='SGLANG_PD_OBSERVER_UPLOAD_V1',
    remote_directory='/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi/observer', files=files), indent=2) + '\n')
print(json.dumps(dict(status=receipt['status'], files=len(files),
    manifest_sha256=hashlib.sha256((HERE / 'upload-manifest.json').read_bytes()).hexdigest())))
