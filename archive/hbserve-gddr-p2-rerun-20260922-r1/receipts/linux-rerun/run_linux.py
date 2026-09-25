"""Run the rebuilt fixture on the frozen plan inside the same sandbox.

No input bytes are modified: the original plan.json (sha 4c34da34...) is used at
its frozen macOS path, which inside the sandbox resolves to the reconstructed tree.
"""
import hashlib
import json
import os
import subprocess
import sys
import time

R = '/home/xmu/nvidiagds/codex-runs/hbserve-memgen-gtsim-alignment'
B = os.path.join(R, 'build-r1')
MIRROR = os.path.join(B, 'mirror')
MAC = '/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment'
PLAN_MAC = MAC + '/work/ada-cosim-alignment-20260922-r1/gddr-full-p2-r1/plan.json'
PLAN_HOST = os.path.join(MIRROR, PLAN_MAC.replace(MAC + '/', '', 1))
OUT = os.path.join(B, 'out/result.json')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


BW = ['bwrap', '--tmpfs', '/',
      '--dev-bind', '/usr', '/usr', '--dev-bind', '/lib', '/lib',
      '--dev-bind', '/lib64', '/lib64', '--dev-bind', '/bin', '/bin',
      '--dev-bind', '/sbin', '/sbin', '--dev-bind', '/etc', '/etc',
      '--dev-bind', '/home', '/home', '--dev-bind', '/var', '/var',
      '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
      '--dir', '/Users', '--dir', '/Users/wgs', '--dir', '/Users/wgs/Documents',
      '--dir', '/Users/wgs/Documents/Codex', '--dir', '/Users/wgs/Documents/Codex/2026-09-14',
      '--bind', MIRROR, MAC, '--chdir', B]

print('plan sha256 (unchanged, must be 4c34da34...):', sha256(PLAN_HOST))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
cmd = BW + ['./fixture', PLAN_MAC, OUT]
print('run:', ' '.join(cmd[-3:]))
with open(os.path.join(B, 'run-r1.stdout'), 'wb') as so, open(os.path.join(B, 'run-r1.stderr'), 'wb') as se:
    began = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=so, stderr=se)
    rc = proc.wait()
    secs = time.monotonic() - began
print(json.dumps({'returncode': rc, 'wall_seconds': secs, 'wall_minutes': secs / 60,
                  'result_exists': os.path.isfile(OUT),
                  'result_bytes': os.path.getsize(OUT) if os.path.isfile(OUT) else None}))
print('--- stderr tail ---')
for line in open(os.path.join(B, 'run-r1.stderr')).read().strip().splitlines()[-15:]:
    print('  ', line)
