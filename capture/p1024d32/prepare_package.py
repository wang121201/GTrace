#!/usr/bin/env python3
"""Local CPU tests and upload manifest only; never launches a GPU/remote command."""
import ast
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def main():
    for p in HERE.glob('*.py'):
        ast.parse(p.read_text(), filename=str(p))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    run = subprocess.run([sys.executable, '-B', '-m', 'unittest', '-v', 'test_capture'],
                         cwd=HERE, env=env, capture_output=True, text=True, timeout=30)
    receipt = dict(schema='SGLANG_PD_LOCAL_CPU_PREFLIGHT_V1', status='PASS_LOCAL_CPU_ONLY' if run.returncode == 0 else 'FAIL',
                   returncode=run.returncode, stdout=run.stdout, stderr=run.stderr,
                   python=sys.version, GPU_executed=False, remote_executed=False,
                   source_AST_checked=True, packages_imported_on_GPU_host_not_locally=True)
    (HERE / 'cpu-preflight.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if run.returncode:
        print(json.dumps(receipt, indent=2))
        return run.returncode
    files = []
    for p in sorted(HERE.iterdir()):
        if p.is_file() and p.name != 'upload-manifest.json':
            files.append(dict(path=p.name, bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
    manifest = dict(schema='SGLANG_PD_CAPTURE_UPLOAD_V1', status='LOCAL_CPU_READY_NO_GPU_EXECUTION',
                    remote_directory='/home/xmu/nvidiagds/codex-runs/tilegen-stage-p1024d32-20260918-zhi/capture',
                    immutable_source_files=True, files=files)
    (HERE / 'upload-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(dict(status=receipt['status'], files=len(files),
                         manifest_sha256=hashlib.sha256((HERE / 'upload-manifest.json').read_bytes()).hexdigest())))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
